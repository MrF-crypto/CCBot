#pragma once
// 应用层判定：GUI（main_window / trend_panel）与 headless（main / headless_config）
// 【共用】的那几条规则，抽成不依赖 Qt 的纯函数。
//
// 为什么要有这个文件：
//   GUI 层 3887 行没有一行自动化测试 —— 它的逻辑写在按钮回调、Qt 定时器、
//   表格刷新里，不开窗口就调不到。本会话里 GUI 的缺陷（启动对账清掉真实持仓、
//   周期对账拿 30 秒旧快照判外部已平……）【全部】是靠实盘日志发现的。
//   把其中最危险的几条判定挪到这里，GUI 与 headless 都调用它，
//   测试就能像测引擎那样喂假数据去测（见 tests/test_app_logic.cpp）。
//
// 第二个理由是【两边各写一份必然会分叉】。抽出来时就发现了三处：
//   · 旧反手配置 allow_reverse=false：GUI 静默迁移（对，行为没变）；headless
//     却报"⚠ 行为有变化——原来反向信号成立时会立刻反手"（错，它原来就不反手）
//   · 同一种迁移（allow=true, needs=true），GUI 说"行为几乎不变"，headless 说
//     "行为有变化"—— 对同一件事给出相反的严重程度
//   · 品种名缺 USDT 后缀：GUI 自动补全并告警；headless 完全不处理，于是手写的
//     "BTC" 永远拉不到 K 线、永远"等信号"，进程照常运行 —— 静默失效
#include "core/trend_decision.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>

namespace ccbot::app {

// ═══ 对账闸门 ════════════════════════════════════════════════════════════════
//
// 对账会做【不可逆】的事（清本地状态、停 bot）。它的输入是一份交易所持仓快照，
// 这份快照有两种情况绝不能拿来比：
//   · 根本没拉成 —— 空列表既可能是"确实没仓"也可能是请求失败。当成前者，
//     每一个有持仓的 bot 都会被判"交易所已无此仓位"而清掉。v5.6.0 修过的
//     GUI 启动对账就是这条（实盘里 LINKUSDT 的仓位被清掉过）
//   · 太旧 —— 本地仓位可能是在快照拍摄之后才开的。v5.9.4 实盘：开空成交的同一秒
//     被判成"外部已平"，硬止损随后被当孤儿单撤掉，留下一笔裸仓
//
// 太旧的门槛之外，引擎内部还有一道【按仓位变化时刻】的精细判定（见
// TrendEngine::reconcile_positions 的 snapshot_age）。这里只做粗筛：
// 拿不到 / 过旧就整轮不比，比的时候把快照年龄如实交给引擎。
constexpr int64_t kReconcileMaxSnapshotAgeMs = 30000;

enum class ReconcileSkip {
    None,          // 可以对账
    FetchFailed,   // 这次没拉成（ok=false）
    NeverFetched,  // 从来没成功拉到过（快照时刻为 0）
    TooOld,        // 有，但太旧
};

struct ReconcileGate {
    ReconcileSkip             skip = ReconcileSkip::None;
    std::chrono::milliseconds age{0};   // skip==None 时才有意义
    bool ok() const { return skip == ReconcileSkip::None; }
};

// snap_ms / now_ms 必须来自【同一个时钟】（调用方各自保证）。
// now 比 snap 还早（时钟回拨、跨线程排队的读数顺序）按 0 处理 —— 那只可能
// 意味着快照"非常新"，不该因此跳过一轮对账
inline ReconcileGate reconcile_gate(bool fetched_ok, int64_t snap_ms, int64_t now_ms,
                                    int64_t max_age_ms = kReconcileMaxSnapshotAgeMs) {
    ReconcileGate g;
    if (!fetched_ok)  { g.skip = ReconcileSkip::FetchFailed;  return g; }
    if (snap_ms <= 0) { g.skip = ReconcileSkip::NeverFetched; return g; }
    int64_t age = now_ms - snap_ms;
    if (age < 0) age = 0;
    if (age >= max_age_ms) { g.skip = ReconcileSkip::TooOld; return g; }
    g.age = std::chrono::milliseconds(age);
    return g;
}

// ═══ 品种名规范化 ════════════════════════════════════════════════════════════
//
// 去首尾空白、转大写、缺报价币后缀时补 USDT。
//
// ⚠ USDC 结尾的【不补】。币安 U 本位合约里同时有 USDT 与 USDC 计价的合约
//   （BTCUSDC、ETHUSDC…）。只认 USDT 的话，"BTCUSDC" 会被补成 "BTCUSDCUSDT"
//   —— 一个不存在的品种。GUI 的加品种框本来就只产出 ...USDT，所以对 GUI 这条
//   不改变任何行为；它保护的是 headless 里手写 USDC 品种的人。
struct SymbolFix {
    std::string symbol;
    bool        changed = false;   // 与输入不同（大小写、空白、后缀任一项）
    bool        suffixed = false;  // 其中是否补了 USDT 后缀（这一项值得告警）
};

inline SymbolFix normalize_symbol(const std::string& raw) {
    SymbolFix f;
    std::string s = raw;
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);

    auto ends_with = [&](const char* suf) {
        const std::string t(suf);
        return s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    };
    if (!s.empty() && !ends_with("USDT") && !ends_with("USDC")) {
        s += "USDT";
        f.suffixed = true;
    }
    f.symbol  = s;
    f.changed = (s != raw);
    return f;
}

// ═══ 旧反手配置迁移：allow_reverse + reverse_needs_signal → ReverseMode ══════
//
// v5.1 之前反手由两个 bool 控制，四种组合；新枚举只有两档（立即反手 / 不反手）。
//
//   allow  needs   旧含义                    新值        行为
//   ─────  ─────   ────────────────────────  ─────────  ──────────
//   false  任意    从不反手                  None       不变
//   true   false   止损即反手                Immediate  不变
//   true   true    止损时有反向信号才反手    None       【有变化】
//
// 第三行没有等价物。映射到 None 是取保护性更强的那边：止损后只平掉，回到正常
// 入场流程 —— 反向信号若成立，下一拍照样会按正常入场反向开；差别是不再在
// 止损的同一个动作里反向开，以及同向信号先来也会再进一次。
enum class MigrationEffect { Unchanged, Changed };

struct ReverseMigration {
    trend::ReverseMode mode   = trend::ReverseMode::None;
    MigrationEffect    effect = MigrationEffect::Unchanged;
    // effect==Changed 时给人看的说明（不含品种名，调用方自己加前缀）。
    // GUI 与 headless 打的是【同一句话】—— 两边各写一句，就会像 v5.9.8 之前
    // 那样对同一件事说出相反的严重程度
    std::string note;
};

inline ReverseMigration migrate_reverse_mode(bool allow_reverse, bool needs_signal) {
    ReverseMigration m;
    if (!allow_reverse) {
        m.mode = trend::ReverseMode::None;              // 原来就不反手
        return m;
    }
    if (!needs_signal) {
        m.mode = trend::ReverseMode::Immediate;         // 原来就是止损即反手
        return m;
    }
    m.mode   = trend::ReverseMode::None;
    m.effect = MigrationEffect::Changed;
    m.note   = "旧的「止损时有反向信号才反手」这一档已不存在，已迁移为「只平掉，"
               "回到正常入场流程」。⚠ 行为有两处变化：① 不再在止损的同一个动作里"
               "反向开仓，而是由下一个正常入场信号开（反向信号成立的话，下一拍照样"
               "会反向开）；② 同向信号先来也会再进一次。"
               "想要止损即反手，请把反手方式设为「立即反手」（headless：reverse=immediate）";
    return m;
}

} // namespace ccbot::app
