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
        const TrendConfig def;
        const auto& g = hc.sar_bots[0];
        check(g.rule.strategy == def.rule.strategy,                "  策略默认是海龟");
        check(g.rule.donchian_period == def.rule.donchian_period, "  通道周期取默认值");
        check(g.rule.atr_period == def.rule.atr_period,           "  ATR周期取默认值");
        eqd(g.rule.atr_mult, def.rule.atr_mult, "  k 取默认值");
        check(g.rule.reverse == trend::ReverseMode::None,
              "  默认【不反手】（无条件反手在震荡市是绞肉机）");
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

    // ── 外扩缓冲换口径：占价格% → 占止损距离%（v5.10.0）────────────────────
    // 这是一次【静默改变行为】的改动：两个键都是"一个百分数"，认错了不报错，
    // 只会挂出一张几乎必被交易所抢先触发的单。所以换了键名，而不是改解释
    {
        // 只写旧键：不得被当成新口径用，必须落到新默认值并告警
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"use_disaster_stop\":true,"
                         "\"disaster_stop_buffer_pct\":1.0}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "只有旧键时仍应加载成功: " + err);
        check(hc.sar_bots.size() == 1, "  解析出 1 个 bot");
        check(std::fabs(hc.sar_bots[0].disaster_stop_buffer_pct - 20.0) < 1e-9,
              "  ⚠ 旧键的 1.0 不得被沿用——那在新口径下是止损距离的 1%，"
              "默认参数下约等于价格的 0.06%，比原意紧 16 倍");
        bool warned = false, unknown = false;
        for (const auto& w : hc.warnings) {
            if (w.find("disaster_stop_buf_dist_pct") != std::string::npos) warned = true;
            if (w.find("无法识别的键") != std::string::npos) unknown = true;
        }
        check(warned, "  必须明确告知口径变了、本次按默认值跑");
        check(!unknown, "  旧键仍在识别列表里，不该再报一条「拼写错误?」把人绕晕");
    }
    {
        // 写了新键：原样采用，且不再告警
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"use_disaster_stop\":true,"
                         "\"disaster_stop_buf_dist_pct\":35.0}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "新键应加载成功: " + err);
        check(std::fabs(hc.sar_bots[0].disaster_stop_buffer_pct - 35.0) < 1e-9,
              "  新键的值原样采用");
        // ⚠ 写成 for(...) check(...) 的话，warnings 为空时循环体一次都不执行，
        //   这条断言就【静默变成 0 条】——看起来在测，其实什么都没测。
        //   先数出来再断言，空列表同样会走到 check
        int buf_warns = 0;
        for (const auto& w : hc.warnings)
            if (w.find("disaster_stop") != std::string::npos) ++buf_warns;
        check(buf_warns == 0, "  用了新键就不该再有任何缓冲相关的告警");
    }
    {
        // 新键为 0 仍然要拒：挂在止损线上会让交易所抢先触发
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"use_disaster_stop\":true,"
                         "\"disaster_stop_buf_dist_pct\":0}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "缓冲=0 应被拒绝");
        check(err.find("disaster_stop_buf_dist_pct") != std::string::npos,
              "  报错里要写【新】键名，否则人会去改一个已经不生效的键");
    }

    // ── ②b 三个策略各自的解析与校验 ─────────────────────────────────────────
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"psar\","
                         "\"af_start\":0.03,\"af_step\":0.01,\"af_max\":0.15,"
                         "\"max_consecutive_reverses\":3}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "strategy=psar 能加载: " + err);
        if (hc.sar_bots.size() == 1) {
            const auto& g = hc.sar_bots[0];
            check(g.rule.strategy == trend::Strategy::ParabolicSar, "  策略=抛物线SAR");
            eqd(g.rule.af_start, 0.03, "  af_start");
            eqd(g.rule.af_step,  0.01, "  af_step");
            eqd(g.rule.af_max,   0.15, "  af_max");
            // PSAR 的入场信号【只有】翻转一个来源，不反手就永远空仓——
            // 所以配置层强制写死，不管 JSON 里写了什么
            check(g.rule.reverse == trend::ReverseMode::Immediate,
                  "  PSAR 的反手模式被强制为 immediate");
        }
    }
    {
        // 显式配 reverse=none 也要被 PSAR 的强制覆盖掉。
        // 不覆盖的话 bot 启动后一单也不开，而日志上完全正常
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"psar\","
                         "\"reverse\":\"none\",\"max_consecutive_reverses\":2}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "PSAR + reverse=none 仍能加载");
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].rule.reverse == trend::ReverseMode::Immediate,
                  "PSAR 忽略 reverse=none，否则它会永远空仓");
    }
    {
        // PSAR 不设连续反手上限 = 震荡市里没有刹车，必须明确说一次
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"psar\"}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "PSAR 不设上限仍能启动（不拦）");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("max_consecutive_reverses") != std::string::npos) warned = true;
        check(warned, "  但必须告警「没有刹车」");
    }
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"psar\","
                         "\"af_start\":0.3,\"af_max\":0.2}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err),
              "af_start > af_max 应被拒绝（起点就封顶，加速机制直接失效）");
    }
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"bare_k\","
                         "\"bare_entry\":\"immediate\",\"once_per_bar\":true,"
                         "\"swing_bars\":5}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "strategy=bare_k 能加载: " + err);
        if (hc.sar_bots.size() == 1) {
            const auto& g = hc.sar_bots[0];
            check(g.rule.strategy == trend::Strategy::BareK,          "  策略=纯裸K");
            check(g.rule.bare_entry == trend::BareEntry::Immediate,   "  入场=盘中即时");
            check(g.rule.once_per_bar,                                "  每根一次护栏已开");
            check(g.rule.swing_bars == 5,                             "  摆动根数=5");
        }
    }
    {
        // 盘中即时 + 立即反手 + 无护栏 = 一根K线内来回开平，纯烧手续费
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"bare_k\","
                         "\"bare_entry\":\"immediate\",\"reverse\":\"immediate\"}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "抖动组合仍能启动（合法，只是危险）");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("once_per_bar") != std::string::npos) warned = true;
        check(warned, "  必须告警并建议开 once_per_bar");
    }
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"bare_k\","
                         "\"bare_entry\":\"whatever\"}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "bare_entry 非法值应被拒绝");
    }
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"strategy\":\"martingale\"}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "未知 strategy 应被拒绝而不是默默跑海龟");
    }

    // ── ②c 旧键迁移：必须被明确报出来，不能静默改变行为 ─────────────────────
    {
        // 旧 mode=bar 的入场规则是"刚收盘那根是阳线就做多"，已删除。
        // 静默迁移的后果是信号数量骤变而用户毫不知情
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\",\"mode\":\"bar\"}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "旧 mode=bar 仍能加载");
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].rule.strategy == trend::Strategy::BareK,
                  "  mode=bar 迁移为 strategy=bare_k");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("mode=bar") != std::string::npos) warned = true;
        check(warned, "  必须告警入场规则已变");
    }
    {
        // 旧默认 allow_reverse=true + reverse_needs_signal=true 没有对应的新档位，
        // 映射到 none（保护性更强的那边）并说明差别
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\","
                         "\"allow_reverse\":true,\"reverse_needs_signal\":true}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "旧反手 bool 仍能加载");
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].rule.reverse == trend::ReverseMode::None,
                  "  「反手需信号」映射到 reverse=none");
        bool warned = false;
        for (const auto& w : hc.warnings)
            if (w.find("reverse=none") != std::string::npos) warned = true;
        check(warned, "  必须告警这是行为变化");
    }
    {
        // 旧的无条件反手（allow=true, needs=false）有精确对应，行为不变
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\","
                         "\"allow_reverse\":true,\"reverse_needs_signal\":false}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "旧无条件反手仍能加载");
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].rule.reverse == trend::ReverseMode::Immediate,
                  "  无条件反手精确映射到 reverse=immediate");
    }
    {
        // 新键在场时旧键必须【完全不起作用】——两套键同时被尊重是最难查的一类 bug
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"reverse\":\"immediate\","
                         "\"allow_reverse\":false,\"reverse_needs_signal\":true}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "新旧键并存仍能加载");
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].rule.reverse == trend::ReverseMode::Immediate,
                  "  新键 reverse 优先，旧 bool 被忽略");
    }
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\",\"sar_bots\":["
                         "{\"symbol\":\"BTCUSDT\",\"reverse\":\"maybe\"}]}");
        HeadlessConfig hc; std::string err;
        check(!load_headless_config(path, hc, err), "reverse 非法值应被拒绝");
    }

    // ── ③ 固定单笔风险下单 ──────────────────────────────────────────────────
    {
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"size_mode\":\"risk\",\"risk_usdt\":100}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "固定单笔风险配置应能加载");
        if (hc.sar_bots.size() == 1) {
            const auto& g = hc.sar_bots[0];
            check(g.size_mode == TrendConfig::SizeMode::RiskBased, "  size_mode=risk");
            eqd(g.risk_usdt, 100.0,                                "  risk_usdt");
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
        // 金字塔已整体移除：老配置里的两个键必须落到【未知键告警】上，
        // 而不是被静默接受——静默接受的话用户会以为加仓还在生效
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\","
                         "\"pyramid_max_adds\":3,\"pyramid_step_atr\":0.5}]}");
        HeadlessConfig hc; std::string err;
        check(load_headless_config(path, hc, err), "带旧金字塔键的配置仍能启动");
        int warned = 0;
        for (const auto& w : hc.warnings)
            if (w.find("pyramid") != std::string::npos) ++warned;
        check(warned == 2, "  两个已移除的金字塔键都产生了未知键告警");
    }
    {
        // 默认必须是固定名义：改变下单行为的东西不能悄悄生效
        write_file(path, "{\"api_key\":\"k\",\"api_secret\":\"s\","
                         "\"sar_bots\":[{\"symbol\":\"BTCUSDT\"}]}");
        HeadlessConfig hc; std::string err;
        load_headless_config(path, hc, err);
        if (hc.sar_bots.size() == 1)
            check(hc.sar_bots[0].size_mode == TrendConfig::SizeMode::Notional,
                  "默认仓位算法必须是固定名义");
    }

    std::remove(path.c_str());
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
