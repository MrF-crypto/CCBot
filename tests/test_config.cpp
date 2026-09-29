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

    // ── ① 损坏/缺失文件不崩溃 ───────────────────────────────────────────────
    // v4.7.1 之前这里前面还有四组网格DCA 的用例（默认值回落、显式值采纳、
    // direction=both 拒绝、拼错键告警）。DCA 移除后它们随之删除；
    // 同类覆盖在下面的 SAR 段里一条不少
    {
        write_file(path, "{ 这不是 json");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "损坏 JSON 返回失败而不是崩溃");
        std::remove(path.c_str());
        HeadlessConfig hc2; std::string err2;
        check(!load_headless_config(path, hc2, err2), "文件不存在返回失败");
    }

    // ── ② 趋势SAR 配置（本版起是唯一的策略）─────────────────────────────────
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\"}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "最小 sar_bots 配置能加载: " + err);
        check(hc.sar_bots.size() == 1, "应解析出 1 个 bot");

        // 漏填的字段必须落到引擎默认值。解析器用 c.rule.xxx 自身作兜底
        // （而不是抄一遍字面量），所以引擎改默认值时这里不会悄悄漂走
        const SarConfig def;
        const auto& g = hc.sar_bots[0];
        check(g.rule.donchian_period == def.rule.donchian_period, "  通道周期取默认值");
        check(g.rule.atr_period == def.rule.atr_period,           "  ATR周期取默认值");
        eqd(g.rule.atr_mult, def.rule.atr_mult, "  k 取默认值");
        check(g.rule.reverse_needs_signal == def.rule.reverse_needs_signal,
              "  反手信号闸默认开启（无条件反手在震荡市是绞肉机）");
        check(g.interval == def.interval, "  信号周期取默认值");
    }
    {
        // 旧版的 "bots"（网格DCA）必须【被明确报出来】而不是静默忽略。
        // 静默忽略的后果：升级后用户的配置原样放着，进程正常启动、日志一切正常，
        // 而那些品种其实一个都没在跑——要等到某天去交易所对账才发现。
        // 这种沉默比启动失败危险得多
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"bots\":[{\"symbol\":\"BTCUSDT\"},{\"symbol\":\"ETHUSDT\"}],"
                         "\"sar_bots\":[{\"symbol\":\"SOLUSDT\"}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "带旧版 bots 的配置仍能启动");
        check(hc.sar_bots.size() == 1, "  只有 sar_bots 生效");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("网格DCA") != std::string::npos && w.find("2") != std::string::npos)
                warned = true;
        check(warned, "  明确告警\"这 2 个 DCA 条目完全不会运行\"");
    }
    {
        // 一个策略都没配 = 空转，应报错而不是静默启动
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\"}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "sar_bots 为空应被拒绝");
    }
    {
        // k=0 等于没有止损线，必须拒绝
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\",\"atr_mult\":0}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "atr_mult=0 应被拒绝");
    }
    {
        // SAR 段的拼写错误同样要告警
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\",\"atr_mlut\":3}]}");
        HeadlessConfig hc; std::string err;
        load_headless_config(path, hc, err);
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("atr_mlut") != std::string::npos) warned = true;
        check(warned, "SAR 段拼错的键产生告警");
    }
    {
        // 关掉信号闸（无条件反手）要明确告警一次
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"reverse_needs_signal\":false}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "无条件反手应能加载（是合法配置）");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("reverse_needs_signal") != std::string::npos) warned = true;
        check(warned, "无条件反手应产生告警");
    }

    // ── ⑦ SAR 等风险下单 + 金字塔加仓 ───────────────────────────────────────
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"size_mode\":\"risk\",\"risk_usdt\":100,"
                         "\"pyramid_max_adds\":3,\"pyramid_step_atr\":0.5}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "等风险+金字塔配置应能加载");
        if (hc.sar_bots.size() == 1) {
            const auto& g = hc.sar_bots[0];
            check(g.size_mode == SarConfig::SizeMode::RiskBased, "  size_mode=risk");
            eqd(g.risk_usdt, 100.0,                                "  risk_usdt");
            check(g.rule.pyramid_max_adds == 3,                    "  加仓档数");
            eqd(g.rule.pyramid_step_atr, 0.5,                      "  加仓间距");
        }
    }
    {
        // size_mode=risk 却没填 risk_usdt：算不出任何数量，必须拒绝启动
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\",\"size_mode\":\"risk\"}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err),
              "size_mode=risk 但缺 risk_usdt 应被拒绝");
    }
    {
        // 加仓间距为 0 会在同一价位无限加仓
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"pyramid_max_adds\":3,\"pyramid_step_atr\":0}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "pyramid_step_atr=0 应被拒绝");
    }
    {
        // 默认必须是关闭的：这两个功能都会改变下单行为，不能悄悄生效
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\"}]}");
        HeadlessConfig hc; std::string err;
        load_headless_config(path, hc, err);
        if (hc.sar_bots.size() == 1) {
            check(hc.sar_bots[0].size_mode == SarConfig::SizeMode::Notional,
                  "默认仓位算法必须是固定名义");
            check(hc.sar_bots[0].rule.pyramid_max_adds == 0,
                  "默认必须不加仓");
        }
    }

    std::remove(path.c_str());
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
