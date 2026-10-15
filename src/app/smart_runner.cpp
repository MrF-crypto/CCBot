#include "app/smart_runner.h"
#include "core/parallel.h"
#include "net/trading_client.h"

#include <simdjson.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace ccbot {

namespace {

constexpr int64_t kHourMs = 3600LL * 1000;

std::string fmt(double v, int prec) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}
// 价格：有效数字 8 位（山寨币价格从 0.00001 到几百都有，固定小数位不合适）
std::string fmt_px(double v) {
    std::ostringstream o;
    o << std::setprecision(8) << v;
    return o.str();
}

// 交易所K线 → 智能模式K线，去掉最后一根（未收盘）
std::vector<smart::Bar> closed_bars(const std::vector<TradingClient::Bar>& in) {
    std::vector<smart::Bar> v;
    if (in.size() < 2) return v;
    v.reserve(in.size() - 1);
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        const auto& b = in[i];
        v.push_back({b.open_ms, b.open, b.high, b.low, b.close, b.volume, b.quote_volume});
    }
    return v;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

// 名单 / 分段代码 → 日志里的简称
std::string list_name(char c) {
    switch (c) {
    case smart::kListLong:   return "多";
    case smart::kListShort:  return "空";
    case smart::kSegStrong:  return "强";
    case smart::kSegWeak:    return "弱";
    case smart::kSegMid:     return "中";
    case smart::kSegHot:     return "过热";
    default:                 return "?";
    }
}

} // namespace

std::vector<SmartRunner::Group> SmartRunner::build_groups(const smart::Ranking& rk,
                                                          const smart::Segmented& sg,
                                                          const smart::Config& cfg,
                                                          const std::set<std::string>& with_bot) {
    std::vector<Group> out;
    auto make = [&](const std::string& title, char code, const std::vector<std::string>& list,
                    int quota, auto score_of) {
        Group g;
        g.title = title;
        g.code  = code;
        g.quota = quota;
        for (int i = 0; i < (int)list.size() && i < quota; ++i) {
            const auto& s = rk.rows.at(list[i]);
            Row r;
            r.symbol    = list[i];
            r.list      = code;
            r.rank      = i + 1;
            r.score     = score_of(s);
            r.atr_pct   = s.f.atr_pct;
            r.box12_atr = s.f.box12_atr;
            r.vol_lift  = s.f.vol_lift;
            r.rs        = s.f.rs;
            r.max_up24  = s.f.max_up24;
            r.has_bot   = with_bot.count(list[i]) > 0;
            g.rows.push_back(std::move(r));
        }
        out.push_back(std::move(g));
    };
    if (cfg.dir_mode == smart::DirMode::Mixed) {
        using smart::Scored;
        const auto mid_dir = cfg.dir_long && cfg.dir_short ? "多空" : (cfg.dir_long ? "仅多" : "仅空");
        if (smart::seg_quota(smart::kSegStrong, cfg) > 0)
            make("强势（仅多）", smart::kSegStrong, sg.strong, cfg.strong_n,
                 [](const Scored& s) { return s.long_score; });
        if (smart::seg_quota(smart::kSegWeak, cfg) > 0)
            make("弱势（仅空）", smart::kSegWeak, sg.weak, cfg.weak_n,
                 [](const Scored& s) { return s.short_score; });
        if (smart::seg_quota(smart::kSegMid, cfg) > 0)
            make(std::string("中间（") + mid_dir + "）", smart::kSegMid, sg.mid, cfg.mid_n,
                 [](const Scored& s) { return s.squeeze_score; });
        if (smart::seg_quota(smart::kSegHot, cfg) > 0)
            make("过热（仅空）", smart::kSegHot, sg.hot, cfg.hot_n,
                 [](const Scored& s) { return s.short_score; });
    } else {
        if (cfg.dir_long)
            make("做多", smart::kListLong, rk.longs, cfg.long_n,
                 [](const smart::Scored& s) { return s.long_score; });
        if (cfg.dir_short)
            make("做空", smart::kListShort, rk.shorts, cfg.short_n,
                 [](const smart::Scored& s) { return s.short_score; });
    }
    return out;
}

// ── 全市场扫描 ──────────────────────────────────────────────────────────────

namespace {

// 不需要K线的那一半：交易规则 + 24h 行情 + 资金费率 → 过硬门槛的候选。海龟与裸K 共用。
// 池里的币（pool_syms）走放宽门槛，没拿到数据 / 不合格 / 即将下架分别记进 out。
// 返回 false = 地基没拿到，整轮作废（r.error 已填）。
// coin_only：只要标的类型是 COIN 的（裸K：不埋伏大宗商品、股票、指数），黄金代币另排
bool gate_universe(TradingClient& c, const smart::Config& cfg,
                   const std::set<std::string>& pool_syms, int64_t now_ms,
                   MarketScan& r, std::vector<std::string>& cand, bool coin_only = false) {
    auto& out = r.outcome;

    // 交易规则与 24h 行情是整轮的地基，拿不到就整轮作废——绝不能把"没拿到"
    // 当成"池里的币都不合格"去删 bot
    bool ok_meta = false, ok_t24 = false, ok_fund = false;
    // 失败时带上网络层的原因（超时 / 连接中断 / 返回的不是 JSON……）：同一线程、紧跟着请求读
    auto net_cause = [&c]() {
        const auto e = c.last_error();
        return e.empty() ? std::string() : "：" + e;
    };
    const auto metas = c.fetch_exchange_symbols(&ok_meta);
    if (!ok_meta) { r.error = "交易规则拉取失败" + net_cause(); return false; }
    const auto t24 = c.fetch_all_24h(&ok_t24);
    if (!ok_t24) { r.error = "24h 行情拉取失败" + net_cause(); return false; }
    // 资金费率拿不到不致命：这一轮不按费率判，其余照常
    const auto fund = c.fetch_all_funding_pct(&ok_fund);

    std::set<std::string> seen;
    for (const auto& mm : metas) {
        smart::SymbolMeta m;
        m.symbol = mm.symbol; m.base = mm.base; m.quote = mm.quote;
        m.contract_type = mm.contract_type; m.status = mm.status;
        m.onboard_ms = mm.onboard_ms; m.delivery_ms = mm.delivery_ms; m.tick = mm.tick;
        m.underlying_type = mm.underlying_type;
        if (m.quote != "USDT" || m.contract_type != "PERPETUAL") continue;
        ++r.universe;
        seen.insert(m.symbol);
        if (coin_only && ((!m.underlying_type.empty() && m.underlying_type != "COIN") ||
                          smart::ambush_excluded_base(m.base)))
            continue;
        const bool member = pool_syms.count(m.symbol) > 0;
        if (member && smart::delisting(m, now_ms)) out.delisting.insert(m.symbol);

        const auto it = t24.find(m.symbol);
        if (it == t24.end()) {                 // 这一个没拿到：池里的归为未知
            if (member) out.unknown.insert(m.symbol);
            continue;
        }
        const smart::Ticker24 tv{it->second.quote_vol, it->second.chg_pct, it->second.last};
        const double* fp = nullptr;
        double fv = 0;
        if (ok_fund)
            if (auto f = fund.find(m.symbol); f != fund.end()) { fv = f->second; fp = &fv; }

        const auto why = smart::pre_gate(m, &tv, fp, cfg, now_ms, member);
        if (!why.empty()) {
            if (member) out.hard_fail[m.symbol] = why;
            continue;
        }
        ++r.passed_gate;
        cand.push_back(m.symbol);
    }
    // 交易规则这次【是拿到了的】，池里的币不在里面 = 合约确实没了
    for (const auto& s : pool_syms)
        if (!seen.count(s)) out.hard_fail[s] = "交易所已无此 USDT 永续合约";
    return true;
}

} // namespace

MarketScan scan_market(TradingClient& c, const smart::Config& cfg,
                       const std::set<std::string>& pool_syms, int64_t now_ms, int parallel) {
    MarketScan r;
    auto& out = r.outcome;
    std::vector<std::string> cand;
    if (!gate_universe(c, cfg, pool_syms, now_ms, r, cand)) return r;

    // 201 根 1h：去掉未收盘那根剩 200，够 7 天相对强弱（168）与 ATR100。权重 2
    const auto btc = closed_bars(c.fetch_klines("BTCUSDT", "1h", 201));

    std::vector<smart::Features> feats(cand.size());
    std::vector<char> got(cand.size(), 0);
    parallel_for((int)cand.size(), parallel, [&](int i) {
        try {
            const auto bars = closed_bars(c.fetch_klines(cand[i], "1h", 201));
            if ((int)bars.size() < smart::kMinBars) return;
            feats[i] = smart::compute_features(bars, btc);
            got[i] = 1;
        } catch (...) {}
    });

    int fetched = 0;
    std::map<std::string, smart::Features> eligible;
    for (size_t i = 0; i < cand.size(); ++i) {
        const auto& s = cand[i];
        const bool member = pool_syms.count(s) > 0;
        if (got[i]) ++fetched;
        if (!got[i] || !feats[i].ok) {
            if (member) out.unknown.insert(s);
            continue;
        }
        const auto why = smart::atr_gate(feats[i], cfg, member);
        if (!why.empty()) {
            if (member) out.hard_fail[s] = why;
            continue;
        }
        eligible[s] = feats[i];
    }
    // K线大面积拉不到：百分位是在一个残缺的集合上算的，名单不可信，整轮作废
    if (!cand.empty() && fetched * 10 < (int)cand.size() * 7) {
        r.error = "K线拉取失败过多（" + std::to_string(fetched) + "/" +
                  std::to_string(cand.size()) + "）";
        return r;
    }
    r.eligible = (int)eligible.size();
    // 全市场一个合格的都没有：正常行情不会这样，多半是某个字段解析成了 0
    // （交易所改了格式）。当成数据异常，而不是"今天没有好币"去淘汰整个池子
    if (r.universe > 0 && r.eligible == 0) {
        r.error = "全市场合格品种为 0（门槛通过 " + std::to_string(r.passed_gate) +
                  "），疑似数据异常";
        return r;
    }
    out.ranking = smart::rank(eligible, cfg);
    out.ok = true;
    return r;
}

std::map<std::string, smart::AmbushFeat> fetch_ambush(TradingClient& c,
                                                      const std::vector<std::string>& syms,
                                                      int parallel) {
    std::vector<smart::AmbushFeat> feats(syms.size());
    std::vector<char> got(syms.size(), 0);
    parallel_for((int)syms.size(), parallel, [&](int i) {
        try {
            // 201 根：去掉未收盘那根剩 200，够 ATR100（ATR 压缩比）。权重 2
            const auto bars = closed_bars(c.fetch_klines(syms[i], "1h", 201));
            if (bars.empty()) return;
            feats[i] = smart::compute_ambush(bars);
            got[i] = 1;
        } catch (...) {}
    });
    std::map<std::string, smart::AmbushFeat> out;
    for (size_t i = 0; i < syms.size(); ++i)
        if (got[i]) out[syms[i]] = feats[i];
    return out;
}

BareScan scan_ambush(TradingClient& c, const smart::Config& cfg, int64_t now_ms, int parallel) {
    BareScan r;
    MarketScan g;
    std::vector<std::string> cand;
    // 裸K 自己的硬门槛（成交额 / 上线天数 / 24h 涨跌）；资金费率、价格精度、拉黑与海龟共用
    smart::Config gc = cfg;
    gc.min_quote_vol = cfg.bare.min_quote_vol;
    gc.min_age_days  = cfg.bare.min_age_days;
    gc.max_abs_chg24 = cfg.bare.max_abs_chg24;
    if (!gate_universe(c, gc, {}, now_ms, g, cand, /*coin_only*/ true)) { r.error = g.error; return r; }
    r.universe    = g.universe;
    r.passed_gate = g.passed_gate;
    r.feats   = fetch_ambush(c, cand, parallel);
    r.fetched = (int)r.feats.size();
    // K线大面积拉不到：池子会缺一大块，这一轮不可信（沿用旧池）
    if (!cand.empty() && r.fetched * 10 < (int)cand.size() * 7) {
        r.error = "K线拉取失败过多（" + std::to_string(r.fetched) + "/" +
                  std::to_string(cand.size()) + "）";
        return r;
    }
    if (r.universe > 0 && r.fetched == 0) {
        r.error = "全市场合格品种为 0（门槛通过 " + std::to_string(r.passed_gate) + "），疑似数据异常";
        return r;
    }
    r.ok = true;
    return r;
}

double sum_day_realized(const std::vector<TrendTrade>& trades, int64_t utc_day) {
    double r = 0;
    for (const auto& t : trades) {
        if (t.close_time.time_since_epoch().count() == 0) continue;
        if (TrendEngine::utc_day(t.close_time) != utc_day) continue;
        const double fee = (std::fabs(t.entry_price) + std::fabs(t.exit_price)) *
                           std::fabs(t.qty) * TrendEngine::kEstFeeRate;
        if (std::isfinite(t.pnl) && std::isfinite(fee)) r += t.pnl - fee;
    }
    return r;
}

// ── SmartRunner ────────────────────────────────────────────────────────────

void SmartRunner::seed_day_from_history(const std::vector<TrendTrade>& trades) {
    if (!engine_) return;
    const auto d = engine_->day_pnl();
    if (d.realized != 0) return;   // 状态文件里有今天的数，以它为准
    const double r = sum_day_realized(trades, d.day);
    if (r != 0) {
        engine_->restore_day_pnl({d.day, r});
        if (log_) log_("智能模式：按成交历史补算今天（UTC）已实现 " + fmt(r, 2) + "U（含估算手续费）", false);
    }
}

void SmartRunner::cancel() { cancelled_.store(true); }

SmartRunner::SmartRunner(std::shared_ptr<TradingClient> client, std::shared_ptr<TrendEngine> engine,
                         std::string state_path, LogFn log)
    : client_(std::move(client)), engine_(std::move(engine)),
      state_path_(std::move(state_path)), log_(std::move(log)) {}

void SmartRunner::set_config(const smart::Config& cfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string before = smart::config_to_json(cfg_);
    cfg_ = cfg;
    // 任何变化都立刻重扫一次：门槛、方向、模板变了，名单和 bot 参数都要跟上。
    // 裸K 的埋伏池平时一天建一次，改了配置也立刻按新规则重建
    if (smart::config_to_json(cfg_) != before) {
        next_due_ms_ = 0;
        built_day_   = -1;
    }
    apply_engine_gates_locked();
}

smart::Config SmartRunner::config() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

void SmartRunner::apply_engine_gates_locked() {
    if (!engine_) return;
    // 暂停开新仓是全局应急开关，智能模式关着也照样生效
    engine_->set_pause_new(cfg_.pause_new);
    if (cfg_.enabled) {
        const bool bare = (cfg_.strategy == smart::Strategy::BareK);
        engine_->set_max_open_positions(bare ? cfg_.bare.max_positions : cfg_.max_positions);
        engine_->set_daily_loss_limit(cfg_.daily_loss_limit);
        engine_->set_float_loss_limit(cfg_.float_loss_limit);
        // 裸K 的 bot 没有综合分（确认放量就开），"名额紧张只收高分"对它不适用
        if (bare) engine_->set_priority_reserve(0, 0);
        else      engine_->set_priority_reserve(cfg_.prio_reserve, cfg_.prio_min_score);
    } else {
        // 关闭时撤掉智能模式的闸门。同时持仓上限交还应用层自己的设置
        // （应用层在 set_config 之后会重新设一次）
        engine_->set_daily_loss_limit(0);
        engine_->set_float_loss_limit(0);
        engine_->set_priority_reserve(0, 0);
    }
}

bool SmartRunner::due(int64_t now_ms) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_.enabled && now_ms >= next_due_ms_;
}

void SmartRunner::force() {
    std::lock_guard<std::mutex> lk(mtx_);
    next_due_ms_ = 0;
}

SmartRunner::Report SmartRunner::last_report() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_;
}

bool SmartRunner::same_params(const TrendConfig& a, const TrendConfig& b) {
    const auto& x = a.rule;
    const auto& y = b.rule;
    return a.interval == b.interval && a.budget_usdt == b.budget_usdt &&
           a.leverage == b.leverage && a.size_mode == b.size_mode && a.risk_usdt == b.risk_usdt &&
           a.signal_max_age_sec == b.signal_max_age_sec &&
           a.use_disaster_stop == b.use_disaster_stop &&
           a.disaster_stop_buffer_pct == b.disaster_stop_buffer_pct &&
           x.strategy == y.strategy && x.direction == y.direction && x.reverse == y.reverse &&
           x.donchian_period == y.donchian_period && x.atr_period == y.atr_period &&
           x.atr_mult == y.atr_mult && x.squeeze_box_atr == y.squeeze_box_atr &&
           x.trend_er_min == y.trend_er_min && x.breakout_vol_ratio == y.breakout_vol_ratio &&
           x.lock_atr == y.lock_atr && x.accel_exit == y.accel_exit &&
           x.vol_paced == y.vol_paced && x.fail_exit == y.fail_exit &&
           x.fail_exit_short == y.fail_exit_short &&
           x.roi_trail == y.roi_trail && x.roi_trail_start == y.roi_trail_start &&
           x.roi_trail_back == y.roi_trail_back;
}

SmartRunner::Report SmartRunner::run_once(int64_t now_ms) {
    std::unique_lock<std::mutex> rl(run_mtx_, std::try_to_lock);
    if (!rl.owns_lock()) return last_report();   // 上一轮还在跑

    const smart::Config cfg = config();
    Report rep;
    rep.at_ms = now_ms;
    if (cancelled_.load()) return rep;
    if (!cfg.enabled || !engine_ || !client_) { housekeep(now_ms); return rep; }
    if (cfg.strategy == smart::Strategy::BareK) return run_bare_once(now_ms, cfg);

    // 只跑一套：裸K 还有持仓就不启动（埋伏池已清、空仓的裸K bot 已删），一分钟后再看
    if (blocked_by_foreign(retire_foreign(smart::Strategy::Turtle, now_ms), smart::Strategy::Turtle)) {
        std::lock_guard<std::mutex> lk(mtx_);
        next_due_ms_ = now_ms + 60 * 1000;
        rep.summary  = "智能模式：等裸K 持仓走完";
        last_ = rep;
        return rep;
    }

    const auto bots = engine_->get_bots();
    std::set<std::string> pool_syms;
    std::vector<smart::BotView> views;
    views.reserve(bots.size());
    for (const auto& b : bots) {
        // 裸K 留下、还在走完的 bot 不归这里管（不进候选池的评判，也不会被换参数）
        if (b.is_auto && b.smart_list == smart::kSegBare) continue;
        views.push_back({b.bot_id, b.cfg.symbol, b.is_auto,
                         b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending,
                         b.state == TrendBot::State::Stopped});
        if (b.is_auto) pool_syms.insert(b.cfg.symbol);
    }

    const auto scan = scan_market(*client_, cfg, pool_syms, now_ms);
    // 扫描要几十秒，期间这个运行器可能已经被作废（断线重连换了新的）：不落地
    if (cancelled_.load()) return rep;
    {
        // 下一次：下一个整点后 1 分钟（等 1h K线收盘）
        std::lock_guard<std::mutex> lk(mtx_);
        next_due_ms_ = (now_ms / kHourMs + 1) * kHourMs + 60 * 1000;
    }
    rep.universe    = scan.universe;
    rep.passed_gate = scan.passed_gate;
    rep.eligible    = scan.eligible;
    if (!scan.outcome.ok) {
        int streak = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            streak = ++fail_streak_;
        }
        rep.summary = "智能模式：本轮扫描失败（" + scan.error + "），候选池不动，5 分钟后重试" +
                      (streak > 1 ? "（已连续 " + std::to_string(streak) + " 轮）" : std::string());
        // 只有第一轮失败走告警（会推 webhook）；5 分钟一试，断网时会连着失败，之后每小时提一次
        if (log_ && (streak == 1 || streak % 12 == 0)) log_(rep.summary, streak == 1);
        std::lock_guard<std::mutex> lk(mtx_);
        // 扫描失败不等下个整点：经代理时多半是一次瞬时故障（实盘"交易规则拉取失败"），
        // 空等一小时池子就旧了一小时
        next_due_ms_ = now_ms + 5 * 60 * 1000;
        last_ = rep;
        return rep;
    }
    {
        int was = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            was = fail_streak_;
            fail_streak_ = 0;
        }
        if (was > 0 && log_)
            log_("智能模式：扫描已恢复（此前连续失败 " + std::to_string(was) + " 轮）", false);
    }

    smart::Plan plan;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        plan = pool_.plan(scan.outcome, views, cfg, now_ms);
    }

    // ── 移出 ──
    std::set<std::string> gone_ids;
    std::vector<std::string> rm_desc;
    for (const auto& r : plan.remove) {
        // 扫描期间它可能刚好开了仓：只删空仓的，开了仓的这一轮放过
        if (!engine_->remove_if_flat(r.bot_id)) continue;
        gone_ids.insert(r.bot_id);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pool_.on_removed(r.symbol);
        }
        ++rep.removed;
        rm_desc.push_back(r.symbol + "（" + r.why + "）");
        if (journal_) journal_->pool(now_ms, r.symbol, "移出", 0, 0, 0, r.why);
        if (on_symbol_removed) on_symbol_removed(r.symbol);
    }

    // ── 名单信息 ──
    for (const auto& m : plan.meta)
        if (!gone_ids.count(m.bot_id)) engine_->set_smart_meta(m.bot_id, m.score, m.rank, m.list);

    // ── 参数同步：方向（混合模式下随分段变）或模板改了，空仓的自动 bot 换上新参数 ──
    // 方向取本轮决策给的；这一轮没拿到数据（没有 meta）的不动
    // 段也要带上：过热段的参数和模板不同（不要求量比）
    std::map<std::string, std::pair<trend::Direction, char>> want_dir;
    for (const auto& m : plan.meta)
        if (m.has_dir) want_dir[m.bot_id] = {m.dir, m.list};
    int resynced = 0, redirected = 0;
    for (const auto& b : bots) {
        if (!b.is_auto || gone_ids.count(b.bot_id)) continue;
        if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending) continue;
        if (b.state == TrendBot::State::Stopped) continue;
        const auto wd = want_dir.find(b.bot_id);
        if (wd == want_dir.end()) continue;
        const auto want = smart::make_bot_config(cfg, b.cfg.symbol, wd->second.first,
                                                 wd->second.second);
        if (same_params(b.cfg, want)) continue;
        // ⚠ replace_bot 在 id 不存在时会【新建】——上面已排除本轮删掉的；
        //   它自己会拒绝有持仓/在途订单的
        if (engine_->replace_bot(b.bot_id, want).empty()) continue;
        ++resynced;
        if (b.cfg.rule.direction != want.rule.direction) {
            ++redirected;
            if (journal_)
                journal_->pool(now_ms, b.cfg.symbol, "换方向", wd->second.second, 0, 0,
                               std::string(trend::direction_name(b.cfg.rule.direction)) + " → " +
                               trend::direction_name(want.rule.direction));
        }
    }

    // ── 补位 ──
    std::vector<std::string> add_desc;
    for (const auto& a : plan.add) {
        const auto id = engine_->add_bot(smart::make_bot_config(cfg, a.symbol, a.dir, a.list), true);
        if (id.empty()) continue;   // 同品种已有 bot（扫描期间被手动加了）
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pool_.on_added(a.symbol, now_ms, a.list);
        }
        engine_->set_smart_meta(id, a.score, a.rank, a.list);
        ++rep.added;
        add_desc.push_back(a.symbol + "（" + list_name(a.list) + " #" +
                           std::to_string(a.rank) + " " + fmt(a.score, 0) + "分）");
        if (journal_)
            journal_->pool(now_ms, a.symbol, "加入", a.list, a.rank, a.score,
                           trend::direction_name(a.dir));
        if (on_symbol_added) on_symbol_added(a.symbol);
    }

    for (const auto& s : plan.alerts)
        if (log_) log_("⚠ " + s, true);

    // ── 汇总 ──
    std::set<std::string> with_bot;
    for (const auto& b : engine_->get_bots()) {
        with_bot.insert(b.cfg.symbol);
        if (!b.is_auto) continue;
        ++rep.monitored;
        if (b.qty > 0) ++rep.holding;
    }
    rep.groups = build_groups(scan.outcome.ranking, plan.seg, cfg, with_bot);
    // 数据记录层：完整名单快照（全部合格币，不只是进名单的）
    if (journal_) journal_->scan(now_ms, scan.outcome.ranking, plan.seg, with_bot);
    rep.ok = true;
    std::string lists;
    for (const auto& g : rep.groups) {
        std::string name = g.title.substr(0, g.title.find("（"));
        lists += "，" + name + " " + std::to_string(g.rows.size());
    }
    rep.summary = "智能模式：合格 " + std::to_string(rep.eligible) + "/" + std::to_string(rep.universe) +
                  lists +
                  "；+" + std::to_string(rep.added) + " −" + std::to_string(rep.removed) +
                  "，监控 " + std::to_string(rep.monitored) + "（持仓 " + std::to_string(rep.holding) + "）";
    if (log_) {
        log_(rep.summary, false);
        if (!add_desc.empty()) log_("  新增：" + join(add_desc, "、"), false);
        if (!rm_desc.empty())  log_("  移出：" + join(rm_desc, "、"), false);
        if (resynced > 0)
            log_("  " + std::to_string(resynced) + " 个空仓自动 bot 已换上新参数" +
                 (redirected > 0 ? "（其中 " + std::to_string(redirected) + " 个随分段换了方向）"
                                 : std::string()), false);
        const auto fs = engine_->take_filter_summary();
        if (!fs.empty())       log_("  上一轮以来自动 bot：" + fs, false);
    }

    std::lock_guard<std::mutex> lk(mtx_);
    last_ = rep;
    save_locked();
    return rep;
}

void SmartRunner::housekeep(int64_t now_ms) {
    if (!engine_ || cancelled_.load()) return;
    bool enabled = false;
    smart::Strategy strat = smart::Strategy::Turtle;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        enabled = cfg_.enabled;
        strat   = cfg_.strategy;
    }
    // 开着：另一套留下、已经走完的 bot 每分钟清一次（海龟一小时才扫一轮，不能等到那时）
    if (enabled) retire_foreign(strat, now_ms);
    if (!enabled) {
        clear_seeds();
        // 关闭后：空仓的自动 bot 移除；持仓中的标待移出（管到平仓、不再开新仓），
        // 走完后下一次维护再移除。已停止的（你或对账停的）不替你删
        for (const auto& b : engine_->get_bots()) {
            if (!b.is_auto || b.state == TrendBot::State::Stopped) continue;
            if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending) {
                if (!b.retiring) engine_->set_retiring(b.bot_id);
                continue;
            }
            if (!engine_->remove_if_flat(b.bot_id)) continue;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                pool_.on_removed(b.cfg.symbol);
            }
            if (on_symbol_removed) on_symbol_removed(b.cfg.symbol);
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    save_locked();
}

// ── 智能裸K ─────────────────────────────────────────────────────────────────

namespace {
constexpr int64_t kBar5Ms = 5 * 60 * 1000;
constexpr int64_t kDayMs  = 24 * kHourMs;
// 建好、点火开仓的裸K bot 在这么久之内还是空仓 = 没开成（被风控闸门拦了、下单失败）
// 或已经平仓：删掉，币留在埋伏池，之后还能再点火
constexpr int64_t kBareIdleMs = 60 * 1000;
// UTC 日序号。币安日K 在 UTC 0 点切换（北京 08:00），埋伏池按它每天重建
int64_t day_of(int64_t ms) { return ms / kDayMs; }
}

int smart_holdings(const std::vector<TrendBot>& bots, smart::Strategy s) {
    int n = 0;
    for (const auto& b : bots) {
        if (!b.is_auto) continue;
        const bool is_bare = (b.smart_list == smart::kSegBare);
        if (is_bare != (s == smart::Strategy::BareK)) continue;
        if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending) ++n;
    }
    return n;
}

int SmartRunner::retire_foreign(smart::Strategy keep, int64_t now_ms) {
    if (!engine_) return 0;
    int holding = 0;
    for (const auto& b : engine_->get_bots()) {
        if (!b.is_auto) continue;
        const bool is_bare = (b.smart_list == smart::kSegBare);
        if (is_bare == (keep == smart::Strategy::BareK)) {
            // 是这一套自己的。关掉又用同一套打开时，关闭期间标的「待移出」撤掉。
            // ⚠ 裸K 的 bot 例外：它们【一直】是待移出（自己不开仓，只由点火判定开仓）
            if (b.retiring && !is_bare) engine_->set_retiring(b.bot_id, false);
            continue;
        }
        if (b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending) {
            // 持仓中（含已停止的）：管到平仓，平仓后不再开新仓；名次清零 = 界面上标「待移出」
            ++holding;
            if (!b.retiring) engine_->set_retiring(b.bot_id);
            if (b.smart_rank != 0) engine_->set_smart_meta(b.bot_id, b.smart_score, 0, b.smart_list);
            continue;
        }
        if (b.state == TrendBot::State::Stopped) continue;   // 已停止的（你或对账停的）不替你删
        if (!engine_->remove_if_flat(b.bot_id)) continue;
        if (journal_) journal_->pool(now_ms, b.cfg.symbol, "移出", b.smart_list, 0, 0, "切换策略");
        if (on_symbol_removed) on_symbol_removed(b.cfg.symbol);
    }
    // 另一套的选币数据不留：海龟的候选池（加入时间、连续掉出次数、分段历史）/ 裸K 的埋伏池
    if (keep == smart::Strategy::BareK) {
        std::lock_guard<std::mutex> lk(mtx_);
        pool_ = smart::Pool{};
    } else {
        clear_seeds();
    }
    return holding;
}

bool SmartRunner::blocked_by_foreign(int holding, smart::Strategy keep) {
    const std::string mine  = keep == smart::Strategy::BareK ? "裸K" : "海龟";
    const std::string other = keep == smart::Strategy::BareK ? "海龟" : "裸K";
    int was = -1;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        was = foreign_noted_;
        foreign_noted_ = holding > 0 ? holding : -1;
    }
    if (holding <= 0) {
        if (was > 0 && log_) log_("智能模式：" + other + "持仓已全部走完，" + mine + "开始运行", false);
        return false;
    }
    // 第一次走告警（headless 改配置重启后什么都没跑，要让人知道为什么）；数量变了再提一行
    if (holding != was && log_)
        log_("智能模式：还有 " + std::to_string(holding) + " 笔" + other + "持仓，走完（或手动平仓）后" +
             mine + "才启动", was < 0);
    return true;
}

void SmartRunner::clear_seeds() {
    std::vector<std::string> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& kv : seeds_) gone.push_back(kv.first);
        seeds_.clear();
        built_day_ = -1;   // 再切回裸K 时立刻重建
    }
    if (on_watch)
        for (const auto& s : gone) on_watch(s, false);
}

bool SmartRunner::watching(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return seeds_.count(symbol) > 0;
}

SmartRunner::Report SmartRunner::run_bare_once(int64_t now_ms, const smart::Config& cfg) {
    Report rep;
    rep.at_ms = now_ms;
    // 只跑一套：海龟还有持仓就不启动（候选池已清、空仓的海龟 bot 已删），一分钟后再看
    if (blocked_by_foreign(retire_foreign(smart::Strategy::BareK, now_ms), smart::Strategy::BareK)) {
        std::lock_guard<std::mutex> lk(mtx_);
        next_due_ms_ = now_ms + 60 * 1000;
        rep.summary  = "智能裸K：等海龟持仓走完";
        last_ = rep;
        return rep;
    }
    bool build = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        // 下一次：下一根 1h 收盘后 1 分钟。UTC 0 点（北京 08:00）那一轮跨了日，就是每天的重建
        next_due_ms_ = (now_ms / kHourMs + 1) * kHourMs + 60 * 1000;
        build = (built_day_ != day_of(now_ms));
    }
    return build ? build_ambush(now_ms, cfg) : refresh_ambush(now_ms, cfg);
}

SmartRunner::Report SmartRunner::build_ambush(int64_t now_ms, const smart::Config& cfg) {
    Report rep;
    rep.at_ms = now_ms;
    const auto scan = scan_ambush(*client_, cfg, now_ms);
    if (cancelled_.load()) return rep;
    rep.universe    = scan.universe;
    rep.passed_gate = scan.passed_gate;
    rep.eligible    = scan.fetched;
    if (!scan.ok) {
        int streak = 0;
        bool have_pool = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            streak = ++fail_streak_;
            have_pool = !seeds_.empty();
            next_due_ms_ = now_ms + 5 * 60 * 1000;   // 建池失败不等一小时，5 分钟后重试
        }
        rep.summary = "智能裸K：建埋伏池失败（" + scan.error + "），" +
                      (have_pool ? "沿用旧池" : "暂时没有埋伏池") + "，5 分钟后重试" +
                      (streak > 1 ? "（已连续 " + std::to_string(streak) + " 次）" : std::string());
        // 断网时会连着失败：第一次走告警，之后每小时提一次
        if (log_ && (streak == 1 || streak % 12 == 0)) log_(rep.summary, streak == 1);
        std::lock_guard<std::mutex> lk(mtx_);
        last_ = rep;
        return rep;
    }
    {
        int was = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            was = fail_streak_;
            fail_streak_ = 0;
        }
        if (was > 0 && log_)
            log_("智能裸K：建池已恢复（此前连续失败 " + std::to_string(was) + " 次）", false);
    }

    const auto picked = smart::select_ambush(scan.feats, cfg.bare);
    int gate_ok = 0;
    for (const auto& kv : scan.feats)
        if (smart::ambush_gate(kv.second, cfg.bare).empty()) ++gate_ok;
    std::vector<std::string> added, dropped;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::map<std::string, AmbushState> next;
        for (const auto& a : picked) {
            auto& st = next[a.symbol];
            st.a         = a;
            st.add_close = a.f.close;   // 每天重新起算「进池后涨了多少」
            if (auto it = seeds_.find(a.symbol); it != seeds_.end())
                st.ignited_ms = it->second.ignited_ms;
            else
                added.push_back(a.symbol);
        }
        for (const auto& kv : seeds_)
            if (!next.count(kv.first)) dropped.push_back(kv.first);
        seeds_.swap(next);
        built_day_  = day_of(now_ms);
        bare_stats_ = {};
    }
    if (on_watch) {
        for (const auto& s : added)   on_watch(s, true);
        for (const auto& s : dropped) on_watch(s, false);
    }

    std::set<std::string> with_bot;
    for (const auto& b : engine_->get_bots()) {
        with_bot.insert(b.cfg.symbol);
        if (!b.is_auto) continue;
        ++rep.monitored;
        if (b.qty > 0) ++rep.holding;
    }
    if (journal_) journal_->ambush(now_ms, "建池", picked, with_bot);

    Group g;
    g.title = "埋伏池";
    g.code  = smart::kSegBare;
    g.quota = cfg.bare.pool_n;
    g.rows  = bare_rows(now_ms, {});
    rep.groups.push_back(std::move(g));
    rep.added   = (int)added.size();
    rep.removed = (int)dropped.size();
    rep.ok = true;
    std::vector<double> ks, as;
    for (const auto& a : picked) { ks.push_back(a.f.kappa); as.push_back(a.f.atr_pct); }
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    rep.summary = "智能裸K：建埋伏池 " + std::to_string(rep.universe) + " → 过门槛 " +
                  std::to_string(rep.passed_gate) + " → 结构合格 " + std::to_string(gate_ok) +
                  " → 入池 " + std::to_string(picked.size()) + "（κ 中位 " + fmt(median(ks), 2) +
                  "，1h ATR% 中位 " + fmt(median(as), 2) + "）；持仓 " + std::to_string(rep.holding);
    if (log_) log_(rep.summary, false);
    (void)engine_->take_filter_summary();   // 裸K 的 bot 自己不开仓，引擎那份汇总没有意义

    std::lock_guard<std::mutex> lk(mtx_);
    last_ = rep;
    save_locked();
    return rep;
}

SmartRunner::Report SmartRunner::refresh_ambush(int64_t now_ms, const smart::Config& cfg) {
    Report rep;
    rep.at_ms = now_ms;
    std::vector<std::string> syms;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& kv : seeds_) syms.push_back(kv.first);
    }
    const auto feats = fetch_ambush(*client_, syms);
    if (cancelled_.load()) return rep;

    std::vector<std::pair<std::string, std::string>> removed;
    std::vector<smart::Ambush> now_pool;
    BareStats st;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& s : syms) {
            auto it = seeds_.find(s);
            if (it == seeds_.end()) continue;
            const auto f = feats.find(s);
            if (f == feats.end()) continue;   // 这轮没拉到 ≠ 不合格：不动它
            const auto why = smart::ambush_drop(f->second, it->second.a.f.box_lo,
                                                it->second.add_close, cfg.bare);
            if (!why.empty()) {
                removed.emplace_back(s, why);
                seeds_.erase(it);
                continue;
            }
            if (f->second.ok) {   // 箱体、ATR 跟着最新收盘的 1h 走（点火判定用这个上沿）
                it->second.a.f     = f->second;
                it->second.a.score = smart::ambush_score(f->second, cfg.bare);
            }
        }
        for (const auto& kv : seeds_) now_pool.push_back(kv.second.a);
        st = bare_stats_;
        bare_stats_ = {};
    }
    if (on_watch)
        for (const auto& r : removed) on_watch(r.first, false);

    std::set<std::string> with_bot;
    for (const auto& b : engine_->get_bots()) {
        with_bot.insert(b.cfg.symbol);
        if (!b.is_auto) continue;
        ++rep.monitored;
        if (b.qty > 0) ++rep.holding;
    }
    if (journal_) {
        for (const auto& r : removed)
            journal_->pool(now_ms, r.first, "移出", smart::kSegBare, 0, 0, r.second);
        journal_->ambush(now_ms, "刷新", now_pool, with_bot);
    }

    Group g;
    g.title = "埋伏池";
    g.code  = smart::kSegBare;
    g.quota = cfg.bare.pool_n;
    g.rows  = bare_rows(now_ms, {});
    rep.groups.push_back(std::move(g));
    rep.removed = (int)removed.size();
    rep.ok = true;
    rep.summary = "智能裸K：待命刷新，埋伏池 " + std::to_string(now_pool.size()) +
                  (removed.empty() ? std::string() : "（移出 " + std::to_string(removed.size()) + "）") +
                  "；上一小时点火判定 " + std::to_string(st.judged) + " 次，量比达标 " +
                  std::to_string(st.vol_hit) + "，被否决 " + std::to_string(st.vetoed) + "，点火 " +
                  std::to_string(st.ignited) + "，开仓 " + std::to_string(st.entered) + "；持仓 " +
                  std::to_string(rep.holding);
    if (log_) {
        log_(rep.summary, false);
        if (!removed.empty()) {
            std::vector<std::string> d;
            for (const auto& r : removed) d.push_back(r.first + "（" + r.second + "）");
            log_("  移出：" + join(d, "、"), false);
        }
    }

    std::lock_guard<std::mutex> lk(mtx_);
    last_ = rep;
    save_locked();
    return rep;
}

std::vector<SmartRunner::Row> SmartRunner::bare_rows(int64_t now_ms, const PriceFn& price_of) const {
    std::vector<AmbushState> v;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& kv : seeds_) v.push_back(kv.second);
    }
    std::set<std::string> holding, with_bot;
    if (engine_)
        for (const auto& b : engine_->get_bots()) {
            with_bot.insert(b.cfg.symbol);
            if (b.qty > 0 || b.pending) holding.insert(b.cfg.symbol);
        }
    std::stable_sort(v.begin(), v.end(), [](const AmbushState& a, const AmbushState& b) {
        if (a.a.score != b.a.score) return a.a.score > b.a.score;
        return a.a.symbol < b.a.symbol;
    });
    std::vector<Row> rows;
    int i = 0;
    for (const auto& s : v) {
        const auto& f = s.a.f;
        double px = price_of ? price_of(s.a.symbol) : 0;
        if (!(px > 0)) px = f.close;
        Row r;
        r.symbol       = s.a.symbol;
        r.list         = smart::kSegBare;
        r.rank         = ++i;
        r.score        = s.a.score;
        r.atr_pct      = f.atr_pct;
        r.box12_atr    = f.atr_pct > 0 ? f.box_pct / f.atr_pct : 0;
        r.kappa        = f.kappa;
        r.box_pos      = f.pos;
        r.up_vol_share = f.up_vol_share;
        r.has_bot      = with_bot.count(s.a.symbol) > 0;
        if (px > 0) r.dist_up_pct = (f.box_hi - px) / px * 100.0;
        if (holding.count(s.a.symbol))                                       r.status = "持仓中";
        else if (s.ignited_ms > 0 && day_of(s.ignited_ms) == day_of(now_ms)) r.status = "今日已点火";
        else                                                                 r.status = "埋伏中";
        rows.push_back(std::move(r));
    }
    return rows;
}

void SmartRunner::ignite(const std::string& sym, const smart::Ambush& a, const smart::Ignition& g,
                         const smart::Config& cfg, const PriceFn& price_of, int64_t now_ms) {
    const auto id = engine_->add_bot(smart::make_bare_bot_config(cfg, sym), true);
    if (id.empty()) return;   // 同品种已有 bot（手动的，或刚被别处占了）
    engine_->set_smart_meta(id, a.score, 0, smart::kSegBare);
    // 自己不开仓：只认点火判定（下面的 force_open），它只负责之后的 1h ATR 追踪止损
    engine_->set_retiring(id);
    // 1h 快照：追踪止损的 ATR、开仓 ATR（锁定 ATR / 暴涨收紧的基准）都从这里来。权重 1
    const auto s = client_->fetch_trend_signal(sym, smart::kBareInterval, smart::kAmbushBoxBars,
                                               smart::kAtrPeriod);
    if (!s.ok) {
        engine_->remove_if_flat(id);
        if (log_) log_("智能裸K：" + sym + " 点火，但 1h 数据没拉到，这次不开", false);
        return;
    }
    engine_->update_signal(id, s.atr, s.atr_pct, s.dc_ok, s.dc_up, s.dc_dn, s.bar_open_ms,
                           s.box_atr, s.vol_ratio, s.er_ok, s.er);
    double px = price_of ? price_of(sym) : 0;
    if (!(px > 0)) px = g.close;
    const double box_atr = a.f.atr_pct > 0 ? a.f.box_pct / a.f.atr_pct
                                           : std::numeric_limits<double>::quiet_NaN();
    const auto why = engine_->force_open(id, trend::Pos::Long, px, g.low, 'I', g.vol_ratio, box_atr);
    if (!why.empty()) {
        engine_->remove_if_flat(id);
        if (log_) log_("智能裸K：" + sym + " 点火，没开：" + why, false);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++bare_stats_.entered;
        if (auto it = seeds_.find(sym); it != seeds_.end()) it->second.ignited_ms = now_ms;
    }
    if (log_)
        log_("智能裸K：" + sym + " 点火 @" + fmt_px(px) + "（量比 " + fmt(g.vol_ratio, 1) + "x，实体 +" +
             fmt(g.body_pct, 2) + "%，κ " + fmt(a.f.kappa, 2) + "），开多，止损 @" + fmt_px(g.low) +
             "（−" + fmt((px - g.low) / px * 100.0, 2) + "%）", false);
    if (journal_)
        journal_->pool(now_ms, sym, "点火开仓", smart::kSegBare, 0, a.score,
                       "量比 " + fmt(g.vol_ratio, 2) + "x 实体 " + fmt(g.body_pct, 2) + "% 止损 " +
                       fmt_px(g.low));
    if (on_symbol_added) on_symbol_added(sym);
}

void SmartRunner::bare_tick(int64_t now_ms, const PriceFn& price_of) {
    std::unique_lock<std::mutex> bl(bare_mtx_, std::try_to_lock);
    if (!bl.owns_lock()) return;
    const smart::Config cfg = config();
    if (!cfg.enabled || cfg.strategy != smart::Strategy::BareK || cancelled_.load() ||
        !engine_ || !client_)
        return;
    const auto& bc = cfg.bare;
    // 海龟留下的仓位还没走完：裸K 不开仓（平掉的海龟 bot 这里每拍就删，不等下一轮）
    if (retire_foreign(smart::Strategy::BareK, now_ms) > 0) return;

    // ── 1) 空仓的裸K bot 删掉（平仓了，或点火后被风控闸门拦着没开成）──
    std::set<std::string> with_bot;
    int active = 0;   // 持仓中 + 在途（与引擎"同时持仓上限"同一口径：全部 bot）
    const auto wall_now = std::chrono::system_clock::time_point(std::chrono::milliseconds(now_ms));
    for (const auto& b : engine_->get_bots()) {
        const bool busy = b.st.pos != trend::Pos::Flat || b.qty > 0 || b.pending;
        if (b.is_auto && b.smart_list == smart::kSegBare) {
            // 重启恢复的裸K bot 没有这个标记（不落盘）：补上，平仓后不能让它自己开
            if (!b.retiring) engine_->set_retiring(b.bot_id);
            if (!busy && b.state == TrendBot::State::Running &&
                (b.trade_count > 0 || wall_now - b.start_time >= std::chrono::milliseconds(kBareIdleMs)) &&
                engine_->remove_if_flat(b.bot_id)) {
                if (on_symbol_removed) on_symbol_removed(b.cfg.symbol);
                continue;
            }
        }
        with_bot.insert(b.cfg.symbol);
        if (busy) ++active;
    }

    // ── 2) 点火判定：每根 5m 收盘后 3 秒起、90 秒内，每个币只判一次刚收盘的那根 ──
    const int64_t close_ms = now_ms / kBar5Ms * kBar5Ms;   // 刚收盘那根的收盘时刻
    if (close_ms != ign_bar_) {
        ign_bar_ = close_ms;
        ign_done_.clear();
    }
    const int64_t since = now_ms - close_ms;
    if (since < 3000 || since > 90000) return;
    std::vector<smart::Ambush> todo;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (built_day_ < 0) return;   // 埋伏池还没建好
        for (const auto& kv : seeds_)
            if (!ign_done_.count(kv.first) && !with_bot.count(kv.first)) todo.push_back(kv.second.a);
    }
    if (todo.empty()) return;

    // 61 根 5m：去掉未收盘那根剩 60，够 20 根成交额中位数 + ATR 预热。权重 1
    std::vector<std::vector<smart::Bar>> bars(todo.size());
    parallel_for((int)todo.size(), 4, [&](int i) {
        try { bars[i] = closed_bars(client_->fetch_klines(todo[i].symbol, smart::kIgniteInterval, 61)); }
        catch (...) {}
    });
    const int64_t want_open = close_ms - kBar5Ms;   // 要判的那根的开盘时间
    for (size_t i = 0; i < todo.size(); ++i) {
        const auto& a = todo[i];
        const auto& b = bars[i];
        // 没拉到、或交易所还没出新的一根（拉回来的最后一根已收盘K线不是它）：下一拍再试
        if (b.empty() || b.back().open_ms != want_open) continue;
        ign_done_.insert(a.symbol);
        const auto g = smart::check_ignition(b, a.f.box_hi, a.f.box_lo, bc);
        const bool vol_hit = g.vol_ratio >= bc.vol_ratio;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++bare_stats_.judged;
            if (vol_hit) ++bare_stats_.vol_hit;
            if (vol_hit && g.vetoed) ++bare_stats_.vetoed;
            if (g.ok) ++bare_stats_.ignited;
        }
        // 只记「量比够了却没点火」的：每 5 分钟几十个币，量都不够的写进去只是噪音
        auto note = [&](const std::string& category, const std::string& detail) {
            if (!journal_) return;
            FilterEvent e;
            e.time     = wall_now;
            e.symbol   = a.symbol;
            e.is_long  = true;
            e.is_auto  = true;
            e.category = category;
            e.price    = g.close;
            e.detail   = detail + "（量比 " + fmt(g.vol_ratio, 2) + "x，实体 " + fmt(g.body_pct, 2) +
                         "%，收盘位置 " + fmt(g.close_pos, 2) + "，上影 " + fmt(g.upper_wick, 2) + "）";
            journal_->filter(e);
        };
        if (!g.ok) {
            if (vol_hit) note(g.vetoed ? "点火否决" : "点火未达标", g.why);
            continue;
        }
        if (bc.max_positions > 0 && active >= bc.max_positions) {
            note("名额已满", "点火了，但同时持仓已到上限 " + std::to_string(bc.max_positions));
            continue;
        }
        if (engine_->pause_new()) {
            note("暂停开新仓", "点火了，但暂停开新仓开着");
            continue;
        }
        ignite(a.symbol, a, g, cfg, price_of, now_ms);
        ++active;
    }
}

void SmartRunner::save_locked() const {
    if (state_path_.empty() || cancelled_.load()) return;   // 作废的不和新运行器抢文件
    std::ostringstream o;
    o << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto d = engine_ ? engine_->day_pnl() : TrendEngine::DayPnl{};
    o << "{\"pool\":" << pool_.to_json() << ",\"day\":{\"d\":" << d.day << ",\"r\":" << d.realized << "}}";
    // 原子写：先写临时文件再改名
    const std::string tmp = state_path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << o.str();
        f.flush();
        if (!f.good()) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, state_path_, ec);
    if (ec) {
        std::ofstream f(state_path_, std::ios::binary | std::ios::trunc);
        if (f) f << o.str();
        std::filesystem::remove(tmp, ec);
    }
}

void SmartRunner::load() {
    std::ifstream f(state_path_, std::ios::binary);
    if (!f) return;
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string raw = buf.str();
    simdjson::dom::parser p;
    simdjson::dom::element root;
    auto ps = simdjson::padded_string(raw);
    if (p.parse(ps).get(root) != simdjson::SUCCESS) return;
    simdjson::dom::element pool;
    if (root["pool"].get(pool) == simdjson::SUCCESS) {
        std::lock_guard<std::mutex> lk(mtx_);
        pool_.from_json(simdjson::to_string(pool));
    }
    int64_t day = 0;
    double r = 0;
    if (root["day"]["d"].get(day) == simdjson::SUCCESS && engine_) {
        if (root["day"]["r"].get(r) != simdjson::SUCCESS) {
            int64_t ri = 0;
            if (root["day"]["r"].get(ri) == simdjson::SUCCESS) r = (double)ri;
        }
        engine_->restore_day_pnl({day, r});
    }
}

} // namespace ccbot
