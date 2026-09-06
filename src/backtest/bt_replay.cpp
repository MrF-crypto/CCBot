#include "backtest/bt_replay.h"
#include "backtest/bt_sim.h"
#include "core/indicators.h"
#include "core/sr_zones.h"
#include "core/decision.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace ccbot::bt {

std::string BacktestResult::to_text() const {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << "  净盈亏    : " << total_pnl << " U (" << return_pct << "%)"
      << "   [手续费 " << fees << " / 资金费 " << funding << "]\n"
      << "  最大回撤  : " << max_drawdown << " U (" << max_dd_pct << "%)"
      << "   收益/回撤 = " << std::setprecision(2) << profit_dd() << "\n"
      << "  周期数    : " << cycles << "   胜率 " << win_rate() << "%"
      << "   订单 " << orders << "\n"
      << "  层数      : 平均 " << avg_layers << " / 最大 " << max_layers
      << "   持仓 " << time_in_pos_pct << "%"
      << "   满层 " << full_layer_pct << "%\n"
      << "  峰值名义  : " << max_notional << " U\n"
      << "  三层决策  : 放行 " << gate_pass << " / 宏观拦 " << gate_block_htf
      << " / 结构拦 " << gate_block_sr << "\n";
    return o.str();
}

namespace {

// 高周期序列的滚动维护：只在周期边界重算，避免每个1m都全量算指标
struct TfSeries {
    int64_t period_ms = 0;
    int64_t cur_start = -1;
    std::vector<double> closes;   // 已收盘的收盘价（末尾追加"当前未收盘"的实时值）
    std::vector<Bar>    bars;     // 已收盘的完整bar（SR区域检测用）
    Bar cur{};

    void feed(const Bar& b) {
        int64_t bucket = b.ts_ms - (b.ts_ms % period_ms);
        if (bucket != cur_start) {
            if (cur_start >= 0) { closes.push_back(cur.close); bars.push_back(cur); }
            cur_start = bucket;
            cur = b;
            cur.ts_ms = bucket;
        } else {
            cur.high  = std::max(cur.high, b.high);
            cur.low   = std::min(cur.low,  b.low);
            cur.close = b.close;
            cur.volume    += b.volume;
            cur.quote_vol += b.quote_vol;
        }
    }
    // 含"当前未收盘"bar的收盘价序列——与实盘一致（实盘拉K线最后一根也是未收盘的）
    std::vector<double> closes_live() const {
        auto v = closes;
        if (cur_start >= 0) v.push_back(cur.close);
        return v;
    }
    std::vector<srzones::Bar> sr_bars(size_t limit) const {
        std::vector<srzones::Bar> out;
        size_t start = bars.size() > limit ? bars.size() - limit : 0;
        out.reserve(bars.size() - start);
        for (size_t i = start; i < bars.size(); ++i)
            out.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close, bars[i].quote_vol});
        return out;
    }
};

} // namespace

BacktestResult run_replay(const Series& series, const ReplayOptions& opt) {
    BacktestResult res;
    res.symbol = series.symbol;
    if (series.bars.empty()) return res;

    // ── 虚拟时钟：回放时间由数据驱动，冷却/新鲜度检查全部按模拟时间流逝 ──────
    int64_t vnow_ms = series.bars.front().ts_ms;
    EngineHost host;
    host.now_wall = [&vnow_ms] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    host.now_steady = [&vnow_ms] {
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    host.submit = [](std::function<void()> fn) { fn(); };   // 内联同步=确定性可复现

    SimConfig scfg;
    scfg.leverage = opt.cfg.leverage;
    auto sim = std::make_shared<SimClient>(scfg);
    auto engine = std::make_shared<CcgEngine>(sim, host);

    // 统计钩子
    int layer_sum = 0;
    engine->set_trade_cb([&](const TradeRecord& tr) {
        ++res.cycles;
        if (tr.pnl > 0) ++res.wins;
        layer_sum += tr.layers;
        res.max_layers = std::max(res.max_layers, tr.layers);
    });
    engine->set_log_cb([&](const std::string& msg) {
        if (msg.find("[决策]") != std::string::npos) {
            if (msg.find("首仓派发") != std::string::npos) ++res.gate_pass;
        } else if (msg.find("三层决策拦截") != std::string::npos) {
            if (msg.find("高位") != std::string::npos) ++res.gate_block_htf;
            else ++res.gate_block_sr;
        }
        if (opt.verbose) std::printf("    %s\n", msg.c_str());
    });

    // 宏观涨幅拦截在回测里【未实现】：这里喂 update_htf 时不带涨幅（chg_ok=false），
    // 而引擎在 strict 模式下把"算不出涨幅"等同于"数据没到不开仓"。若配置里带了
    // 非零阈值（从实盘 bots.json 复制过来的场景），回测会一单不开却毫无提示。
    // 强制归零并让它保持静默失效，比静默拦死一切要诚实
    auto bt_cfg = opt.cfg;
    bt_cfg.htf_day_chg_max  = 0;
    bt_cfg.htf_week_chg_max = 0;
    auto bot_id = engine->add_bot(bt_cfg);
    if (bot_id.empty()) return res;

    // ── 高周期序列 ──────────────────────────────────────────────────────────
    TfSeries tf_ind, tf_trend, tf_htf, tf_sr;
    // 多周期梯子的四档：1h / 4h / 12h / 1d（固定，不跟随 kline_interval）
    TfSeries tf_mtf[4];
    { const int64_t P[4] = {3600000LL, 14400000LL, 43200000LL, 86400000LL};
      for (int i = 0; i < 4; ++i) tf_mtf[i].period_ms = P[i]; }
    tf_ind.period_ms   = interval_ms(opt.cfg.kline_interval);
    tf_trend.period_ms = interval_ms(opt.cfg.trend_interval);
    tf_htf.period_ms   = interval_ms(opt.cfg.htf_interval);
    tf_sr.period_ms    = interval_ms(opt.cfg.sr_interval);
    if (tf_ind.period_ms   <= 0) tf_ind.period_ms   = 3600000;
    if (tf_trend.period_ms <= 0) tf_trend.period_ms = 14400000;
    if (tf_htf.period_ms   <= 0) tf_htf.period_ms   = 86400000;
    if (tf_sr.period_ms    <= 0) tf_sr.period_ms    = 14400000;

    // 权益跟踪
    const double init_eq = opt.initial_equity;
    double peak_eq = init_eq, equity = init_eq;
    int64_t last_day = 0, in_pos_min = 0, total_min = 0;
    int64_t full_min = 0;   // 处于满层状态的分钟数
    int64_t last_sr_calc = 0, last_slow_calc = 0;
    // 预热期基线：跨过 stats_from_ms 时拍一次快照，最终统计全部减去它。
    // 预热期内引擎照常运行（指标才能预热），只是不计分
    const int64_t stats_from = opt.stats_from_ms ? opt.stats_from_ms : opt.start_ms;
    bool   scoring   = (stats_from == 0);
    double base_real = 0, base_fees = 0, base_fund = 0;
    int    base_cyc  = 0, base_win = 0, base_ord = 0, base_layer = 0;
    std::vector<srzones::Zone> sr_zones;
    double sr_atr = 0;

    for (const auto& b : series.bars) {
        if (opt.start_ms && b.ts_ms < opt.start_ms) continue;
        if (opt.end_ms   && b.ts_ms > opt.end_ms)   break;

        if (!scoring && stats_from && b.ts_ms >= stats_from) {
            scoring    = true;
            base_real  = sim->realized();
            base_fees  = sim->fees();
            base_fund  = sim->funding_paid();
            base_ord   = sim->order_count();
            base_cyc   = res.cycles;  base_win = res.wins;  base_layer = layer_sum;
            // 权益与回撤从统计起点重新起算，否则预热期的波动会污染测试段回撤
            equity = peak_eq = init_eq + sim->realized() + sim->unrealized();
            res.max_drawdown = 0; res.max_dd_pct = 0;
            res.max_layers = 0; res.max_notional = 0;
            res.gate_pass = res.gate_block_htf = res.gate_block_sr = 0;
            res.equity_curve.clear(); res.equity_days.clear();
        }

        vnow_ms = b.ts_ms;
        tf_ind.feed(b); tf_trend.feed(b); tf_htf.feed(b); tf_sr.feed(b);
        if (opt.cfg.mtf_ladder) for (auto& t : tf_mtf) t.feed(b);

        // ① 指标（每根1m更新，与实盘每3秒刷新等价）
        {
            auto closes = tf_ind.closes_live();
            if ((int)closes.size() >= opt.cfg.boll_period + 1) {
                auto bo = indicators::bollinger(closes, opt.cfg.boll_period, opt.cfg.boll_mult);
                double rsi = indicators::rsi(closes, opt.cfg.rsi_period);
                if (bo.ok) engine->update_indicator(bot_id, bo.lb, bo.ub, rsi);
            }
        }
        // ①b 多周期梯子的四档带值。1h 档每根1m更新（与实盘每3秒刷新等价），
        //    4h/12h/1d 跟慢速批次（实盘也是几分钟才拉一次高周期K线）
        if (opt.cfg.mtf_ladder) {
            for (int t = 0; t < 4; ++t) {
                if (t > 0 && b.ts_ms - last_slow_calc < 300000) continue;
                auto c = tf_mtf[t].closes_live();
                if ((int)c.size() < opt.cfg.boll_period + 1) continue;
                auto bo = indicators::bollinger(c, opt.cfg.boll_period, opt.cfg.boll_mult);
                if (bo.ok) engine->update_mtf_band(bot_id, t, bo.lb, bo.ub);
            }
        }

        // ② 趋势 + 日线%B（每5分钟，与实盘同频）
        if (b.ts_ms - last_slow_calc >= 300000) {
            last_slow_calc = b.ts_ms;
            if (opt.cfg.use_trend_filter) {
                auto tc = tf_trend.closes_live();
                if ((int)tc.size() >= opt.cfg.trend_ema_period + 4) {
                    double ema = indicators::ema(tc, opt.cfg.trend_ema_period);
                    double mb_now = indicators::sma_at(tc, 20, 0);
                    double mb_prev = indicators::sma_at(tc, 20, 3);
                    if (ema > 0 && mb_prev > 0) {
                        double slope = (mb_now - mb_prev) / mb_prev * 100.0;
                        engine->update_trend(bot_id, tc.back() < ema && slope < -0.2);
                    }
                }
            }
            if (opt.cfg.use_htf_filter) {
                auto hc = tf_htf.closes_live();
                if (hc.size() >= 21) {
                    auto hb = indicators::bollinger(hc, 20, 2.0);
                    if (hb.ok) engine->update_htf(bot_id,
                                                  decision::pct_b(hc.back(), hb.lb, hb.ub));
                }
            }
        }
        // ③ SR区域（每15分钟重算，与实盘同频）
        if (opt.cfg.sr_radar && b.ts_ms - last_sr_calc >= 900000 && tf_sr.bars.size() >= 60) {
            last_sr_calc = b.ts_ms;
            auto sb = tf_sr.sr_bars(400);
            sr_zones = srzones::detect_zones(sb);
            sr_atr   = srzones::atr(sb, 14);
        }

        // ── bar内四子tick走价：阳线 O→L→H→C，阴线 O→H→L→C ─────────────────
        const bool bull = b.close >= b.open;
        const double path[4] = { b.open,
                                 bull ? b.low  : b.high,
                                 bull ? b.high : b.low,
                                 b.close };
        for (double px : path) {
            sim->set_market(series.symbol, px, b.spread);
            sim->set_now(b.ts_ms);
            // 结构摘要按当前价实时提炼（与实盘每tick做的完全一致）
            if (!sr_zones.empty()) {
                decision::DigestOpts dop; dop.min_conf = opt.cfg.sr_min_confluence;
                dop.res_min_conf = opt.cfg.sr_res_min_conf;
                dop.independent_conf = opt.cfg.sr_independent_conf;
                dop.lower_half_only  = opt.cfg.sr_lower_half_only;
                auto dg = decision::digest_zones(sr_zones, px, dop);
                double stop_lv = (dg.deep_sup_lo > 0 && sr_atr > 0)
                                 ? dg.deep_sup_lo - 0.25 * sr_atr : 0;
                engine->update_sr_structure(bot_id, dg.at_support, dg.sup_hi, dg.res_lo, stop_lv);
            }
            engine->tick(series.symbol, px);
        }

        // ④ 资金费结算
        if (b.funding != 0) sim->settle_funding(b.funding);

        // ⑤ 权益与回撤跟踪（预热期不计分）
        if (!scoring) continue;
        equity = init_eq + sim->realized() + sim->unrealized();
        peak_eq = std::max(peak_eq, equity);
        double dd = peak_eq - equity;
        if (dd > res.max_drawdown) {
            res.max_drawdown = dd;
            res.max_dd_pct   = peak_eq > 0 ? dd / peak_eq * 100.0 : 0;
        }
        res.max_notional = std::max(res.max_notional, sim->position_qty() * b.close);
        ++total_min;
        if (sim->position_qty() > 0) ++in_pos_min;
        // 满层 = 梯子用尽：此后既不能再摊薄，也只能干等价格回来
        for (const auto& bt : engine->get_bots())
            if ((int)bt.entries.size() >= bt.cfg.max_entries) { ++full_min; break; }
        // 按天采样权益曲线
        int64_t day = b.ts_ms / 86400000;
        if (day != last_day) {
            last_day = day;
            res.equity_curve.push_back(equity);
            res.equity_days.emplace_back(b.ts_ms, equity);
        }
    }

    res.total_pnl  = (sim->realized() - base_real) + sim->unrealized();
    res.return_pct = init_eq > 0 ? res.total_pnl / init_eq * 100.0 : 0;
    res.fees       = sim->fees()         - base_fees;
    res.funding    = sim->funding_paid() - base_fund;
    res.orders     = sim->order_count()  - base_ord;
    res.cycles    -= base_cyc;
    res.wins      -= base_win;
    layer_sum     -= base_layer;
    res.avg_layers = res.cycles ? (double)layer_sum / res.cycles : 0;
    res.time_in_pos_pct = total_min ? 100.0 * in_pos_min / total_min : 0;
    res.full_layer_pct  = total_min ? 100.0 * full_min   / total_min : 0;
    return res;
}

} // namespace ccbot::bt
