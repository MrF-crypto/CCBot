// 智能模式纯逻辑测试：门槛、特征、打分排名、候选池增删、配置往返。不连网络。
#include "core/smart_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace ccbot;
using namespace ccbot::smart;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static constexpr int64_t kDay = 24LL * 3600 * 1000;
static constexpr int64_t kNow = 1'800'000'000'000LL;

static SymbolMeta meta(const std::string& sym) {
    SymbolMeta m;
    m.symbol = sym;
    m.base = sym.substr(0, sym.size() - 4);
    m.quote = "USDT";
    m.contract_type = "PERPETUAL";
    m.status = "TRADING";
    m.onboard_ms = kNow - 100 * kDay;
    m.delivery_ms = 4133404800000LL;
    m.tick = 0.0001;
    return m;
}

static void test_gates() {
    Config c;
    Ticker24 t{20e6, 5, 1.0};
    const double f = 0.01;
    check(pre_gate(meta("AAAUSDT"), &t, &f, c, kNow, false).empty(), "门槛：正常币通过");

    auto m = meta("AAAUSDT"); m.quote = "USDC";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "USDT"), "门槛：非 USDT 计价");
    m = meta("AAAUSDT"); m.contract_type = "CURRENT_QUARTER";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "永续"), "门槛：非永续");
    m = meta("AAAUSDT"); m.delivery_ms = kNow + 10 * kDay;
    check(has(pre_gate(m, &t, &f, c, kNow, false), "下架") && delisting(m, kNow),
          "门槛：交割日期被改到近期 = 即将下架");
    m = meta("AAAUSDT"); m.status = "SETTLING";
    check(delisting(m, kNow), "下架：结算中");
    m = meta("AAAUSDT"); m.status = "PENDING_TRADING";
    check(has(pre_gate(m, &t, &f, c, kNow, false), "状态"), "门槛：非交易中");
    check(has(pre_gate(meta("BTCUSDT"), &t, &f, c, kNow, false), "内置"), "门槛：BTC 内置排除");
    check(has(pre_gate(meta("USDCUSDT"), &t, &f, c, kNow, false), "内置"), "门槛：稳定币内置排除");
    Config cb = c; cb.blacklist = {"AAAUSDT"};
    check(has(pre_gate(meta("AAAUSDT"), &t, &f, cb, kNow, false), "拉黑"), "门槛：拉黑");
    m = meta("AAAUSDT"); m.onboard_ms = kNow - 5 * kDay;
    check(has(pre_gate(m, &t, &f, c, kNow, false), "上线不满 14 天"), "门槛：上线不满 14 天");
    m = meta("AAAUSDT"); m.tick = 0.01;   // 0.01 / 1.0 = 1%
    check(has(pre_gate(m, &t, &f, c, kNow, false), "精度"), "门槛：价格精度太粗");

    Ticker24 low{4.5e6, 5, 1.0};
    check(has(pre_gate(meta("AAAUSDT"), &low, &f, c, kNow, false), "成交额"), "门槛：成交额 4.5M < 5M");
    check(pre_gate(meta("AAAUSDT"), &low, &f, c, kNow, true).empty(),
          "  已在池里：按 80% 判（4.5M ≥ 4M）不淘汰——滞回");
    Ticker24 pump{20e6, 65, 1.0};
    check(has(pre_gate(meta("AAAUSDT"), &pump, &f, c, kNow, false), "涨跌"), "门槛：24h 涨 65% 不新加");
    check(pre_gate(meta("AAAUSDT"), &pump, &f, c, kNow, true).empty(), "  已在池里的不因此淘汰");
    const double hot = 0.15;
    check(has(pre_gate(meta("AAAUSDT"), &t, &hot, c, kNow, false), "资金费率"), "门槛：资金费率 0.15% 过高");
    check(pre_gate(meta("AAAUSDT"), nullptr, nullptr, c, kNow, false).empty(),
          "门槛：行情/费率没拿到时不据此判不合格（由调用方归为未知）");

    Features fe; fe.ok = true; fe.atr_pct = 0.5;
    check(has(atr_gate(fe, c, false), "太低"), "ATR% 0.5 < 0.8 太低");
    fe.atr_pct = 0.7;
    check(atr_gate(fe, c, true).empty(), "  已在池里放宽 20%（0.64）");
    fe.atr_pct = 7;
    check(has(atr_gate(fe, c, false), "太高"), "ATR% 7 > 6 太高");
}

// 一串收盘价生成K线：高低各 ±0.5，量固定
static std::vector<Bar> bars_from(const std::vector<double>& closes, double vol = 100) {
    std::vector<Bar> v;
    double prev = closes.front();
    for (size_t i = 0; i < closes.size(); ++i) {
        Bar b;
        b.open_ms = (int64_t)i * 3600000;
        b.open = prev; b.close = closes[i];
        b.high = std::max(b.open, b.close) + 0.5;
        b.low  = std::min(b.open, b.close) - 0.5;
        b.volume = vol;
        v.push_back(b);
        prev = closes[i];
    }
    return v;
}

static void test_features() {
    check(!compute_features(bars_from(std::vector<double>(100, 100)), {}).ok, "特征：K线不足 170 根");

    // 前面来回震荡，最后 12 根窄幅，最后 6 根放量
    std::vector<double> cl;
    for (int i = 0; i < 200; ++i) cl.push_back(100 + ((i % 2) ? 3.0 : -3.0));
    for (int i = 0; i < 12; ++i) cl.push_back(100 + ((i % 2) ? 0.2 : -0.2));
    auto b = bars_from(cl);
    for (size_t i = b.size() - 6; i < b.size(); ++i) b[i].volume = 400;
    const auto f = compute_features(b, {});
    check(f.ok, "特征：数据足够时可算");
    check(f.box12_atr < f.box48_atr, "  最近 12 根比 48 根窄");
    check(f.atr_ratio > 0, "  波动率压缩比可算");
    check(f.vol_lift > 3.5, "  最近 6 根放量约 4 倍（相对 168 根均量）");
    check(f.pos12 >= 0 && f.pos12 <= 1, "  箱体内位置在 0~1");

    // 一路上涨 vs BTC 不动：相对强弱为正、效率比为正
    std::vector<double> up, flat;
    for (int i = 0; i < 200; ++i) { up.push_back(100 + i * 0.5 + ((i % 3) ? 0.3 : -0.3)); flat.push_back(100); }
    const auto fu = compute_features(bars_from(up), bars_from(flat));
    check(fu.ok && fu.rs > 0 && fu.er24 > 0.5, "特征：稳步上涨 ⇒ 相对强弱为正、效率比高");
    std::vector<double> dn;
    for (int i = 0; i < 200; ++i) dn.push_back(300 - i * 0.5 + ((i % 3) ? 0.3 : -0.3));
    const auto fd = compute_features(bars_from(dn), bars_from(flat));
    check(fd.ok && fd.rs < 0 && fd.er24 < -0.5, "特征：稳步下跌 ⇒ 相对强弱为负、效率比为负");
}

static Features feat(double rs, double er, double b12, double ar, double b48, double vl, double pos) {
    Features f;
    f.ok = true; f.atr_pct = 2;
    f.rs = rs; f.er24 = er; f.box12_atr = b12; f.atr_ratio = ar; f.box48_atr = b48;
    f.vol_lift = vl; f.pos12 = pos;
    return f;
}

static void test_rank() {
    Config c;
    c.min_score = 0;
    std::map<std::string, Features> e;
    e["STRONGUSDT"] = feat( 2.0,  0.8, 3, 0.8, 8, 2.0, 0.9);   // 强势、靠上沿、放量
    e["WEAKUSDT"]   = feat(-2.0, -0.8, 3, 0.8, 8, 2.0, 0.1);   // 弱势、靠下沿、放量
    e["MEHUSDT"]    = feat( 0.0,  0.0, 6, 1.2, 15, 0.8, 0.5);  // 平庸、箱体宽、缩量
    auto r = rank(e, c);
    check(r.longs.size() == 3 && r.longs.front() == "STRONGUSDT", "排名：做多名单第一是强势币");
    check(r.shorts.front() == "WEAKUSDT", "排名：做空名单第一是弱势币");
    check(r.rank_long("MEHUSDT") == 3 && r.rank_short("MEHUSDT") == 3, "排名：平庸币两边都垫底");
    check(r.rank_long("NOPEUSDT") == 0, "排名：不在名单返回 0");
    // STRONG 与 WEAK 在箱体/压缩/横盘/量能四项并列第一，并列取平均名次 ⇒ 75 分；
    // 强弱、效率比、位置三项独占第一 ⇒ 100。合计 (30+15+10)×100 + (10+10+10+15)×75 = 8875
    check(near(r.rows["STRONGUSDT"].long_score, 88.75), "  手算核对：强势币做多 88.75 分");
    // MEH：强弱/效率比/位置居中 50，其余四项垫底 0 ⇒ (30+15+10)×50 = 2750
    check(near(r.rows["MEHUSDT"].long_score, 27.5), "  手算核对：平庸币 27.5 分");
    check(near(r.rows["WEAKUSDT"].short_score, 88.75), "  做空对称：弱势币做空 88.75 分");

    Config c2; c2.min_score = 50;
    auto r2 = rank(e, c2);
    check(r2.longs.size() == 1 && r2.shorts.size() == 1, "最低分 50：平庸币进不了任何名单");

    std::map<std::string, Features> one;
    one["ONLYUSDT"] = feat(0, 0, 4, 1, 10, 1, 0.5);
    check(near(rank(one, c).rows["ONLYUSDT"].long_score, 100.0), "只有一个合格币：百分位 100");
    std::map<std::string, Features> tie;
    tie["AUSDT"] = feat(1, 0, 4, 1, 10, 1, 0.5);
    tie["BUSDT"] = feat(1, 0, 4, 1, 10, 1, 0.5);
    auto rt = rank(tie, c);
    check(near(rt.rows["AUSDT"].long_score, rt.rows["BUSDT"].long_score) && rt.longs.front() == "AUSDT",
          "并列：分数相同、按名字排，结果确定");
}

// 造一个名单：前 n 个做多、后 m 个做空
static ScanOutcome scan_with(const std::vector<std::string>& longs,
                             const std::vector<std::string>& shorts) {
    ScanOutcome s;
    s.ok = true;
    s.ranking.longs = longs;
    s.ranking.shorts = shorts;
    double sc = 99;
    for (const auto& x : longs)  { s.ranking.rows[x].symbol = x; s.ranking.rows[x].long_score = sc; sc -= 1; }
    sc = 98.5;
    for (const auto& x : shorts) { s.ranking.rows[x].symbol = x; s.ranking.rows[x].short_score = sc; sc -= 1; }
    return s;
}
static BotView bot(const std::string& id, const std::string& sym, bool is_auto, bool pos = false,
                   bool stopped = false) {
    return BotView{id, sym, is_auto, pos, stopped};
}
static bool removed(const Plan& p, const std::string& sym) {
    for (const auto& r : p.remove) if (r.symbol == sym) return true;
    return false;
}
static bool added(const Plan& p, const std::string& sym) {
    for (const auto& a : p.add) if (a.symbol == sym) return true;
    return false;
}

static void test_pool() {
    Config c;
    c.long_n = 2; c.short_n = 1;

    {
        Pool pool;
        auto s = scan_with({"L1USDT", "L2USDT", "L3USDT"}, {"S1USDT"});
        auto p = pool.plan(s, {bot("m1", "L2USDT", false)}, c, kNow);
        check(added(p, "L1USDT") && added(p, "S1USDT"), "补位：两份名单的前 N 名");
        check(!added(p, "L2USDT"), "  手动 bot 占着的品种不加");
        check(!added(p, "L3USDT"), "  第 3 名不在前 2，不加");
        check(p.add.size() == 2, "  容量 3 里手动 bot 不占名额，但 L2 被跳过后只剩 2 个可加");
    }
    {
        Pool pool;
        ScanOutcome bad = scan_with({"L1USDT"}, {});
        bad.ok = false;
        auto p = pool.plan(bad, {bot("a1", "XUSDT", true)}, c, kNow);
        check(p.add.empty() && p.remove.empty(), "整轮扫描失败：一个都不动");
    }
    {
        // 持仓中的绝不移除；下架告警
        Pool pool;
        auto s = scan_with({"L1USDT"}, {});
        s.hard_fail["HOLDUSDT"] = "即将下架";
        s.delisting.insert("HOLDUSDT");
        auto p = pool.plan(s, {bot("a1", "HOLDUSDT", true, /*pos*/ true)}, c, kNow);
        check(!removed(p, "HOLDUSDT"), "持仓中：不合格也不移除，等这笔走完");
        bool leaving = false;
        for (const auto& m : p.meta) if (m.bot_id == "a1") leaving = m.leaving;
        check(leaving, "  标成待移出");
        check(p.alerts.size() == 1 && has(p.alerts[0], "下架"), "  持仓中即将下架：醒目告警");
    }
    {
        // 硬门槛不合格：空仓立即移除，不看保护期
        Pool pool;
        pool.on_added("BADUSDT", kNow);
        auto s = scan_with({"L1USDT"}, {});
        s.hard_fail["BADUSDT"] = "24h 成交额不足";
        auto p = pool.plan(s, {bot("a1", "BADUSDT", true)}, c, kNow + 3600000);
        check(removed(p, "BADUSDT"), "硬门槛不合格：空仓立即移除（无视 24 小时保护期）");
    }
    {
        // 没拿到数据：什么都不做
        Pool pool;
        pool.on_added("UNKUSDT", kNow - 10 * kDay);
        auto s = scan_with({"L1USDT"}, {});
        s.unknown.insert("UNKUSDT");
        for (int i = 0; i < 5; ++i) {
            auto p = pool.plan(s, {bot("a1", "UNKUSDT", true)}, c, kNow);
            check(!removed(p, "UNKUSDT"), "这一轮没拿到数据：不移除（没拿到 ≠ 不合格）#" + std::to_string(i));
        }
        check(pool.entries().at("UNKUSDT").miss_scans == 0, "  也不累计掉出次数");
    }
    {
        // 掉出名单：连续 2 轮且满 24 小时
        Pool pool;
        pool.on_added("OUTUSDT", kNow);
        auto s = scan_with({"L1USDT", "L2USDT"}, {"S1USDT"});
        auto p1 = pool.plan(s, {bot("a1", "OUTUSDT", true)}, c, kNow + 25 * 3600000LL);
        check(!removed(p1, "OUTUSDT"), "掉出第 1 轮：不移除");
        auto p2 = pool.plan(s, {bot("a1", "OUTUSDT", true)}, c, kNow + 26 * 3600000LL);
        check(removed(p2, "OUTUSDT"), "掉出第 2 轮且满 24 小时：移除");

        Pool young;
        young.on_added("NEWUSDT", kNow);
        young.plan(s, {bot("a1", "NEWUSDT", true)}, c, kNow + 3600000);
        auto p3 = young.plan(s, {bot("a1", "NEWUSDT", true)}, c, kNow + 2 * 3600000);
        check(!removed(p3, "NEWUSDT"), "加入不满 24 小时：掉出 2 轮也不移除");

        Pool back;
        back.on_added("BACKUSDT", kNow);
        back.plan(s, {bot("a1", "BACKUSDT", true)}, c, kNow + 25 * 3600000LL);
        auto sb = scan_with({"L1USDT", "BACKUSDT"}, {});
        back.plan(sb, {bot("a1", "BACKUSDT", true)}, c, kNow + 26 * 3600000LL);
        check(back.entries().at("BACKUSDT").miss_scans == 0, "回到名单：掉出计数清零（必须连续）");
    }
    {
        // 1.5N 的缓冲区：第 3 名（N=2，1.5N=3）不算掉出
        Pool pool;
        pool.on_added("L3USDT", kNow);
        auto s = scan_with({"L1USDT", "L2USDT", "L3USDT", "L4USDT"}, {});
        pool.plan(s, {bot("a1", "L3USDT", true)}, c, kNow + 30 * 3600000LL);
        auto p = pool.plan(s, {bot("a1", "L3USDT", true)}, c, kNow + 31 * 3600000LL);
        check(!removed(p, "L3USDT"), "排名在 N 和 1.5N 之间：保留（不对称门槛）");
        bool tagged = false;
        for (const auto& m : p.meta) if (m.bot_id == "a1") tagged = (m.list == 'L' && m.rank == 3);
        check(tagged, "  名单信息：做多第 3");
    }
    {
        // 已停止的自动 bot：不替用户删
        Pool pool;
        pool.on_added("STOPUSDT", kNow - 10 * kDay);
        auto s = scan_with({"L1USDT"}, {});
        s.hard_fail["STOPUSDT"] = "成交额不足";
        auto p = pool.plan(s, {bot("a1", "STOPUSDT", true, false, /*stopped*/ true)}, c, kNow);
        check(!removed(p, "STOPUSDT"), "已停止的自动 bot（你或对账停的）：不自动删");
    }
    {
        // 容量满了不再加；只勾做多时不加做空名单
        Config cl = c; cl.dir_short = false;
        Pool pool;
        auto s = scan_with({"L1USDT", "L2USDT"}, {"S1USDT"});
        auto p = pool.plan(s, {bot("a1", "L1USDT", true), bot("a2", "L2USDT", true)}, cl, kNow);
        check(p.add.empty(), "只勾做多：容量 2 已满、做空名单不看");
        check(bot_direction(cl) == trend::Direction::LongOnly, "只勾做多：bot 方向仅多");
        Config cb = c;
        check(bot_direction(cb) == trend::Direction::Both, "两个都勾：bot 方向多空");
    }
    {
        Pool pool;
        pool.on_added("AUSDT", 123);
        pool.on_added("BUSDT", 456);
        Pool p2;
        check(p2.from_json(pool.to_json()) && p2.entries().size() == 2 &&
              p2.entries().at("BUSDT").added_ms == 456, "候选池 JSON 往返");
        check(!p2.from_json("{坏的"), "  坏 JSON 返回 false");
    }
}

static void test_config_json() {
    Config c;
    c.enabled = true;
    c.dir_short = false;
    c.min_quote_vol = 8e6;
    c.blacklist = {"XUSDT", "YUSDT"};
    c.long_n = 40;
    c.daily_loss_limit = 123.5;
    c.tmpl.rule.atr_mult = 2.5;
    c.tmpl.size_mode = TrendConfig::SizeMode::RiskBased;
    c.tmpl.risk_usdt = 7;
    Config d;
    std::string err;
    check(config_from_json(config_to_json(c), d, &err), "配置 JSON 往返：能读回");
    check(d.enabled && !d.dir_short && near(d.min_quote_vol, 8e6) && d.blacklist.size() == 2 &&
          d.long_n == 40 && near(d.daily_loss_limit, 123.5), "  各项一致");
    check(near(d.tmpl.rule.atr_mult, 2.5) && d.tmpl.size_mode == TrendConfig::SizeMode::RiskBased &&
          near(d.tmpl.risk_usdt, 7), "  模板一致");

    Config e;
    check(config_from_json("{\"long_n\":10}", e, &err) && e.long_n == 10 && e.short_n == 50,
          "缺的键保持默认值");
    Config f;
    check(!config_from_json("{\"atr_pct_min\":5,\"atr_pct_max\":2}", f, &err) && has(err, "ATR%"),
          "非法值整份拒绝");
    check(f.atr_pct_min == 0.8, "  拒绝时原配置不动");
    check(!config_from_json("{\"template\":{\"interval\":\"5m\"}}", f, &err), "模板周期只认 1h/2h/4h");

    const auto t = make_bot_config(c, "ABCUSDT");
    check(t.symbol == "ABCUSDT" && t.rule.strategy == trend::Strategy::Turtle &&
          t.rule.reverse == trend::ReverseMode::None && t.use_disaster_stop &&
          t.rule.direction == trend::Direction::LongOnly,
          "bot 配置：海龟、不反手、硬止损强制开、方向跟勾选");
    const auto dt = Config::default_template();
    check(dt.interval == "1h" && dt.rule.donchian_period == 12 && dt.rule.lock_atr && dt.rule.accel_exit &&
          near(dt.rule.squeeze_box_atr, 4) && near(dt.rule.trend_er_min, 0.4) &&
          near(dt.rule.breakout_vol_ratio, 1.5), "默认模板：1h、通道 12、过滤与出场优化都开");
}

int main() {
    test_gates();
    test_features();
    test_rank();
    test_pool();
    test_config_json();
    if (g_fail == 0) {
        std::printf("OK: 智能模式测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
