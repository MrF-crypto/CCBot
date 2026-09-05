// headless 配置解析测试。
//
// 核心断言只有一条，但它守住的是一整类 bug：
//
//   **配置文件里只写 symbol 时，解析出来的每个字段都必须等于引擎的默认值。**
//
// 为什么这条最重要：headless 的解析器给每个键都写了一个字面量兜底，
//   c.max_entries = (int)get_num(bo, "max_entries", 7);
//                                                   ↑ 和引擎的默认值重复了一遍
// 而引擎的默认值是会改的（v3.3.0 就把 7层/3.5% 改成了 8层/2.0%）。改了引擎、
// 忘了改这里，headless 用户漏填这个键就会静默拿到【旧配置】——而且是文档里
// 明确写着"回撤相当于 80% 本金"的那一组。GUI 用户不受影响，两边行为分叉，
// 没有任何地方会报错。
//
// 这条断言让重复的字面量再也不能悄悄漂走。
#include "headless/headless_config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}
template <typename T>
static void eq(T got, T want, const std::string& what) {
    bool ok = (got == want);
    if (ok) std::printf("[ OK ]  %s\n", what.c_str());
    else    { std::printf("[FAIL]  %s\n", what.c_str()); ++g_fail; }
}
static void eqd(double got, double want, const std::string& what) {
    bool ok = std::fabs(got - want) < 1e-12;
    if (ok) std::printf("[ OK ]  %s\n", what.c_str());
    else    { std::printf("[FAIL]  %s  引擎默认 %.6g，headless 解析出 %.6g\n",
                          what.c_str(), want, got); ++g_fail; }
}

static bool write_file(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << s;
    return true;
}

int main() {
    const std::string path = "test_cfg.json";

    // ── ① 最小配置：只给 symbol，其余全部应当回落到【引擎默认值】 ────────────
    {
        write_file(path,
            "{\"api_key\":\"k\",\"api_secret\":\"s\","
            "\"bots\":[{\"symbol\":\"BTCUSDT\"}]}");

        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "最小配置能解析: " + err);
        check(hc.bots.size() == 1, "解析出 1 个 bot");
        if (hc.bots.empty()) { std::printf("\n无法继续\n"); return 1; }

        const CcgConfig& g = hc.bots[0];   // headless 解析结果
        const CcgConfig  d;                // 引擎默认值（头文件里的成员初始化器）

        std::printf("\n── 漏填的键必须回落到引擎默认值 ──\n");
        eq(g.strat_type,   d.strat_type,   "strat_type");
        eq((int)g.max_entries, (int)d.max_entries, "max_entries（与保底利润强耦合，必须成对）");
        eq((int)g.leverage,    (int)d.leverage,    "leverage");
        eqd(g.budget_usdt,  d.budget_usdt,  "budget_usdt");
        eqd(g.interval_pct, d.interval_pct, "interval_pct");
        eqd(g.trail_entry,  d.trail_entry,  "trail_entry");
        eqd(g.tp_pct,       d.tp_pct,       "tp_pct");
        eqd(g.trail_tp,     d.trail_tp,     "trail_tp");
        eq(g.auto_restart,  d.auto_restart,  "auto_restart");
        eq((int)g.cooldown_secs, (int)d.cooldown_secs, "cooldown_secs");
        eqd(g.stop_loss_pct, d.stop_loss_pct, "stop_loss_pct");
        eq(g.use_disaster_stop, d.use_disaster_stop, "use_disaster_stop");
        eqd(g.disaster_stop_pct, d.disaster_stop_pct, "disaster_stop_pct");
        eq(g.entry_mode,    d.entry_mode,    "entry_mode");
        eq(g.kline_interval, d.kline_interval, "kline_interval");
        eq((int)g.boll_period, (int)d.boll_period, "boll_period");
        eqd(g.boll_mult,    d.boll_mult,    "boll_mult");
        eq(g.use_rsi_filter, d.use_rsi_filter, "use_rsi_filter");
        eq((int)g.rsi_period, (int)d.rsi_period, "rsi_period");
        eqd(g.rsi_threshold, d.rsi_threshold, "rsi_threshold");
        eq(g.rsi_confirm_mode, d.rsi_confirm_mode, "rsi_confirm_mode");
        eqd(g.rsi_oversold_th, d.rsi_oversold_th, "rsi_oversold_th");
        eq(g.dynamic_band_mode, d.dynamic_band_mode, "dynamic_band_mode");
        eqd(g.min_profit_floor, d.min_profit_floor, "min_profit_floor（与层数强耦合，必须成对）");
        eqd(g.dyn_fixed_interval, d.dyn_fixed_interval, "dyn_fixed_interval 缺字段时继承默认");
        eq(g.mtf_ladder,    d.mtf_ladder,    "mtf_ladder");
        eq(g.mtf_tier_layers, d.mtf_tier_layers, "mtf_tier_layers");
        eqd(g.mtf_k,        d.mtf_k,        "mtf_k");
        eqd(g.mtf_min_gap_pct, d.mtf_min_gap_pct, "mtf_min_gap_pct");
        eq(g.use_trend_filter, d.use_trend_filter, "use_trend_filter");
        eq(g.trend_interval, d.trend_interval, "trend_interval");
        eq((int)g.trend_ema_period, (int)d.trend_ema_period, "trend_ema_period");
        eq(g.sr_radar,      d.sr_radar,      "sr_radar");
        eq(g.sr_interval,   d.sr_interval,   "sr_interval");
        eq(g.use_htf_filter, d.use_htf_filter, "use_htf_filter");
        eq(g.htf_interval,  d.htf_interval,  "htf_interval");
        eqd(g.htf_pos_max,  d.htf_pos_max,  "htf_pos_max");
        eq(g.use_sr_support, d.use_sr_support, "use_sr_support");
        eq(g.use_sr_headroom, d.use_sr_headroom, "use_sr_headroom");
        eq((int)g.sr_min_confluence, (int)d.sr_min_confluence, "sr_min_confluence");
        eqd(g.sr_headroom_ratio, d.sr_headroom_ratio, "sr_headroom_ratio");
        eq(g.use_sr_exit,   d.use_sr_exit,   "use_sr_exit");
        eq(g.use_structural_stop, d.use_structural_stop, "use_structural_stop");
    }

    // ── ② 显式填写的值必须被采纳（别修完默认值把读取也弄坏了）────────────────
    {
        std::printf("\n── 显式配置必须覆盖默认值 ──\n");
        write_file(path,
            "{\"api_key\":\"k\",\"api_secret\":\"s\",\"bots\":[{"
            "\"symbol\":\"ETHUSDT\",\"direction\":\"short\",\"strat_type\":\"flat\","
            "\"max_entries\":3,\"leverage\":10,\"budget_usdt\":777.5,"
            "\"min_profit_floor\":4.25,\"dyn_fixed_interval\":6.0,"
            "\"mtf_ladder\":true,\"mtf_tier_layers\":\"5,1,1,1\","
            "\"entry_mode\":\"immediate\",\"use_trend_filter\":false}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "带显式值的配置能解析: " + err);
        if (!hc.bots.empty()) {
            const auto& g = hc.bots[0];
            eq(g.symbol, std::string("ETHUSDT"), "symbol");
            eq(g.direction, CcgConfig::Direction::Short, "direction=short");
            eq(g.strat_type, CcgConfig::StratType::Flat, "strat_type=flat");
            eq((int)g.max_entries, 3, "max_entries=3");
            eq((int)g.leverage, 10, "leverage=10");
            eqd(g.budget_usdt, 777.5, "budget_usdt=777.5");
            eqd(g.min_profit_floor, 4.25, "min_profit_floor=4.25");
            eqd(g.dyn_fixed_interval, 6.0, "dyn_fixed_interval=6.0（显式值生效）");
            eq(g.mtf_ladder, true, "mtf_ladder=true");
            eq(g.mtf_tier_layers, std::string("5,1,1,1"), "mtf_tier_layers");
            eq(g.entry_mode, CcgConfig::EntryMode::Immediate, "entry_mode=immediate");
            eq(g.use_trend_filter, false, "use_trend_filter=false");
        }
    }

    // ── ③ direction=both 必须被拒绝 ─────────────────────────────────────────
    // 引擎内部 Both 会走纯空头分支：用户想要对冲、实际拿到裸空单
    {
        std::printf("\n── 危险配置必须拒绝 ──\n");
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"bots\":[{\"symbol\":\"BTCUSDT\",\"direction\":\"both\"}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "direction=both 被拒绝启动");
        check(err.find("both") != std::string::npos, "  错误信息说明了原因");
    }

    // ── ④ 未知键要告警（拼错的键会静默失效，用户以为配上了）──────────────────
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"bots\":[{\"symbol\":\"BTCUSDT\",\"max_entires\":9}]}");
        HeadlessConfig hc; std::string err;
        load_headless_config(path, hc, err);
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("max_entires") != std::string::npos) warned = true;
        check(warned, "拼错的键（max_entires）产生告警");
    }

    // ── ⑤ 损坏/缺失文件不崩溃 ───────────────────────────────────────────────
    {
        write_file(path, "{ 这不是 json");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "损坏 JSON 返回失败而不是崩溃");
        std::remove(path.c_str());
        HeadlessConfig hc2; std::string err2;
        check(!load_headless_config(path, hc2, err2), "文件不存在返回失败");
    }

    std::remove(path.c_str());
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
