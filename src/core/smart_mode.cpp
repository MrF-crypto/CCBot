#include "core/smart_mode.h"
#include "core/indicators.h"

#include <simdjson.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>

namespace ccbot::smart {

namespace {

constexpr int64_t kDayMs  = 24LL * 3600 * 1000;
constexpr int64_t kHourMs = 3600LL * 1000;
// 交割日期在这么久以内就算"即将下架"。永续的交割日期是 2100 年，币安公告下架后
// 会把它改到下架那天——提前一个月起就不再新加，持仓中的醒目告警
constexpr int64_t kDelistHorizonMs = 30 * kDayMs;
// 批量淘汰保护的下限：池子很小时（比如只剩 3 个）一轮淘汰 2 个是正常的
constexpr int kMassRemoveMin = 4;

std::string fmt(double v, int prec) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

} // namespace

// ── 配置 ────────────────────────────────────────────────────────────────────

TrendConfig Config::default_template() {
    TrendConfig t;
    t.interval    = "1h";
    t.budget_usdt = 100;
    t.leverage    = 3;
    t.rule.strategy        = trend::Strategy::Turtle;
    t.rule.donchian_period = 12;
    t.rule.atr_period      = 14;
    t.rule.atr_mult        = 3.0;
    t.rule.squeeze_box_atr    = 4.0;
    t.rule.trend_er_min       = 0.4;
    t.rule.breakout_vol_ratio = 1.5;
    t.rule.lock_atr   = true;
    t.rule.accel_exit = true;
    t.use_disaster_stop        = true;
    t.disaster_stop_buffer_pct = 0.2;
    return t;
}

trend::Direction bot_direction(const Config& cfg) {
    if (cfg.dir_long && !cfg.dir_short) return trend::Direction::LongOnly;
    if (cfg.dir_short && !cfg.dir_long) return trend::Direction::ShortOnly;
    return trend::Direction::Both;
}

TrendConfig make_bot_config(const Config& cfg, const std::string& symbol) {
    TrendConfig c = cfg.tmpl;
    c.symbol = symbol;
    // 智能模式只建海龟。下面三项不随模板：
    //   · 方向跟勾选走（两个都勾 = 多空，向上突破做多、向下突破做空）
    //   · 不反手：自动 bot 在没看过的币上交易，亏损止损后立刻反向开一笔是在放大
    //     不确定性。多空模式下反方向真突破了，照样会按正常信号开
    //   · 硬止损强制开：程序崩溃时这些仓位不能没有交易所侧的底
    c.rule.strategy  = trend::Strategy::Turtle;
    c.rule.direction = bot_direction(cfg);
    c.rule.reverse   = trend::ReverseMode::None;
    c.use_disaster_stop = true;
    return c;
}

// ── 硬门槛 ──────────────────────────────────────────────────────────────────

bool builtin_excluded(const std::string& base) {
    // BTC / ETH 不是"山寨币"；稳定币没有趋势；BTCDOM 是指数合约
    static const std::set<std::string> kSkip = {
        "BTC", "ETH", "BTCDOM",
        "USDC", "FDUSD", "TUSD", "USDP", "DAI", "BUSD", "USDE", "PYUSD", "RLUSD", "USD1",
    };
    return kSkip.count(base) > 0;
}

bool delisting(const SymbolMeta& m, int64_t now_ms) {
    if (m.status == "SETTLING" || m.status == "CLOSE" || m.status == "DELIVERING") return true;
    return m.delivery_ms > 0 && m.delivery_ms < now_ms + kDelistHorizonMs;
}

std::string pre_gate(const SymbolMeta& m, const Ticker24* t, const double* funding_pct,
                     const Config& cfg, int64_t now_ms, bool relax) {
    if (m.quote != "USDT")                return "非 USDT 计价";
    if (m.contract_type != "PERPETUAL")   return "非永续";
    if (delisting(m, now_ms))             return "即将下架";
    if (m.status != "TRADING")            return "状态 " + m.status;
    if (builtin_excluded(m.base))         return "内置排除";
    if (cfg.blacklist.count(m.symbol))    return "已拉黑";
    if (m.onboard_ms > 0 && now_ms - m.onboard_ms < cfg.min_age_days * kDayMs)
        return "上线不满 " + std::to_string(cfg.min_age_days) + " 天";
    if (t) {
        if (m.tick > 0 && t->last > 0 && m.tick / t->last * 100.0 > cfg.max_tick_pct)
            return "价格精度太粗";
        const double need = relax ? cfg.min_quote_vol * 0.8 : cfg.min_quote_vol;
        if (!(t->quote_vol >= need))
            return "24h 成交额 " + fmt(t->quote_vol / 1e6, 1) + "M 不足";
        // 下面两条只拦【新加入】：已在池里的币突然暴涨 60%，空仓时淘汰交给排名，
        // 而不是在这里一刀切掉
        if (!relax && std::fabs(t->chg_pct) > cfg.max_abs_chg24)
            return "24h 涨跌 " + fmt(t->chg_pct, 1) + "% 过大";
    }
    if (funding_pct && !relax && std::fabs(*funding_pct) > cfg.max_abs_funding)
        return "资金费率 " + fmt(*funding_pct, 3) + "% 过高";
    return {};
}

std::string atr_gate(const Features& f, const Config& cfg, bool relax) {
    const double lo = relax ? cfg.atr_pct_min * 0.8 : cfg.atr_pct_min;
    const double hi = relax ? cfg.atr_pct_max * 1.2 : cfg.atr_pct_max;
    if (f.atr_pct < lo) return "ATR% " + fmt(f.atr_pct, 2) + " 太低";
    if (f.atr_pct > hi) return "ATR% " + fmt(f.atr_pct, 2) + " 太高";
    return {};
}

// ── 特征 ────────────────────────────────────────────────────────────────────

Features compute_features(const std::vector<Bar>& bars, const std::vector<Bar>& btc) {
    Features f;
    const int n = (int)bars.size();
    if (n < kMinBars) { f.why = "K线不足"; return f; }

    std::vector<indicators::Ohlc> oh;
    oh.reserve(bars.size());
    for (const auto& b : bars) oh.push_back({b.high, b.low, b.close});

    const double atr14  = indicators::atr(oh, 14);
    const double atr100 = indicators::atr(oh, 100);
    const double close  = bars.back().close;
    if (!(atr14 > 0 && atr100 > 0 && close > 0)) { f.why = "ATR 算不出"; return f; }

    auto box = [&](int k, double& hi, double& lo) {
        hi = bars[n - k].high; lo = bars[n - k].low;
        for (int i = n - k + 1; i < n; ++i) { hi = std::max(hi, bars[i].high); lo = std::min(lo, bars[i].low); }
    };
    double hi12, lo12, hi48, lo48;
    box(12, hi12, lo12);
    box(48, hi48, lo48);

    f.atr_pct   = atr14 / close * 100.0;
    f.box12_atr = (hi12 - lo12) / atr14;
    f.box48_atr = (hi48 - lo48) / atr14;
    f.atr_ratio = atr14 / atr100;
    f.pos12     = (hi12 > lo12) ? (close - lo12) / (hi12 - lo12) : 0.5;

    auto avg_vol = [&](int k) {
        double s = 0;
        for (int i = n - k; i < n; ++i) s += bars[i].volume;
        return s / k;
    };
    const double v168 = avg_vol(168);
    f.vol_lift = (v168 > 0) ? avg_vol(6) / v168 : 0;

    // 全是已收盘K线，不排除最后一根
    if (const auto e = indicators::efficiency_ratio(oh, 24, 0); e.ok) f.er24 = e.er;

    // 相对强弱：7 天涨幅减去 BTC 的，再除以 7 天波动率（小时对数收益的标准差 × √168）。
    // 除以波动率是为了让"稳稳涨 20%"排在"上蹿下跳涨 20%"前面
    const double ret7 = close / bars[n - 1 - 168].close - 1.0;
    double btc_ret7 = 0;
    if ((int)btc.size() >= 169 && btc[btc.size() - 1 - 168].close > 0)
        btc_ret7 = btc.back().close / btc[btc.size() - 1 - 168].close - 1.0;
    double s = 0, s2 = 0;
    int m = 0;
    for (int i = n - 168; i < n; ++i) {
        if (!(bars[i - 1].close > 0 && bars[i].close > 0)) continue;
        const double r = std::log(bars[i].close / bars[i - 1].close);
        s += r; s2 += r * r; ++m;
    }
    double vol7 = 0;
    if (m > 1) {
        const double mean = s / m;
        const double var  = std::max(0.0, s2 / m - mean * mean);
        vol7 = std::sqrt(var) * std::sqrt(168.0);
    }
    f.rs = (vol7 > 0) ? (ret7 - btc_ret7) / vol7 : 0;

    f.ok = std::isfinite(f.atr_pct) && std::isfinite(f.box12_atr) && std::isfinite(f.box48_atr) &&
           std::isfinite(f.atr_ratio) && std::isfinite(f.vol_lift) && std::isfinite(f.pos12) &&
           std::isfinite(f.rs) && std::isfinite(f.er24);
    if (!f.ok) f.why = "特征含非法值";
    return f;
}

// ── 打分排名 ────────────────────────────────────────────────────────────────

namespace {

// 百分位：最好的 100，最差的 0，并列取平均名次
std::vector<double> percentiles(const std::vector<double>& v, bool higher_better) {
    const int n = (int)v.size();
    std::vector<double> out(n, 100.0);
    if (n <= 1) return out;
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return v[a] < v[b]; });
    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double r = (i + j) / 2.0;                // 升序名次（0 = 最小）
        const double p = r / (n - 1) * 100.0;
        for (int k = i; k <= j; ++k) out[idx[k]] = higher_better ? p : 100.0 - p;
        i = j + 1;
    }
    return out;
}

} // namespace

int Ranking::rank_long(const std::string& s) const {
    auto it = std::find(longs.begin(), longs.end(), s);
    return it == longs.end() ? 0 : (int)(it - longs.begin()) + 1;
}
int Ranking::rank_short(const std::string& s) const {
    auto it = std::find(shorts.begin(), shorts.end(), s);
    return it == shorts.end() ? 0 : (int)(it - shorts.begin()) + 1;
}

Ranking rank(const std::map<std::string, Features>& eligible, const Config& cfg, const Weights& w) {
    Ranking r;
    std::vector<std::string> syms;
    std::vector<const Features*> fs;
    for (const auto& kv : eligible)
        if (kv.second.ok) { syms.push_back(kv.first); fs.push_back(&kv.second); }
    const int n = (int)syms.size();
    if (n == 0) return r;

    auto col = [&](double Features::*m) {
        std::vector<double> v(n);
        for (int i = 0; i < n; ++i) v[i] = fs[i]->*m;
        return v;
    };
    const auto rs = col(&Features::rs),        er = col(&Features::er24);
    const auto b12 = col(&Features::box12_atr), ar = col(&Features::atr_ratio);
    const auto b48 = col(&Features::box48_atr), vl = col(&Features::vol_lift);
    const auto ps = col(&Features::pos12);

    // 方向无关的指标两边共用；有方向的指标做空时取反（越弱、越靠近下沿越好）
    const auto p_b12 = percentiles(b12, false), p_ar = percentiles(ar, false);
    const auto p_b48 = percentiles(b48, false), p_vl = percentiles(vl, true);
    const auto pl_rs = percentiles(rs, true),  ps_rs = percentiles(rs, false);
    const auto pl_er = percentiles(er, true),  ps_er = percentiles(er, false);
    const auto pl_ps = percentiles(ps, true),  ps_ps = percentiles(ps, false);

    const double wsum = w.rs + w.er + w.box12 + w.atr_ratio + w.box48 + w.vol + w.pos;
    for (int i = 0; i < n; ++i) {
        const double common = w.box12 * p_b12[i] + w.atr_ratio * p_ar[i] +
                              w.box48 * p_b48[i] + w.vol * p_vl[i];
        Scored s;
        s.symbol      = syms[i];
        s.f           = *fs[i];
        s.long_score  = (common + w.rs * pl_rs[i] + w.er * pl_er[i] + w.pos * pl_ps[i]) / wsum;
        s.short_score = (common + w.rs * ps_rs[i] + w.er * ps_er[i] + w.pos * ps_ps[i]) / wsum;
        r.rows[syms[i]] = s;
    }

    auto build = [&](bool lng) {
        std::vector<std::string> v;
        for (const auto& kv : r.rows) {
            const double sc = lng ? kv.second.long_score : kv.second.short_score;
            if (sc >= cfg.min_score) v.push_back(kv.first);
        }
        std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
            const double sa = lng ? r.rows[a].long_score : r.rows[a].short_score;
            const double sb = lng ? r.rows[b].long_score : r.rows[b].short_score;
            if (sa != sb) return sa > sb;
            return a < b;   // 同分按名字，结果确定
        });
        return v;
    };
    r.longs  = build(true);
    r.shorts = build(false);
    return r;
}

// ── 候选池 ──────────────────────────────────────────────────────────────────

Plan Pool::plan(const ScanOutcome& scan, const std::vector<BotView>& bots,
                const Config& cfg, int64_t now_ms) {
    Plan p;
    if (!scan.ok) return p;   // 整轮扫描不可信：一个 bot 都不动

    const int keep_l = (int)std::ceil(cfg.long_n  * cfg.drop_rank_factor);
    const int keep_s = (int)std::ceil(cfg.short_n * cfg.drop_rank_factor);

    std::set<std::string> present;     // 任何 bot（含手动、含已停止）占着的品种
    std::set<std::string> auto_syms;
    int auto_count = 0;
    for (const auto& b : bots) present.insert(b.symbol);

    // 本轮准备移除的（先攒着，最后过一道"批量淘汰"保护再落定）
    std::vector<std::pair<Plan::Remove, Plan::Meta>> rm;
    int judged = 0;   // 本轮真正被评判过的空仓自动 bot（拿到了数据、没停止）

    for (const auto& b : bots) {
        if (!b.is_auto) continue;
        ++auto_count;
        auto_syms.insert(b.symbol);
        auto& e = entries_[b.symbol];
        // 池文件丢了（或首次接管）：从现在起算保护期，宁可晚淘汰
        if (e.added_ms == 0) e.added_ms = now_ms;

        // 这一轮没拿到它的数据：什么都不做。没拿到 ≠ 不合格
        if (scan.unknown.count(b.symbol)) continue;

        const int rl = cfg.dir_long  ? scan.ranking.rank_long(b.symbol)  : 0;
        const int rs = cfg.dir_short ? scan.ranking.rank_short(b.symbol) : 0;
        double sl = 0, ss = 0;
        if (auto it = scan.ranking.rows.find(b.symbol); it != scan.ranking.rows.end()) {
            sl = it->second.long_score;
            ss = it->second.short_score;
        }
        Plan::Meta m;
        m.bot_id = b.bot_id;
        if (rl > 0 && (rs == 0 || sl >= ss)) { m.list = 'L'; m.rank = rl; m.score = sl; }
        else if (rs > 0)                     { m.list = 'S'; m.rank = rs; m.score = ss; }
        else m.score = std::max(cfg.dir_long ? sl : 0.0, cfg.dir_short ? ss : 0.0);

        const bool in_list = (rl > 0 && rl <= keep_l) || (rs > 0 && rs <= keep_s);
        const auto hard = scan.hard_fail.find(b.symbol);

        if (b.has_pos) {
            // 持仓中：绝不移除，等这笔走完。不合格的标成"待移出"
            m.leaving = (hard != scan.hard_fail.end()) || !in_list;
            if (scan.delisting.count(b.symbol))
                p.alerts.push_back(b.symbol + " 即将下架，持仓中——请尽快手动处理（程序不自动平仓）");
            p.meta.push_back(m);
            continue;
        }
        // 已停止的：是你或对账停的，可能有要看的原因，不替你删
        if (b.stopped) { p.meta.push_back(m); continue; }

        ++judged;
        if (hard != scan.hard_fail.end()) {
            rm.push_back({{b.bot_id, b.symbol, hard->second}, m});
            continue;
        }
        if (!in_list) {
            ++e.miss_scans;
            const bool old_enough = now_ms - e.added_ms >= cfg.min_residence_h * kHourMs;
            if (e.miss_scans >= cfg.drop_after_scans && old_enough) {
                rm.push_back({{b.bot_id, b.symbol,
                               "连续 " + std::to_string(e.miss_scans) + " 轮掉出名单"}, m});
                continue;
            }
        } else {
            e.miss_scans = 0;
        }
        p.meta.push_back(m);
    }

    // ── 批量淘汰保护 ──
    // 一轮里过半的自动 bot 同时被判出局，正常行情里几乎不会发生；更可能是数据
    // 本身出了问题（交易所改了字段名、某个字段解析成 0）——那就是"拿到的不是
    // 我以为的那个"，照单执行会一轮删光整个池子。这一轮一个都不删，告警，
    // 下一轮再看。持续成立的话每轮都会再拦、再告警
    if ((int)rm.size() >= kMassRemoveMin && (int)rm.size() * 2 > judged) {
        p.alerts.push_back("智能模式：本轮 " + std::to_string(rm.size()) + "/" +
                           std::to_string(judged) + " 个自动 bot 同时被判出局，疑似数据异常，"
                           "这一轮不移除，下一轮再看");
        for (const auto& x : rm) p.meta.push_back(x.second);
    } else {
        for (const auto& x : rm) {
            p.remove.push_back(x.first);
            --auto_count;
        }
    }

    // 补位：两份名单各取前 N，按分数统一排队，直到填满容量
    const int cap = (cfg.dir_long ? cfg.long_n : 0) + (cfg.dir_short ? cfg.short_n : 0);
    std::vector<Plan::Add> cands;
    if (cfg.dir_long)
        for (int i = 0; i < (int)scan.ranking.longs.size() && i < cfg.long_n; ++i) {
            const auto& s = scan.ranking.longs[i];
            cands.push_back({s, 'L', i + 1, scan.ranking.rows.at(s).long_score});
        }
    if (cfg.dir_short)
        for (int i = 0; i < (int)scan.ranking.shorts.size() && i < cfg.short_n; ++i) {
            const auto& s = scan.ranking.shorts[i];
            cands.push_back({s, 'S', i + 1, scan.ranking.rows.at(s).short_score});
        }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Plan::Add& a, const Plan::Add& b) { return a.score > b.score; });
    for (const auto& c : cands) {
        if (auto_count >= cap) break;
        if (present.count(c.symbol)) continue;   // 已有 bot（含手动）或同轮已选
        present.insert(c.symbol);
        p.add.push_back(c);
        ++auto_count;
    }

    // 不再有自动 bot 的条目清掉（加上的由 on_added 写回）
    for (auto it = entries_.begin(); it != entries_.end();)
        it = auto_syms.count(it->first) ? std::next(it) : entries_.erase(it);
    return p;
}

void Pool::on_added(const std::string& symbol, int64_t now_ms) {
    entries_[symbol] = Entry{now_ms, 0};
}
void Pool::on_removed(const std::string& symbol) { entries_.erase(symbol); }

std::string Pool::to_json() const {
    std::ostringstream o;
    o << "{\"v\":1,\"entries\":[";
    bool first = true;
    for (const auto& kv : entries_) {
        if (!first) o << ",";
        first = false;
        o << "{\"s\":\"" << kv.first << "\",\"a\":" << kv.second.added_ms
          << ",\"m\":" << kv.second.miss_scans << "}";
    }
    o << "]}";
    return o.str();
}

bool Pool::from_json(std::string_view s) {
    simdjson::dom::parser p;
    simdjson::dom::element root;
    auto ps = simdjson::padded_string(s);
    if (p.parse(ps).get(root) != simdjson::SUCCESS) return false;
    simdjson::dom::array arr;
    if (root["entries"].get_array().get(arr) != simdjson::SUCCESS) return false;
    std::map<std::string, Entry> got;
    for (auto el : arr) {
        std::string_view sym;
        int64_t a = 0, m = 0;
        if (el["s"].get(sym) != simdjson::SUCCESS || sym.empty()) continue;
        if (el["a"].get(a) != simdjson::SUCCESS) a = 0;
        if (el["m"].get(m) != simdjson::SUCCESS) m = 0;
        got[std::string(sym)] = Entry{a, (int)m};
    }
    entries_ = std::move(got);
    return true;
}

// ── 配置 JSON ───────────────────────────────────────────────────────────────

std::string config_to_json(const Config& c) {
    std::ostringstream o;
    o << std::setprecision(std::numeric_limits<double>::max_digits10);
    auto b = [](bool v) { return v ? "true" : "false"; };
    o << "{"
      << "\"enabled\":" << b(c.enabled) << ","
      << "\"dir_long\":" << b(c.dir_long) << ","
      << "\"dir_short\":" << b(c.dir_short) << ","
      << "\"min_quote_vol\":" << c.min_quote_vol << ","
      << "\"min_age_days\":" << c.min_age_days << ","
      << "\"atr_pct_min\":" << c.atr_pct_min << ","
      << "\"atr_pct_max\":" << c.atr_pct_max << ","
      << "\"max_abs_chg24\":" << c.max_abs_chg24 << ","
      << "\"max_abs_funding\":" << c.max_abs_funding << ","
      << "\"max_tick_pct\":" << c.max_tick_pct << ","
      << "\"blacklist\":[";
    bool first = true;
    for (const auto& s : c.blacklist) { o << (first ? "" : ",") << "\"" << s << "\""; first = false; }
    o << "],"
      << "\"long_n\":" << c.long_n << ","
      << "\"short_n\":" << c.short_n << ","
      << "\"min_score\":" << c.min_score << ","
      << "\"drop_rank_factor\":" << c.drop_rank_factor << ","
      << "\"drop_after_scans\":" << c.drop_after_scans << ","
      << "\"min_residence_h\":" << c.min_residence_h << ","
      << "\"max_positions\":" << c.max_positions << ","
      << "\"daily_loss_limit\":" << c.daily_loss_limit << ","
      << "\"float_loss_limit\":" << c.float_loss_limit << ","
      << "\"prio_reserve\":" << c.prio_reserve << ","
      << "\"prio_min_score\":" << c.prio_min_score << ","
      << "\"pause_new\":" << b(c.pause_new) << ",";
    const auto& t = c.tmpl;
    o << "\"template\":{"
      << "\"interval\":\"" << t.interval << "\","
      << "\"budget_usdt\":" << t.budget_usdt << ","
      << "\"leverage\":" << t.leverage << ","
      << "\"size_mode\":\"" << (t.size_mode == TrendConfig::SizeMode::RiskBased ? "risk" : "notional") << "\","
      << "\"risk_usdt\":" << t.risk_usdt << ","
      << "\"donchian_period\":" << t.rule.donchian_period << ","
      << "\"atr_period\":" << t.rule.atr_period << ","
      << "\"atr_mult\":" << t.rule.atr_mult << ","
      << "\"squeeze_box_atr\":" << t.rule.squeeze_box_atr << ","
      << "\"trend_er_min\":" << t.rule.trend_er_min << ","
      << "\"breakout_vol_ratio\":" << t.rule.breakout_vol_ratio << ","
      << "\"lock_atr\":" << b(t.rule.lock_atr) << ","
      << "\"accel_exit\":" << b(t.rule.accel_exit) << ","
      << "\"disaster_stop_buffer_pct\":" << t.disaster_stop_buffer_pct << ","
      << "\"signal_max_age_sec\":" << t.signal_max_age_sec
      << "}}";
    return o.str();
}

namespace {

bool num(simdjson::dom::object& o, const char* k, double& out) {
    simdjson::dom::element v;
    if (o[k].get(v) != simdjson::SUCCESS) return false;
    double d; int64_t i; uint64_t u;
    if (v.get(d) == simdjson::SUCCESS) { out = d; return true; }
    if (v.get(i) == simdjson::SUCCESS) { out = (double)i; return true; }
    if (v.get(u) == simdjson::SUCCESS) { out = (double)u; return true; }
    return false;
}
void get_num(simdjson::dom::object& o, const char* k, double& out) { num(o, k, out); }
void get_int(simdjson::dom::object& o, const char* k, int& out) {
    double d;
    if (num(o, k, d)) out = (int)std::llround(d);
}
void get_bool(simdjson::dom::object& o, const char* k, bool& out) {
    bool v;
    if (o[k].get(v) == simdjson::SUCCESS) out = v;
}

} // namespace

bool config_from_json(std::string_view s, Config& cfg, std::string* err) {
    simdjson::dom::parser p;
    simdjson::dom::object o;
    auto ps = simdjson::padded_string(s);
    if (p.parse(ps).get_object().get(o) != simdjson::SUCCESS) {
        if (err) *err = "智能模式配置不是合法的 JSON 对象";
        return false;
    }
    Config c = cfg;
    get_bool(o, "enabled", c.enabled);
    get_bool(o, "dir_long", c.dir_long);
    get_bool(o, "dir_short", c.dir_short);
    get_num(o, "min_quote_vol", c.min_quote_vol);
    get_int(o, "min_age_days", c.min_age_days);
    get_num(o, "atr_pct_min", c.atr_pct_min);
    get_num(o, "atr_pct_max", c.atr_pct_max);
    get_num(o, "max_abs_chg24", c.max_abs_chg24);
    get_num(o, "max_abs_funding", c.max_abs_funding);
    get_num(o, "max_tick_pct", c.max_tick_pct);
    {
        simdjson::dom::array arr;
        if (o["blacklist"].get_array().get(arr) == simdjson::SUCCESS) {
            c.blacklist.clear();
            for (auto el : arr) {
                std::string_view v;
                if (el.get(v) == simdjson::SUCCESS && !v.empty()) c.blacklist.insert(std::string(v));
            }
        }
    }
    get_int(o, "long_n", c.long_n);
    get_int(o, "short_n", c.short_n);
    get_num(o, "min_score", c.min_score);
    get_num(o, "drop_rank_factor", c.drop_rank_factor);
    get_int(o, "drop_after_scans", c.drop_after_scans);
    get_int(o, "min_residence_h", c.min_residence_h);
    get_int(o, "max_positions", c.max_positions);
    get_num(o, "daily_loss_limit", c.daily_loss_limit);
    get_num(o, "float_loss_limit", c.float_loss_limit);
    get_int(o, "prio_reserve", c.prio_reserve);
    get_num(o, "prio_min_score", c.prio_min_score);
    get_bool(o, "pause_new", c.pause_new);

    simdjson::dom::object t;
    if (o["template"].get_object().get(t) == simdjson::SUCCESS) {
        auto& tm = c.tmpl;
        std::string_view sv;
        if (t["interval"].get(sv) == simdjson::SUCCESS) tm.interval = std::string(sv);
        get_num(t, "budget_usdt", tm.budget_usdt);
        get_int(t, "leverage", tm.leverage);
        if (t["size_mode"].get(sv) == simdjson::SUCCESS)
            tm.size_mode = (sv == "risk") ? TrendConfig::SizeMode::RiskBased
                                          : TrendConfig::SizeMode::Notional;
        get_num(t, "risk_usdt", tm.risk_usdt);
        get_int(t, "donchian_period", tm.rule.donchian_period);
        get_int(t, "atr_period", tm.rule.atr_period);
        get_num(t, "atr_mult", tm.rule.atr_mult);
        get_num(t, "squeeze_box_atr", tm.rule.squeeze_box_atr);
        get_num(t, "trend_er_min", tm.rule.trend_er_min);
        get_num(t, "breakout_vol_ratio", tm.rule.breakout_vol_ratio);
        get_bool(t, "lock_atr", tm.rule.lock_atr);
        get_bool(t, "accel_exit", tm.rule.accel_exit);
        get_num(t, "disaster_stop_buffer_pct", tm.disaster_stop_buffer_pct);
        get_int(t, "signal_max_age_sec", tm.signal_max_age_sec);
    }

    // ── 校验：有一项不合法就整份拒绝，cfg 不动 ──
    auto bad = [&](const std::string& m) { if (err) *err = "智能模式：" + m; return false; };
    if (!(c.min_quote_vol >= 0))                         return bad("min_quote_vol 不能为负");
    if (c.min_age_days < 0)                              return bad("min_age_days 不能为负");
    if (!(c.atr_pct_min >= 0 && c.atr_pct_max > c.atr_pct_min))
        return bad("ATR% 区间不合法（要求 0 ≤ 下限 < 上限）");
    if (!(c.max_abs_chg24 > 0))                          return bad("max_abs_chg24 必须大于 0");
    if (!(c.max_abs_funding > 0))                        return bad("max_abs_funding 必须大于 0");
    if (!(c.max_tick_pct > 0))                           return bad("max_tick_pct 必须大于 0");
    if (c.long_n < 0 || c.long_n > 200 || c.short_n < 0 || c.short_n > 200)
        return bad("名单大小要在 0~200 之间");
    if (!(c.min_score >= 0 && c.min_score <= 100))       return bad("min_score 要在 0~100 之间");
    if (!(c.drop_rank_factor >= 1))                      return bad("drop_rank_factor 不能小于 1");
    if (c.drop_after_scans < 1)                          return bad("drop_after_scans 至少为 1");
    if (c.min_residence_h < 0)                           return bad("min_residence_h 不能为负");
    if (c.max_positions < 0)                             return bad("max_positions 不能为负");
    if (!(c.daily_loss_limit >= 0) || !(c.float_loss_limit >= 0))
        return bad("亏损限额不能为负（0 = 关）");
    if (c.prio_reserve < 0)                              return bad("prio_reserve 不能为负");
    const auto& tm = c.tmpl;
    if (tm.interval != "1h" && tm.interval != "2h" && tm.interval != "4h")
        return bad("模板周期只支持 1h / 2h / 4h（扫描按 1h K线打分）");
    if (!(tm.budget_usdt > 0))                           return bad("模板 budget_usdt 必须大于 0");
    if (tm.leverage < 1 || tm.leverage > 125)            return bad("模板杠杆要在 1~125 之间");
    if (tm.size_mode == TrendConfig::SizeMode::RiskBased && !(tm.risk_usdt > 0))
        return bad("模板按风险定仓时 risk_usdt 必须大于 0");
    if (tm.rule.donchian_period < 2 || tm.rule.atr_period < 2)
        return bad("模板通道周期与 ATR 周期至少为 2");
    if (!(tm.rule.atr_mult > 0))                         return bad("模板 atr_mult 必须大于 0");
    if (!(tm.rule.squeeze_box_atr >= 0) || !(tm.rule.breakout_vol_ratio >= 0) ||
        !(tm.rule.trend_er_min >= 0 && tm.rule.trend_er_min <= 1))
        return bad("模板过滤阈值不合法");
    if (!(tm.disaster_stop_buffer_pct > 0))              return bad("模板硬止损缓冲必须大于 0");
    cfg = c;
    return true;
}

} // namespace ccbot::smart
