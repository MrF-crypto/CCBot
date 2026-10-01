// 应用层判定测试（src/core/app_logic.h）。
//
// 存在的理由：GUI 层 3887 行没有一行自动化测试，它的逻辑写在按钮回调、Qt 定时器、
// 表格刷新里，不开窗口就调不到。本会话里 GUI 的缺陷【全部】是靠实盘日志发现的：
//   · 启动对账在拉取失败时清掉真实持仓（v5.6.0）
//   · 周期对账拿最旧 30 秒的快照，把刚成交的仓位判成"外部已平"（v5.9.5）
// 这里测的是从 GUI / headless 里抽出来、两边共用的那几条判定。抽出来时还顺带
// 发现了三处两边不一致（见 app_logic.h 头注释），这几条用例同时钉住它们。
//
// ⚠ 不起线程、不碰网络，所以只进 ASan 列表，不进 TSan 列表。
#include "core/app_logic.h"

#include <cstdio>
#include <string>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

int main() {
    // ═══ 对账闸门 ════════════════════════════════════════════════════════════
    std::printf("── 对账闸门 ──\n");
    {
        using app::reconcile_gate;
        using app::ReconcileSkip;
        const int64_t now = 1'700'000'000'000;

        // 最要命的一条：拉取失败时持仓列表是空的。拿它比对，每一个有持仓的 bot
        // 都会被判"交易所已无此仓位"而清掉 —— v5.6.0 修过的 GUI 启动对账就是这条
        auto g = reconcile_gate(false, now, now);
        check(!g.ok(), "⚠ 拉取失败时绝不对账 —— 空列表不等于交易所没有仓位");
        check(g.skip == ReconcileSkip::FetchFailed, "  且原因要分得清是「没拉成」");

        // 从没成功拉到过（GUI 的 posCacheMs_ 仍是 0）
        g = reconcile_gate(true, 0, now);
        check(!g.ok() && g.skip == ReconcileSkip::NeverFetched,
              "从没拉成功过（快照时刻为 0）不得对账");

        // 太旧：v5.9.4 实盘那次就是拿最旧 30 秒的缓存，把同一秒开的仓判成外部已平
        g = reconcile_gate(true, now - 30000, now);
        check(!g.ok() && g.skip == ReconcileSkip::TooOld,
              "⚠ 快照年龄到达上限（30 秒）就不得对账");
        g = reconcile_gate(true, now - 29999, now);
        check(g.ok(), "  差 1 毫秒到上限仍可对账（边界）");

        // 新鲜的快照：放行，并如实报出年龄 —— 引擎要拿它去判断哪些仓位变化
        // 发生在快照之后（那部分仓位本轮必须跳过）
        g = reconcile_gate(true, now - 1234, now);
        check(g.ok(), "新鲜快照放行");
        check(g.age.count() == 1234, "  且如实交出快照年龄 1234ms（引擎靠它做逐 bot 的判定）");

        // 读数顺序颠倒（跨线程排队时 now 先读、snap 后写）：只可能意味着快照非常新
        g = reconcile_gate(true, now + 50, now);
        check(g.ok() && g.age.count() == 0,
              "now 比快照还早时按年龄 0 处理 —— 不该因此跳过一整轮对账");

        // 上限可配：GUI 与 headless 都用默认值，但函数本身不该把 30 秒写死
        g = reconcile_gate(true, now - 5000, now, 3000);
        check(!g.ok() && g.skip == ReconcileSkip::TooOld, "自定义上限生效");
    }

    // ═══ 品种名规范化 ════════════════════════════════════════════════════════
    std::printf("\n── 品种名规范化 ──\n");
    {
        using app::normalize_symbol;

        auto f = normalize_symbol("BTCUSDT");
        check(f.symbol == "BTCUSDT" && !f.changed && !f.suffixed, "规范的名字原样通过、不告警");

        // headless 在 v5.9.9 之前完全不处理这一条：手写 "BTC" 的条目永远拉不到
        // K 线、永远"等信号"，而进程照常运行 —— 静默失效
        f = normalize_symbol("BTC");
        check(f.symbol == "BTCUSDT" && f.suffixed, "⚠ 缺后缀补 USDT，并标记要告警");

        f = normalize_symbol("  btcusdt \t");
        check(f.symbol == "BTCUSDT", "去首尾空白并转大写");
        check(f.changed && !f.suffixed, "  算「改过」（要落盘），但不是补后缀（不必告警）");

        // 币安 U 本位合约里还有 USDC 计价的合约。只认 USDT 的话，BTCUSDC 会被
        // 补成 BTCUSDCUSDT —— 一个不存在的品种
        f = normalize_symbol("BTCUSDC");
        check(f.symbol == "BTCUSDC" && !f.suffixed, "⚠ USDC 计价的品种不得被补成 ...USDCUSDT");

        f = normalize_symbol("eth");
        check(f.symbol == "ETHUSDT" && f.suffixed, "小写且缺后缀：先转大写再补");

        // "USDT" 本身不是一个品种。只判"以 USDT 结尾"的话它会被当成规范名放过
        f = normalize_symbol("USDT");
        check(f.symbol == "USDTUSDT", "只有报价币、没有标的的输入不得被当成规范名放过");

        f = normalize_symbol("");
        check(f.symbol.empty() && !f.suffixed, "空串保持空串（由调用方按缺 symbol 处理）");
    }

    // ═══ 旧反手配置迁移 ══════════════════════════════════════════════════════
    std::printf("\n── 旧反手配置迁移 ──\n");
    {
        using app::migrate_reverse_mode;
        using app::MigrationEffect;
        using trend::ReverseMode;

        // allow=false：原来就不反手。v5.9.9 之前 headless 在这里也报
        // "⚠ 行为有变化——原来反向信号成立时会立刻反手" —— 错，它原来根本不反手
        for (bool needs : {false, true}) {
            auto m = migrate_reverse_mode(false, needs);
            check(m.mode == ReverseMode::None, std::string("allow=false, needs=") +
                  (needs ? "true" : "false") + " ⇒ None");
            check(m.effect == MigrationEffect::Unchanged && m.note.empty(),
                  "  ⚠ 行为没变，不得报「有变化」（headless 之前就报错了这一条）");
        }

        auto m = migrate_reverse_mode(true, false);
        check(m.mode == ReverseMode::Immediate, "allow=true, needs=false（止损即反手）⇒ Immediate");
        check(m.effect == MigrationEffect::Unchanged && m.note.empty(), "  行为不变，不告警");

        // 唯一真正没有等价物的一档。之前 GUI 说"行为几乎不变"、headless 说
        // "行为有变化"—— 对同一件事给出相反的严重程度
        m = migrate_reverse_mode(true, true);
        check(m.mode == ReverseMode::None,
              "allow=true, needs=true ⇒ None（取保护性更强的那边）");
        check(m.effect == MigrationEffect::Changed, "  ⚠ 必须标成「有变化」—— 这一档确实没有等价物");
        check(m.note.find("立即反手") != std::string::npos,
              "  说明里要告诉人怎么改回去（设为「立即反手」）");
        check(m.note.find("同向信号") != std::string::npos,
              "  并且讲清具体变了什么，而不是只说「有变化」");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\nOK: 应用层判定测试全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
