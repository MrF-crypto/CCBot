#include "backtest/bt_portfolio.h"
#include "backtest/bt_multisim.h"
#include "core/indicators.h"
#include "core/sr_zones.h"
#include "core/decision.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <map>

namespace ccbot::bt {

std::string PortfolioResult::to_text() const {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << "  期末权益  : " << final_equity << " U   净盈亏 " << total_pnl
      << " U (" << return_pct << "%)\n"
      << "  成本      : 手续费 " << fees << " U   资金费 " << funding << " U\n"
      << "  最大回撤  : " << max_drawdown << " U (" << max_dd_pct << "%)"
      << "   收益/回撤 = " << (max_drawdown > 1e-9 ? total_pnl / max_drawdown : 0) << "\n"
      << "  周期      : " << total_cycles << " 次   胜率 "
      << (total_cycles ? 100.0 * total_wins / total_cycles : 0) << "%   订单 " << total_orders << "\n"
      << "  持仓峰值  : " << peak_positions << " 个品种同时持仓"
      << "   峰值保证金 " << peak_margin << " U   峰值名义 " << peak_notional << " U\n"
      << "  额度拦截  : " << margin_blocks << " 次（总保证金上限挡住的开仓）\n"
      << "  参与品种  : " << symbols_traded << " 个实际开过仓\n";
    return o.str();
}

namespace {
// 高周期状态：closes 末尾恒为"当前未收盘bar"的实时收盘价——与实盘拉K线的
// 语义一致，且避免了每根1m都拷贝整个序列（全市场回测时那是性能杀手）
struct TfState {
    int64_t period = 0, cur_start = -1;
    std::vector<double> closes;      // 末元素 = 当前未收盘bar
    std::vector<Bar>    bars;        // 已收盘bar（SR区域用）
    Bar cur{};
    void feed(const Bar& b) {
        int64_t bucket = b.ts_ms - (b.ts_ms % period);
        if (bucket != cur_start) {
            if (cur_start >= 0) bars.push_back(cur);
            cur_start = bucket; cur = b; cur.ts_ms = bucket;
            closes.push_back(cur.close);                  // 新bar入列
            if (closes.size() > 2400) closes.erase(closes.begin(), closes.begin() + 800);
            if (bars.size()   > 1000) bars.erase(bars.begin(), bars.begin() + 400);
        } else {
            cur.high = std::max(cur.high, b.high);
            cur.low  = std::min(cur.low,  b.low);
            cur.close = b.close;
            cur.quote_vol += b.quote_vol;
            if (!closes.empty()) closes.back() = cur.close;   // 原地更新，零拷贝
        }
    }
    const std::vector<double>& live() const { return closes; }
    std::vector<srzones::Bar> sr(size_t lim) const {
        std::vector<srzones::Bar> o;
        size_t st = bars.size() > lim ? bars.size() - lim : 0;
        for (size_t i = st; i < bars.size(); ++i)
            o.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close, bars[i].quote_vol});
        return o;
    }
};
struct SymState {
    const Series* ser = nullptr;
    size_t idx = 0;                 // 当前读到第几根
    std::string bot_id;
    TfState tf_ind, tf_trend, tf_htf, tf_sr;
    std::vector<srzones::Zone> zones;
    double atr = 0;
    int64_t last_sr = 0, last_slow = 0;
};
} // namespace

PortfolioResult run_portfolio(const std::vector<Series>& all, const PortfolioOptions& opt) {
    PortfolioResult res;
    if (all.empty()) return res;

    // ── 虚拟时钟 + 引擎（一个引擎管全部品种，与实盘同构）────────────────────
    int64_t vnow = 0;
    for (const auto& s : all) if (!s.bars.empty())
        vnow = (vnow == 0) ? s.bars.front().ts_ms : std::min(vnow, s.bars.front().ts_ms);

    EngineHost host;
    host.now_wall   = [&vnow]{ return std::chrono::system_clock::time_point(std::chrono::milliseconds(vnow)); };
    host.now_steady = [&vnow]{ return std::chrono::steady_clock::time_point(std::chrono::milliseconds(vnow)); };
    host.submit     = [](std::function<void()> f){ f(); };

    auto sim = std::make_shared<MultiSimClient>();
    auto eng = std::make_shared<CcgEngine>(sim, host);
    // 账户级总保证金上限：0=按本金90%自动（留10%缓冲给浮亏）
    double cap = opt.max_total_margin > 0 ? opt.max_total_margin : opt.initial_equity * 0.9;
    eng->set_max_total_margin(cap);

    std::map<std::string, SymbolStat> stats;
    eng->set_trade_cb([&](const TradeRecord& tr) {
        ++res.total_cycles;
        if (tr.pnl > 0) ++res.total_wins;
        auto& st = stats[tr.symbol];
        st.symbol = tr.symbol;
        st.pnl += tr.pnl; ++st.cycles;
        if (tr.pnl > 0) ++st.wins;
        st.max_layers = std::max(st.max_layers, tr.layers);
    });
    eng->set_log_cb([&](const std::string& m) {
        if (m.find("总保证金上限") != std::string::npos) ++res.margin_blocks;
    });

    // ── 初始化各品种状态与 bot ──────────────────────────────────────────────
    std::vector<SymState> syms;
    syms.reserve(all.size());
    for (const auto& s : all) {
        if (s.bars.empty()) continue;
        SymState st;
        st.ser = &s;
        auto cfg = opt.base_cfg;
        cfg.symbol = s.symbol;
        cfg.budget_usdt = opt.per_symbol_budget;
        st.bot_id = eng->add_bot(cfg);
        if (st.bot_id.empty()) continue;
        st.tf_ind.period   = interval_ms(cfg.kline_interval);
        st.tf_trend.period = interval_ms(cfg.trend_interval);
        st.tf_htf.period   = interval_ms(cfg.htf_interval);
        st.tf_sr.period    = interval_ms(cfg.sr_interval);
        if (!st.tf_ind.period)   st.tf_ind.period = 3600000;
        if (!st.tf_trend.period) st.tf_trend.period = 14400000;
        if (!st.tf_htf.period)   st.tf_htf.period = 86400000;
        if (!st.tf_sr.period)    st.tf_sr.period = 14400000;
        // 跳到起始时间
        if (opt.start_ms)
            while (st.idx < s.bars.size() && s.bars[st.idx].ts_ms < opt.start_ms) ++st.idx;
        syms.push_back(std::move(st));
    }
    if (syms.empty()) return res;

    const double init_eq = opt.initial_equity;
    double peak_eq = init_eq, equity = init_eq;
    int64_t last_day = 0;
    const int lev = std::max(1, opt.base_cfg.leverage);

    // ── 时间轴合并推进：每分钟处理所有到期的品种 ────────────────────────────
    for (;;) {
        // 找当前最小时间戳
        int64_t t = 0;
        for (const auto& s : syms)
            if (s.idx < s.ser->bars.size()) {
                int64_t ts = s.ser->bars[s.idx].ts_ms;
                if (t == 0 || ts < t) t = ts;
            }
        if (t == 0) break;
        if (opt.end_ms && t > opt.end_ms) break;
        vnow = t;

        for (auto& s : syms) {
            if (s.idx >= s.ser->bars.size()) continue;
            const Bar& b = s.ser->bars[s.idx];
            if (b.ts_ms != t) continue;
            ++s.idx;

            s.tf_ind.feed(b); s.tf_trend.feed(b); s.tf_htf.feed(b); s.tf_sr.feed(b);
            const auto& cfg = opt.base_cfg;

            // 指标
            {
                const auto& c = s.tf_ind.live();
                if ((int)c.size() >= cfg.boll_period + 1) {
                    auto bo = indicators::bollinger(c, cfg.boll_period, cfg.boll_mult);
                    if (bo.ok) eng->update_indicator(s.bot_id, bo.lb, bo.ub,
                                                     indicators::rsi(c, cfg.rsi_period));
                }
            }
            // 趋势 + 日线%B（5分钟一次）
            if (b.ts_ms - s.last_slow >= 300000) {
                s.last_slow = b.ts_ms;
                if (cfg.use_trend_filter) {
                    const auto& c = s.tf_trend.live();
                    if ((int)c.size() >= cfg.trend_ema_period + 4) {
                        double ema = indicators::ema(c, cfg.trend_ema_period);
                        double m0 = indicators::sma_at(c, 20, 0), m3 = indicators::sma_at(c, 20, 3);
                        if (ema > 0 && m3 > 0)
                            eng->update_trend(s.bot_id, c.back() < ema && (m0-m3)/m3*100.0 < -0.2);
                    }
                }
                if (cfg.use_htf_filter) {
                    const auto& c = s.tf_htf.live();
                    if (c.size() >= 21) {
                        auto hb = indicators::bollinger(c, 20, 2.0);
                        if (hb.ok) eng->update_htf(s.bot_id, decision::pct_b(c.back(), hb.lb, hb.ub));
                    }
                }
            }
            // SR区域（15分钟一次）
            if (cfg.sr_radar && b.ts_ms - s.last_sr >= 900000 && s.tf_sr.bars.size() >= 60) {
                s.last_sr = b.ts_ms;
                auto sb = s.tf_sr.sr(400);
                s.zones = srzones::detect_zones(sb);
                s.atr   = srzones::atr(sb, 14);
            }

            // bar内四子tick
            const bool bull = b.close >= b.open;
            const double path[4] = { b.open, bull ? b.low : b.high, bull ? b.high : b.low, b.close };
            for (double px : path) {
                sim->set_market(s.ser->symbol, px, b.spread);
                if (!s.zones.empty()) {
                    auto dg = decision::digest_zones(s.zones, px, cfg.sr_min_confluence);
                    double sl = (dg.deep_sup_lo > 0 && s.atr > 0) ? dg.deep_sup_lo - 0.25*s.atr : 0;
                    eng->update_sr_structure(s.bot_id, dg.at_support, dg.sup_hi, dg.res_lo, sl);
                }
                eng->tick(s.ser->symbol, px);
            }
            if (b.funding != 0) sim->settle_funding(s.ser->symbol, b.funding);
        }

        // 账户级跟踪
        equity = init_eq + sim->realized() + sim->unrealized();
        peak_eq = std::max(peak_eq, equity);
        double dd = peak_eq - equity;
        if (dd > res.max_drawdown) {
            res.max_drawdown = dd;
            res.max_dd_pct = peak_eq > 0 ? dd / peak_eq * 100.0 : 0;
        }
        int op = sim->open_positions();
        res.peak_positions = std::max(res.peak_positions, op);
        double notion = sim->total_notional();
        res.peak_notional = std::max(res.peak_notional, notion);
        res.peak_margin   = std::max(res.peak_margin, notion / lev);

        int64_t day = t / 86400000;
        if (day != last_day) { last_day = day; res.equity_curve.emplace_back(t, equity); }
    }

    res.total_pnl    = sim->realized() + sim->unrealized();
    res.final_equity = init_eq + res.total_pnl;
    res.return_pct   = init_eq > 0 ? res.total_pnl / init_eq * 100.0 : 0;
    res.fees         = sim->fees();
    res.funding      = sim->funding();
    res.total_orders = sim->orders();
    for (auto& [k, v] : stats) res.per_symbol.push_back(v);
    std::sort(res.per_symbol.begin(), res.per_symbol.end(),
              [](const SymbolStat& a, const SymbolStat& b){ return a.pnl > b.pnl; });
    res.symbols_traded = (int)res.per_symbol.size();
    return res;
}

// ── 全市场流式版：内存与品种数无关（每品种只驻留一个小缓冲）──────────────────
PortfolioResult run_portfolio_stream(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& files,
    const PortfolioOptions& opt) {

    PortfolioResult res;
    if (files.empty()) return res;

    struct StreamSym {
        BarStream st;
        std::string sym, bot_id;
        Bar pending{};
        bool has_pending = false;
        TfState tf_ind, tf_trend, tf_htf, tf_sr;
        std::vector<srzones::Zone> zones;
        double atr = 0;
        int64_t last_sr = 0, last_slow = 0;
    };

    int64_t vnow = 0;
    EngineHost host;
    host.now_wall   = [&vnow]{ return std::chrono::system_clock::time_point(std::chrono::milliseconds(vnow)); };
    host.now_steady = [&vnow]{ return std::chrono::steady_clock::time_point(std::chrono::milliseconds(vnow)); };
    host.submit     = [](std::function<void()> f){ f(); };

    auto sim = std::make_shared<MultiSimClient>();
    auto eng = std::make_shared<CcgEngine>(sim, host);
    double cap = opt.max_total_margin > 0 ? opt.max_total_margin : opt.initial_equity * 0.9;
    eng->set_max_total_margin(cap);

    std::map<std::string, SymbolStat> stats;
    eng->set_trade_cb([&](const TradeRecord& tr) {
        ++res.total_cycles;
        if (tr.pnl > 0) ++res.total_wins;
        auto& s = stats[tr.symbol];
        s.symbol = tr.symbol; s.pnl += tr.pnl; ++s.cycles;
        if (tr.pnl > 0) ++s.wins;
        s.max_layers = std::max(s.max_layers, tr.layers);
    });
    eng->set_log_cb([&](const std::string& m) {
        if (m.find("总保证金上限") != std::string::npos) ++res.margin_blocks;
    });

    std::vector<std::unique_ptr<StreamSym>> syms;
    syms.reserve(files.size());
    for (const auto& [sym, paths] : files) {
        auto ss = std::make_unique<StreamSym>();
        if (!ss->st.open(sym, paths)) continue;
        ss->sym = ss->st.symbol();
        auto cfg = opt.base_cfg;
        cfg.symbol = ss->sym;
        cfg.budget_usdt = opt.per_symbol_budget;
        ss->bot_id = eng->add_bot(cfg);
        if (ss->bot_id.empty()) continue;
        ss->tf_ind.period   = interval_ms(cfg.kline_interval);
        ss->tf_trend.period = interval_ms(cfg.trend_interval);
        ss->tf_htf.period   = interval_ms(cfg.htf_interval);
        ss->tf_sr.period    = interval_ms(cfg.sr_interval);
        if (!ss->tf_ind.period)   ss->tf_ind.period = 3600000;
        if (!ss->tf_trend.period) ss->tf_trend.period = 14400000;
        if (!ss->tf_htf.period)   ss->tf_htf.period = 86400000;
        if (!ss->tf_sr.period)    ss->tf_sr.period = 14400000;
        ss->has_pending = ss->st.next(ss->pending);
        if (ss->has_pending) {
            if (vnow == 0 || ss->pending.ts_ms < vnow) vnow = ss->pending.ts_ms;
        }
        syms.push_back(std::move(ss));
    }
    if (syms.empty()) return res;

    const double init_eq = opt.initial_equity;
    double peak_eq = init_eq, equity = init_eq;
    int64_t last_day = 0;
    const int lev = std::max(1, opt.base_cfg.leverage);
    const auto& cfg = opt.base_cfg;

    for (;;) {
        int64_t t = 0;
        for (const auto& s : syms)
            if (s->has_pending && (t == 0 || s->pending.ts_ms < t)) t = s->pending.ts_ms;
        if (t == 0) break;
        if (opt.end_ms && t > opt.end_ms) break;
        vnow = t;

        for (auto& sp : syms) {
            auto& s = *sp;
            if (!s.has_pending || s.pending.ts_ms != t) continue;
            const Bar b = s.pending;
            s.has_pending = s.st.next(s.pending);

            if (opt.start_ms && b.ts_ms < opt.start_ms) continue;

            s.tf_ind.feed(b); s.tf_trend.feed(b); s.tf_htf.feed(b); s.tf_sr.feed(b);

            {
                const auto& c = s.tf_ind.live();
                if ((int)c.size() >= cfg.boll_period + 1) {
                    auto bo = indicators::bollinger(c, cfg.boll_period, cfg.boll_mult);
                    if (bo.ok) eng->update_indicator(s.bot_id, bo.lb, bo.ub,
                                                     indicators::rsi(c, cfg.rsi_period));
                }
            }
            if (b.ts_ms - s.last_slow >= 300000) {
                s.last_slow = b.ts_ms;
                if (cfg.use_trend_filter) {
                    const auto& c = s.tf_trend.live();
                    if ((int)c.size() >= cfg.trend_ema_period + 4) {
                        double ema = indicators::ema(c, cfg.trend_ema_period);
                        double m0 = indicators::sma_at(c, 20, 0), m3 = indicators::sma_at(c, 20, 3);
                        if (ema > 0 && m3 > 0)
                            eng->update_trend(s.bot_id, c.back() < ema && (m0-m3)/m3*100.0 < -0.2);
                    }
                }
                if (cfg.use_htf_filter) {
                    const auto& c = s.tf_htf.live();
                    if (c.size() >= 21) {
                        auto hb = indicators::bollinger(c, 20, 2.0);
                        if (hb.ok) eng->update_htf(s.bot_id, decision::pct_b(c.back(), hb.lb, hb.ub));
                    }
                }
            }
            if (cfg.sr_radar && b.ts_ms - s.last_sr >= 900000 && s.tf_sr.bars.size() >= 60) {
                s.last_sr = b.ts_ms;
                auto sb = s.tf_sr.sr(400);
                s.zones = srzones::detect_zones(sb);
                s.atr   = srzones::atr(sb, 14);
            }

            const bool bull = b.close >= b.open;
            const double path[4] = { b.open, bull ? b.low : b.high, bull ? b.high : b.low, b.close };
            for (double px : path) {
                sim->set_market(s.sym, px, b.spread);
                if (!s.zones.empty()) {
                    auto dg = decision::digest_zones(s.zones, px, cfg.sr_min_confluence);
                    double sl = (dg.deep_sup_lo > 0 && s.atr > 0) ? dg.deep_sup_lo - 0.25*s.atr : 0;
                    eng->update_sr_structure(s.bot_id, dg.at_support, dg.sup_hi, dg.res_lo, sl);
                }
                eng->tick(s.sym, px);
            }
            if (b.funding != 0) sim->settle_funding(s.sym, b.funding);
        }

        equity = init_eq + sim->realized() + sim->unrealized();
        peak_eq = std::max(peak_eq, equity);
        double dd = peak_eq - equity;
        if (dd > res.max_drawdown) {
            res.max_drawdown = dd;
            res.max_dd_pct = peak_eq > 0 ? dd / peak_eq * 100.0 : 0;
        }
        res.peak_positions = std::max(res.peak_positions, sim->open_positions());
        double notion = sim->total_notional();
        res.peak_notional = std::max(res.peak_notional, notion);
        res.peak_margin   = std::max(res.peak_margin, notion / lev);

        int64_t day = t / 86400000;
        if (day != last_day) { last_day = day; res.equity_curve.emplace_back(t, equity); }
    }

    res.total_pnl    = sim->realized() + sim->unrealized();
    res.final_equity = init_eq + res.total_pnl;
    res.return_pct   = init_eq > 0 ? res.total_pnl / init_eq * 100.0 : 0;
    res.fees         = sim->fees();
    res.funding      = sim->funding();
    res.total_orders = sim->orders();
    for (auto& [k, v] : stats) res.per_symbol.push_back(v);
    std::sort(res.per_symbol.begin(), res.per_symbol.end(),
              [](const SymbolStat& a, const SymbolStat& b){ return a.pnl > b.pnl; });
    res.symbols_traded = (int)res.per_symbol.size();
    return res;
}

} // namespace ccbot::bt
