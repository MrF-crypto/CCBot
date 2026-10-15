// 智能模式纯逻辑测试：门槛、特征、打分排名、候选池增删、配置往返。不连网络。
#include "core/smart_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
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

    Features fe; fe.ok = true; fe.atr_pct = 0.4;
    check(has(atr_gate(fe, c, false), "太低"), "ATR% 0.4 < 0.5 太低");
    fe.atr_pct = 0.45;
    check(atr_gate(fe, c, true).empty(), "  已在池里放宽 20%（0.4）");
    fe.atr_pct = 2.2;
    check(has(atr_gate(fe, c, false), "太高"), "ATR% 2.2 > 2 太高（回测 > 2% 期望≈0）");
    check(atr_gate(fe, c, true).empty(), "  已在池里放宽 20%（2.4）");
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
    f.kappa12 = b12 / std::sqrt(12.0);   // κ ≈ 箱体/ATR ÷ √12（价格与下沿的差别忽略）
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
    // 12 根箱体按 κ 区间打分：STRONG / WEAK 箱体 3 ATR ⇒ κ≈0.87（0.8~1.2）满分 100；
    // MEH 箱体 6 ATR ⇒ κ≈1.73（已扩张）0 分。
    // 压缩/横盘/量能三项 STRONG 与 WEAK 并列第一，并列取平均名次 ⇒ 75；
    // 强弱、效率比、位置三项独占第一 ⇒ 100。
    // 合计 (30+15+10)×100 + 10×100 + (10+10+15)×75 = 9125
    check(near(r.rows["STRONGUSDT"].long_score, 91.25), "  手算核对：强势币做多 91.25 分");
    // MEH：强弱/效率比/位置居中 50，κ 0 分，其余三项垫底 0 ⇒ (30+15+10)×50 = 2750
    check(near(r.rows["MEHUSDT"].long_score, 27.5), "  手算核对：平庸币 27.5 分");
    check(near(r.rows["WEAKUSDT"].short_score, 91.25), "  做空对称：弱势币做空 91.25 分");

    // 箱体极窄（κ < 0.5，假突破重灾区）不再因为"越窄越好"排到前面
    std::map<std::string, Features> k;
    k["TIGHTUSDT"] = feat(0, 0, 1.2, 1, 10, 1, 0.5);   // κ≈0.35
    k["GOODUSDT"]  = feat(0, 0, 3.5, 1, 10, 1, 0.5);   // κ≈1.01
    const auto rk = rank(k, c);
    check(rk.rows.at("GOODUSDT").long_score - rk.rows.at("TIGHTUSDT").long_score > 9.9,
          "  κ 按区间打分：κ≈1.0 比 κ≈0.35（箱体极窄）高 10 分");

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
    c.dir_mode = DirMode::Breakout;   // 这一组测的是突破模式的两份名单；混合模式见 test_mixed
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
        bool meta_rank0 = false;
        for (const auto& m : p.meta) if (m.bot_id == "a1") meta_rank0 = (m.rank == 0);
        check(meta_rank0, "  名次清零（界面标「待移出」）");
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
        // 批量淘汰保护：过半同时出局 ⇒ 疑似数据异常，这一轮一个都不删
        Pool pool;
        std::vector<BotView> bots;
        auto s = scan_with({"L1USDT"}, {});
        for (int i = 0; i < 6; ++i) {
            const std::string sym = "M" + std::to_string(i) + "USDT";
            pool.on_added(sym, kNow - 10 * kDay);
            bots.push_back(bot("a" + std::to_string(i), sym, true));
            if (i < 5) s.hard_fail[sym] = "状态 ";   // 6 个里 5 个被判不合格
        }
        auto p = pool.plan(s, bots, c, kNow);
        check(p.remove.empty(), "6 个里 5 个同时被判出局：疑似数据异常，一个都不删");
        bool warned = false;
        for (const auto& a : p.alerts) if (has(a, "疑似数据异常")) warned = true;
        check(warned, "  告警说明原因");

        Pool pool2;
        std::vector<BotView> bots2;
        auto s2 = scan_with({"L1USDT"}, {});
        for (int i = 0; i < 6; ++i) {
            const std::string sym = "N" + std::to_string(i) + "USDT";
            pool2.on_added(sym, kNow - 10 * kDay);
            bots2.push_back(bot("b" + std::to_string(i), sym, true));
        }
        s2.hard_fail["N0USDT"] = "24h 成交额不足";
        s2.ranking.longs = {"N1USDT", "N2USDT", "N3USDT", "N4USDT", "N5USDT"};
        for (const auto& x : s2.ranking.longs) s2.ranking.rows[x].long_score = 80;
        Config c6 = c; c6.long_n = 6;
        auto p2 = pool2.plan(s2, bots2, c6, kNow);
        check(p2.remove.size() == 1 && removed(p2, "N0USDT"), "只有个别出局：照常移除");
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

// ── 混合模式：分段、覆盖规则、缓冲区、名额、方向 ─────────────────────────
static Scored sc(double rs_pct, double rs, double er = 0, double up = 0, double dn = 0) {
    Scored s;
    s.rs_pct = rs_pct;
    s.long_score = s.short_score = s.squeeze_score = 80;
    s.f.ok = true;
    s.f.rs = rs; s.f.er24 = er; s.f.max_up24 = up; s.f.max_dn24 = dn;
    return s;
}

static void test_mixed() {
    Config c;   // 默认就是混合模式
    check(c.dir_mode == DirMode::Mixed && c.strong_n == 20 && c.weak_n == 20 &&
          c.mid_n == 50 && c.hot_n == 10, "默认：混合模式，名额 20/20/50/10");

    // ── 分段 ──
    SegState st;
    check(classify(sc(85, 1.2), c, st) == kSegStrong, "排名前 30% 且跑赢 BTC ⇒ 强势");
    st = {};
    check(classify(sc(85, -0.1), c, st) == kSegMid, "排名靠前但跑输 BTC（全市场都弱）⇒ 不算强势");
    st = {};
    check(classify(sc(10, -1.0), c, st) == kSegWeak, "排名后 30% 且跑输 BTC ⇒ 弱势");
    st = {};
    check(classify(sc(50, 0.1), c, st) == kSegMid, "中间 ⇒ 中间段");
    st = {};
    check(classify(sc(95, 3.0, 0.5, 90), c, st) == kSegHot, "7 天内单日涨 90% ⇒ 过热（不管排名多高）");
    st = {};
    check(classify(sc(5, -3.0, -0.5, 0, 60), c, st) == kSegMid, "7 天内单日跌 60% ⇒ 中间段，不进弱势");
    st = {};
    check(classify(sc(85, 1.2, -0.5), c, st) == kSegMid, "强势但 24 根效率比 −0.5（掉头）⇒ 降为中间段");
    check(st.base == kSegStrong, "  掉头只改这一轮的方向，底段仍是强势（缓冲区照常）");
    st = {};
    check(classify(sc(10, -1.0, 0.5), c, st) == kSegMid, "弱势但在反弹 ⇒ 降为中间段");

    // ── 缓冲区：进 30%、出 40%、连续 2 轮 ──
    st = {};
    classify(sc(80, 1.0), c, st);
    check(classify(sc(65, 0.8), c, st) == kSegStrong, "已在强势、掉到 65%（仍在前 40%）⇒ 留在强势");
    check(classify(sc(55, 0.5), c, st) == kSegStrong, "掉出前 40% 第 1 轮 ⇒ 还留着");
    check(classify(sc(55, 0.5), c, st) == kSegMid, "连续第 2 轮 ⇒ 离开强势");
    st = {};
    check(classify(sc(65, 0.8), c, st) == kSegMid, "新币在 65%：没进前 30%，不进强势（进出门槛不对称）");
    st = {};
    classify(sc(80, 1.0), c, st);
    classify(sc(55, 0.5), c, st);                     // 出段第 1 轮
    classify(sc(70, 0.9), c, st);                     // 回来了，计数清零
    check(classify(sc(55, 0.5), c, st) == kSegStrong, "出段要【连续】2 轮，中间回来过就重新数");
    st = {};
    classify(sc(80, 1.0), c, st);
    check(classify(sc(80, -0.2), c, st) == kSegStrong, "强势币跑输 BTC 第 1 轮 ⇒ 还留着");
    check(classify(sc(80, -0.2), c, st) == kSegMid, "  第 2 轮 ⇒ 离开");

    // ── 方向与勾选 ──
    bool ok = false;
    check(seg_direction(kSegStrong, c, &ok) == trend::Direction::LongOnly && ok, "强势 ⇒ 仅多");
    check(seg_direction(kSegWeak, c, &ok) == trend::Direction::ShortOnly && ok, "弱势 ⇒ 仅空");
    check(seg_direction(kSegHot, c, &ok) == trend::Direction::ShortOnly && ok, "过热 ⇒ 仅空");
    check(seg_direction(kSegMid, c, &ok) == trend::Direction::Both && ok, "中间 ⇒ 多空，谁突破做谁");
    Config cl = c; cl.dir_short = false;
    seg_direction(kSegWeak, cl, &ok);
    check(!ok && seg_quota(kSegWeak, cl) == 0 && seg_quota(kSegHot, cl) == 0,
          "只勾做多：弱势段、过热段不能做，名额为 0");
    check(seg_direction(kSegMid, cl, &ok) == trend::Direction::LongOnly && ok, "只勾做多：中间段只做多");

    // ── 名单：每段按名额补、方向跟段 ──
    Ranking r;
    auto put = [&](const std::string& s, Scored x) { x.symbol = s; r.rows[s] = x; };
    put("STRONGUSDT", sc(90, 1.5));
    put("WEAKUSDT",   sc(5, -1.5));
    put("MIDUSDT",    sc(50, 0.0));
    put("HOTUSDT",    sc(99, 4.0, 0, 120));
    Config c1 = c; c1.strong_n = c1.weak_n = c1.mid_n = c1.hot_n = 1;
    Pool pool;
    ScanOutcome s; s.ok = true; s.ranking = r;
    auto p = pool.plan(s, {}, c1, kNow);
    auto dir_of = [&](const std::string& sym) {
        for (const auto& a : p.add) if (a.symbol == sym) return std::make_pair(a.list, a.dir);
        return std::make_pair(char(0), trend::Direction::Both);
    };
    check(p.add.size() == 4, "四段各补 1 个");
    check(dir_of("STRONGUSDT") == std::make_pair(kSegStrong, trend::Direction::LongOnly), "  强势币：仅多");
    check(dir_of("WEAKUSDT") == std::make_pair(kSegWeak, trend::Direction::ShortOnly), "  弱势币：仅空");
    check(dir_of("MIDUSDT") == std::make_pair(kSegMid, trend::Direction::Both), "  中间币：多空");
    check(dir_of("HOTUSDT") == std::make_pair(kSegHot, trend::Direction::ShortOnly), "  过热币：仅空");

    // 池里的币换了段：给出新方向（空仓时由执行层切换）
    for (const auto& a : p.add) pool.on_added(a.symbol, kNow - 10 * kDay, a.list);
    Ranking r2 = r;
    r2.rows["STRONGUSDT"].f.er24 = -0.6;   // 强势币开始掉头
    ScanOutcome s2; s2.ok = true; s2.ranking = r2;
    auto p2 = pool.plan(s2, {bot("a1", "STRONGUSDT", true)}, c1, kNow);
    bool switched = false;
    for (const auto& m : p2.meta)
        if (m.bot_id == "a1") switched = m.has_dir && m.dir == trend::Direction::Both && m.list == kSegMid;
    check(switched, "强势币掉头：本轮分到中间段，方向改为多空");

    // 分段状态跨重启
    Pool p3;
    check(p3.from_json(pool.to_json()) && p3.entries().at("STRONGUSDT").seg.base == kSegStrong,
          "分段状态（底段）随候选池落盘");
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
    c.dir_mode = DirMode::Breakout;
    c.mid_n = 33;
    c.hot_pct = 70;
    c.weak_vol_ratio = 0.8;
    Config d;
    std::string err;
    check(config_from_json(config_to_json(c), d, &err), "配置 JSON 往返：能读回");
    check(d.enabled && !d.dir_short && near(d.min_quote_vol, 8e6) && d.blacklist.size() == 2 &&
          d.long_n == 40 && near(d.daily_loss_limit, 123.5), "  各项一致");
    check(near(d.tmpl.rule.atr_mult, 2.5) && d.tmpl.size_mode == TrendConfig::SizeMode::RiskBased &&
          near(d.tmpl.risk_usdt, 7), "  模板一致");
    check(d.dir_mode == DirMode::Breakout && d.mid_n == 33 && near(d.hot_pct, 70) &&
          near(d.weak_vol_ratio, 0.8), "  混合模式相关设置一致");
    Config g;
    check(!config_from_json("{\"dir_mode\":\"sideways\"}", g, &err), "方向模式拼错：拒绝");
    check(!config_from_json("{\"seg_enter_pct\":45,\"seg_exit_pct\":40}", g, &err),
          "进段门槛大于出段门槛：拒绝（缓冲区反了）");

    Config e;
    check(config_from_json("{\"long_n\":10}", e, &err) && e.long_n == 10 && e.short_n == 50,
          "缺的键保持默认值");
    Config f;
    check(!config_from_json("{\"atr_pct_min\":5,\"atr_pct_max\":2}", f, &err) && has(err, "ATR%"),
          "非法值整份拒绝");
    check(f.atr_pct_min == 0.5, "  拒绝时原配置不动");
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
    check(dt.rule.vol_paced && dt.rule.fail_exit && !dt.rule.fail_exit_short,
          "  量比按时间折算开；假突破提前出场默认只判多单");
    Config def;
    check(near(def.atr_pct_min, 0.5) && near(def.atr_pct_max, 2.0), "  1h ATR% 区间默认 0.5~2");
    check(!dt.rule.roi_trail, "  收益率追踪止盈默认关");
    Config roi;
    roi.tmpl.rule.roi_trail = true; roi.tmpl.rule.roi_trail_start = 12; roi.tmpl.rule.roi_trail_back = 4;
    roi.bare.roi_trail = true; roi.bare.roi_start = 20; roi.bare.roi_back = 8;
    Config roi2;
    std::string e3;
    check(config_from_json(config_to_json(roi), roi2, &e3) && roi2.tmpl.rule.roi_trail &&
              near(roi2.tmpl.rule.roi_trail_start, 12) && near(roi2.tmpl.rule.roi_trail_back, 4) &&
              roi2.bare.roi_trail && near(roi2.bare.roi_start, 20) && near(roi2.bare.roi_back, 8),
          "  追踪止盈随海龟模板、裸K 配置往返（两套各自一份）");
    const auto bb = make_bare_bot_config(roi2, "XYZUSDT");
    check(bb.rule.roi_trail && near(bb.rule.roi_trail_start, 20) && near(bb.rule.roi_trail_back, 8),
          "  裸K 的 bot 用裸K 自己的追踪止盈设置，不跟海龟模板");
    check(!config_from_json("{\"template\":{\"roi_trail_back\":0}}", roi2, &e3), "  回撤 0 个点拒绝");

    Config rt;
    rt.tmpl.rule.vol_paced = false;
    rt.tmpl.rule.fail_exit = false;
    rt.tmpl.rule.fail_exit_short = true;
    Config rt2;
    std::string e2;
    check(config_from_json(config_to_json(rt), rt2, &e2) && !rt2.tmpl.rule.vol_paced &&
              !rt2.tmpl.rule.fail_exit && rt2.tmpl.rule.fail_exit_short,
          "  这三项随模板往返");
    Config old;
    check(config_from_json("{\"template\":{\"lock_atr\":true}}", old, &e2) && old.tmpl.rule.vol_paced &&
              old.tmpl.rule.fail_exit && !old.tmpl.rule.fail_exit_short,
          "  老配置没有这些键：按默认（空单不判）");
}

// ── 智能裸K（埋伏点火）：结构特征、κ、打分、移出、点火判定、bot 配置、配置往返 ──────────
static Bar bar(int64_t t, double o, double h, double l, double c, double qv = 1000) {
    Bar b;
    b.open_ms = t; b.open = o; b.high = h; b.low = l; b.close = c; b.volume = 10; b.quote_vol = qv;
    return b;
}

// 1h：200 根在 100 附近窄幅横盘（箱体 99.5~100.5，ATR 约 1），最后一根收 last_close
static std::vector<Bar> amb_bars(double last_close = 100.2, double wick = 0.3) {
    std::vector<Bar> v;
    for (int i = 0; i < 200; ++i) {
        double o = (i % 2) ? 99.8 : 100.2, c = (i % 2) ? 100.2 : 99.8;
        if (i == 199) c = last_close;
        v.push_back(bar(i * 3600000LL, o, std::max(o, c) + wick, std::min(o, c) - wick, c,
                        c > o ? 2000 : 1000));
    }
    return v;
}

// 5m：60 根已收盘，前 59 根在 99.9/100.1 之间（ATR 约 0.6、成交额 1000），最后一根是要判的
static std::vector<Bar> ign_bars(double o, double h, double l, double c, double qv, bool red5 = false) {
    std::vector<Bar> v;
    for (int i = 0; i < 59; ++i) {
        double bo = (i % 2) ? 99.9 : 100.1, bc = (i % 2) ? 100.1 : 99.9, bh = 100.3, bl = 99.7;
        if (red5 && i >= 54) { bo = 100.3; bc = 99.9; bh = 100.4; bl = 99.8; }
        v.push_back(bar(i * 300000LL, bo, bh, bl, bc));
    }
    v.push_back(bar(59 * 300000LL, o, h, l, c, qv));
    return v;
}

static void test_bare() {
    BareConfig bc;

    // ── 结构特征 ──
    auto f = compute_ambush(amb_bars());
    check(f.ok && near(f.box_hi, 100.5) && near(f.box_lo, 99.5) && near(f.close, 100.2),
          "埋伏结构：12 根 1h 箱体上下沿");
    const double want_kappa = (1.0 / 99.5 * 100.0) / f.atr_pct / std::sqrt(12.0);
    check(near(f.kappa, want_kappa, 1e-9) && f.atr_pct > 0.8 && f.atr_pct < 1.2,
          "  κ = (箱体高度% ÷ ATR%) ÷ √12，箱体高度按下沿算");
    check(near(f.pos, 0.7, 1e-9) && f.atr_ratio > 0, "  箱体内位置 0.7，ATR 压缩比算得出");
    check(near(f.up_vol_share, 2000.0 * 6 / (2000.0 * 6 + 1000.0 * 6), 1e-9), "  上涨量占比按成交额算");
    {
        auto few = amb_bars();
        few.resize(30);
        check(!compute_ambush(few).ok, "  K线不够：不合格（不硬算）");
    }

    // ── κ 打分 ──
    check(kappa_score(1.0) == 1 && kappa_score(0.8) == 1 && kappa_score(1.2) == 1, "κ 0.8~1.2 满分");
    check(near(kappa_score(0.65), 0.5) && near(kappa_score(1.35), 0.5), "  两边线性递减");
    check(kappa_score(0.4) == 0 && kappa_score(1.6) == 0, "  < 0.5（假突破重灾区）与 > 1.5（已扩张）0 分");

    // ── 硬条件与打分 ──
    check(ambush_gate(f, bc).empty(), "ATR% 1%、位置 0.7：合格");
    const auto hot = compute_ambush(amb_bars(100.2, 1.4));
    check(hot.ok && hot.atr_pct > 2 && !ambush_gate(hot, bc).empty(), "  1h ATR% > 2：不进");
    const auto low = compute_ambush(amb_bars(99.6));
    check(low.ok && low.pos < 0.55 && !ambush_gate(low, bc).empty(), "  收在箱体下部：不进");
    AmbushFeat best = f;
    best.kappa = 1.0; best.pos = 1.0; best.atr_pct = 0.8; best.higher_low = true;
    best.up_vol_share = 0.6; best.atr_ratio = 0.8;
    check(near(ambush_score(best, bc), 100), "满分结构 100 分");
    AmbushFeat worse = best;
    worse.kappa = 0.4;
    check(near(ambush_score(worse, bc), 60), "  κ 掉到 0.4：少 40 分（κ 权重最大）");

    std::map<std::string, AmbushFeat> m;
    m["AAAUSDT"] = best;
    m["BBBUSDT"] = worse;
    m["CCCUSDT"] = f;
    m["HOTUSDT"] = hot;
    bc.pool_n = 2;
    const auto pool = select_ambush(m, bc);
    check(pool.size() == 2 && pool[0].symbol == "AAAUSDT" && pool[0].score > pool[1].score,
          "选池：不合格的剔除，按分数取前 pool_n 个");
    bc.pool_n = 40;

    // ── 待命刷新：移出 ──
    check(ambush_drop(f, 99.5, 100.2, bc).empty(), "还在箱体里：留");
    auto down = f; down.close = 99.0;
    check(has(ambush_drop(down, 99.5, 100.2, bc), "跌破箱体下沿"), "  跌破上次的箱体下沿：移出");
    auto up = f; up.close = 130;
    check(has(ambush_drop(up, 99.5, 100.0, bc), "已涨"), "  进池后涨超 25%：移出");
    check(has(ambush_drop(hot, 99.5, 100.2, bc), "ATR%"), "  ATR% 超限：移出");
    AmbushFeat bad;
    check(ambush_drop(bad, 99.5, 100.2, bc).empty(), "  这轮没算出来（数据不全）：不动它");

    // ── 点火判定（已收盘的 5m）──
    const double hi = 100.5, lo = 99.5;
    auto g = check_ignition(ign_bars(100.2, 101.5, 100.1, 101.4, 5000), hi, lo, bc);
    check(g.ok && near(g.vol_ratio, 5) && near(g.low, 100.1) && g.close_pos > 0.9 && g.upper_wick < 0.1,
          "点火：量比 5x、实体 1.2%、收在高位、上影短、贴着箱体上沿");
    check(near(g.atr, 0.6, 0.05), "  ATR 用点火【之前】的（不含点火那根）");
    g = check_ignition(ign_bars(100.2, 101.5, 100.1, 101.4, 2000), hi, lo, bc);
    check(!g.ok && !g.vetoed && has(g.why, "量比"), "量比 2x：不点火");
    // 收盘位置刚好 0.6（过线），上影 0.4（> 0.35）
    g = check_ignition(ign_bars(100.2, 102.6, 100.1, 101.6, 5000), hi, lo, bc);
    check(!g.ok && has(g.why, "上影"), "上影太长（冲上去被砸回）：不点火");
    g = check_ignition(ign_bars(100.2, 101.5, 100.1, 101.4, 5000, true), hi, lo, bc);
    check(!g.ok && g.vetoed && has(g.why, "连阴"), "否决：点火前 5 根连阴（下跌中继反抽）");
    g = check_ignition(ign_bars(100.2, 101.5, 100.1, 101.4, 5000), 110, 99.9, bc);
    check(!g.ok && g.vetoed && has(g.why, "箱体下部"), "否决：点火前在箱体下部");
    g = check_ignition(ign_bars(100.2, 100.8, 100.1, 100.7, 5000), hi, lo, bc);
    check(!g.ok && g.vetoed && has(g.why, "涨幅/ATR"), "否决：涨幅 ÷ ATR < 1.5（相对自身波动太弱）");
    // 箱体 95~104：点火前位置 0.54（不否决），收盘 101.4 离上沿 104 还有 2.5%
    g = check_ignition(ign_bars(100.2, 101.5, 100.1, 101.4, 5000), 104, 95, bc);
    check(!g.ok && has(g.why, "箱体上沿"), "离箱体上沿太远（收盘没贴到上沿）：不点火");
    check(!check_ignition(std::vector<Bar>(5), hi, lo, bc).ok, "K线不够：不判");

    // ── bot 配置 ──
    Config c;
    c.tmpl.budget_usdt = 250;
    c.tmpl.leverage    = 5;
    c.tmpl.rule.squeeze_box_atr = 4;
    const auto t = make_bare_bot_config(c, "XYZUSDT");
    check(t.symbol == "XYZUSDT" && t.interval == "1h" && t.rule.strategy == trend::Strategy::Turtle &&
          t.rule.donchian_period == 12 && t.rule.atr_period == 14 && near(t.rule.atr_mult, 3.0) &&
          t.rule.direction == trend::Direction::LongOnly,
          "裸K bot：1h、仅多、3 × ATR14 追踪");
    check(t.rule.squeeze_box_atr == 0 && t.rule.trend_er_min == 0 && t.rule.breakout_vol_ratio == 0 &&
          t.rule.reverse == trend::ReverseMode::None && t.use_disaster_stop &&
          near(t.budget_usdt, 250) && t.leverage == 5,
          "  入场过滤全关（入场不归它管）、不反手、硬止损、仓位杠杆沿用模板");
    check(ambush_excluded_base("PAXG") && ambush_excluded_base("XAUT") && !ambush_excluded_base("SOL"),
          "黄金代币不埋伏");

    // ── 配置往返与校验 ──
    Config b;
    b.strategy = Strategy::BareK;
    b.bare.pool_n = 30; b.bare.vol_ratio = 4; b.bare.atr_pct_max = 1.5; b.bare.veto_red_bars = 4;
    b.bare.min_quote_vol = 5e6; b.bare.edge_tol_pct = 0.2; b.bare.accel_exit = false;
    Config r;
    std::string err;
    check(config_from_json(config_to_json(b), r, &err) && r.strategy == Strategy::BareK &&
              r.bare.pool_n == 30 && near(r.bare.vol_ratio, 4) && near(r.bare.atr_pct_max, 1.5) &&
              r.bare.veto_red_bars == 4 && near(r.bare.min_quote_vol, 5e6) &&
              near(r.bare.edge_tol_pct, 0.2) && !r.bare.accel_exit && r.bare.lock_atr,
          "裸K 配置往返");
    Config d;
    check(config_from_json("{\"strategy\":\"bare\",\"bare\":{}}", d, &err) &&
              d.bare.pool_n == 40 && near(d.bare.vol_ratio, 3.0) && d.bare.max_positions == 10 &&
              near(d.bare.atr_pct_max, 2.0) && near(d.bare.pos_min, 0.55),
          "  没写的键取默认（池 40 / 量比 3x / ATR% ≤ 2 / 位置 ≥ 0.55）");
    check(!config_from_json("{\"strategy\":\"grid\"}", d, &err) && has(err, "strategy"), "  未知策略整份拒绝");
    check(!config_from_json("{\"bare\":{\"close_pos_min\":1.5}}", d, &err), "  收盘位置超出 0~1 拒绝");
    check(!config_from_json("{\"bare\":{\"atr_mult\":0}}", d, &err), "  追踪止损 0 ATR 拒绝");
}

int main() {
    test_bare();
    test_gates();
    test_features();
    test_rank();
    test_pool();
    test_mixed();
    test_config_json();
    if (g_fail == 0) {
        std::printf("OK: 智能模式测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
