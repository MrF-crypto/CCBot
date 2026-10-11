#include "app/smart_runner.h"
#include "core/parallel.h"
#include "net/trading_client.h"

#include <simdjson.h>
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

// 交易所K线 → 智能模式K线，去掉最后一根（未收盘）
std::vector<smart::Bar> closed_bars(const std::vector<TradingClient::Bar>& in) {
    std::vector<smart::Bar> v;
    if (in.size() < 2) return v;
    v.reserve(in.size() - 1);
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        const auto& b = in[i];
        v.push_back({b.open_ms, b.open, b.high, b.low, b.close, b.volume});
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
            g.rows.push_back({list[i], code, i + 1, score_of(s), s.f.atr_pct, s.f.box12_atr,
                              s.f.vol_lift, s.f.rs, s.f.max_up24, with_bot.count(list[i]) > 0});
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

MarketScan scan_market(TradingClient& c, const smart::Config& cfg,
                       const std::set<std::string>& pool_syms, int64_t now_ms, int parallel) {
    MarketScan r;
    auto& out = r.outcome;

    // 交易规则与 24h 行情是整轮的地基，拿不到就整轮作废——绝不能把"没拿到"
    // 当成"池里的币都不合格"去删 bot
    bool ok_meta = false, ok_t24 = false, ok_fund = false;
    const auto metas = c.fetch_exchange_symbols(&ok_meta);
    if (!ok_meta) { r.error = "交易规则拉取失败"; return r; }
    const auto t24 = c.fetch_all_24h(&ok_t24);
    if (!ok_t24) { r.error = "24h 行情拉取失败"; return r; }
    // 资金费率拿不到不致命：这一轮不按费率判，其余照常
    const auto fund = c.fetch_all_funding_pct(&ok_fund);

    std::set<std::string> seen;
    std::vector<std::string> cand;
    for (const auto& mm : metas) {
        smart::SymbolMeta m;
        m.symbol = mm.symbol; m.base = mm.base; m.quote = mm.quote;
        m.contract_type = mm.contract_type; m.status = mm.status;
        m.onboard_ms = mm.onboard_ms; m.delivery_ms = mm.delivery_ms; m.tick = mm.tick;
        if (m.quote != "USDT" || m.contract_type != "PERPETUAL") continue;
        ++r.universe;
        seen.insert(m.symbol);
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
    // 任何变化都立刻重扫一次：门槛、方向、模板变了，名单和 bot 参数都要跟上
    if (smart::config_to_json(cfg_) != before) next_due_ms_ = 0;
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
        engine_->set_max_open_positions(cfg_.max_positions);
        engine_->set_daily_loss_limit(cfg_.daily_loss_limit);
        engine_->set_float_loss_limit(cfg_.float_loss_limit);
        engine_->set_priority_reserve(cfg_.prio_reserve, cfg_.prio_min_score);
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
           x.lock_atr == y.lock_atr && x.accel_exit == y.accel_exit;
}

SmartRunner::Report SmartRunner::run_once(int64_t now_ms) {
    std::unique_lock<std::mutex> rl(run_mtx_, std::try_to_lock);
    if (!rl.owns_lock()) return last_report();   // 上一轮还在跑

    const smart::Config cfg = config();
    Report rep;
    rep.at_ms = now_ms;
    if (cancelled_.load()) return rep;
    if (!cfg.enabled || !engine_ || !client_) { housekeep(now_ms); return rep; }

    const auto bots = engine_->get_bots();
    std::set<std::string> pool_syms;
    std::vector<smart::BotView> views;
    views.reserve(bots.size());
    for (const auto& b : bots) {
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
        rep.summary = "智能模式：本轮扫描失败（" + scan.error + "），候选池不动，下个整点再试" +
                      (streak > 1 ? "（已连续 " + std::to_string(streak) + " 轮）" : std::string());
        // 只有第一轮失败走告警（会推 webhook），之后只记日志
        if (log_) log_(rep.summary, streak == 1);
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

void SmartRunner::housekeep(int64_t /*now_ms*/) {
    if (!engine_ || cancelled_.load()) return;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        enabled = cfg_.enabled;
    }
    if (!enabled) {
        // 关闭后：空仓的自动 bot 移除；持仓中的留到这笔走完，下一次维护再移除。
        // 已停止的（你或对账停的）不替你删
        for (const auto& b : engine_->get_bots()) {
            if (!b.is_auto || b.state == TrendBot::State::Stopped) continue;
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
