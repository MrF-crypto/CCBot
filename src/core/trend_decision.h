#pragma once
#include <algorithm>
#include <cmath>
#include <string>

// 趋势策略的决策核心 —— 三个策略共用一套状态机与出场语义，只在
// 【入场信号】与【止损线怎么算】两处分叉。
//
// 纯函数、无状态依赖、不碰网络，便于单测穷举。引擎（trend_engine）负责把这里
// 给出的 Action 变成订单，并在成交后回写状态。
//
// ── 三个策略 ────────────────────────────────────────────────────────────────
//   ① Turtle（海龟）    唐奇安通道突破入场 + Chandelier ATR 追踪止损
//   ② ParabolicSar      Wilder 抛物线 SAR：SAR 值既是入场也是止损也是反转点
//   ③ BareK（纯裸K）    两种入场模式二选一 + 最近 N 根摆动极值止损
//
// ── 共同的出场语义 ──────────────────────────────────────────────────────────
//   · 只有追踪止损，【没有固定止盈】。趋势跟随胜率天然只有 30~40%，全部收益来自
//     少数几笔跑得很远的单子。任何固定止盈都会把这些单子提前砍断，而亏损笔的
//     大小不变——等于单方面砍掉盈利分布的右尾，期望必然转负
//   · 止损线是【棘轮】的，只朝有利方向移动，绝不回退
//   · 盈利出场后【不反手】——力竭不等于反转，等新信号
//   · 数据断流时【沿用最后一条有效止损线】，绝不因为拿不到数据就撤掉保护
namespace ccbot::trend {

enum class Pos { Flat, Long, Short };

// 三个策略。入场算法与止损算法是【成套】的，不做混搭（比如唐奇安入场 +
// 摆动止损）——那没有实证依据，多开一个维度只会让参数空间翻倍而没人知道怎么填。
enum class Strategy {
    Turtle,        // ① 唐奇安突破 + Chandelier ATR
    ParabolicSar,  // ② Wilder 抛物线 SAR
    BareK,         // ③ 纯裸K
};

// ③ 纯裸K 的两种入场模式
enum class BareEntry {
    // 盘中即时：实时价 > 本根开盘价 ⇒ 做多，< ⇒ 做空。不等收盘。
    //
    // ⚠ 这个模式【天然抖】：新K线刚开盘时价格 ≈ 开盘价，微小波动会让条件
    //   反复翻转。配上"立即反手"就是一根K线内来回开平好几次，每次两笔手续费。
    //   护栏是 Config::once_per_bar（默认关）
    Immediate,
    // 等收盘突破：本根收盘价 > 上一根的最高价 ⇒ 做多；< 上一根最低价 ⇒ 做空。
    // 比"阳线就做多"严格得多——下跌趋势里的一根阳线只是噪音，而突破前一根
    // 的高点是真正的动能信号
    BreakPrevBar,
};

// 亏损止损之后怎么办。
// ⚠ 只有两种。v5.1.0 之前是两个 bool（allow_reverse + reverse_needs_signal），
//   四种组合里有一种（不许反手却又要求信号）是无意义的，枚举天然排除。
enum class ReverseMode {
    Immediate,   // 立即反手开反向仓
    None,        // 只平掉，回到正常入场流程（方向不限，等下一个信号）
};

struct Config {
    Strategy strategy = Strategy::Turtle;

    // ── ① 海龟 ──────────────────────────────────────────────────────────────
    int    donchian_period = 20;    // 入场通道周期（海龟原版 20）
    int    atr_period      = 14;    // ATR 周期
    double atr_mult        = 3.0;   // Chandelier k：止损距离 = k×ATR

    // ── ② 抛物线 SAR ────────────────────────────────────────────────────────
    // SAR(下一根) = SAR(本根) + AF × (EP − SAR(本根))
    //   EP = 持仓期极值（多=最高、空=最低），复用 State::peak
    //   AF = 加速因子，【每当刷新 EP 时】+af_step，封顶 af_max
    // 趋势走得越久 AF 越大，SAR 加速贴近价格——这是它"防止利润回吐太多"的机制。
    // 价格触及 SAR ⇒ 平仓并立即反向（stop-and-reverse 是它的定义）
    double af_start = 0.02;   // Wilder 原版
    double af_step  = 0.02;
    double af_max   = 0.20;

    // ── ③ 纯裸K ─────────────────────────────────────────────────────────────
    BareEntry bare_entry = BareEntry::BreakPrevBar;
    // 每根K线最多开一次。默认【关】。
    // 开着的话，同一根K线里被止损出场后不再重新入场，等下一根——这是 Immediate
    // 模式唯一的抖动护栏
    bool   once_per_bar = false;
    // 止损用 bar 0 之【前】 swing_bars 根的最低价（做多）/ 最高价（做空）。
    //
    // ⚠ 窗口最小值的止损，棘轮【理论上是自带的】：M 只有在新K线的最低价跌破
    //   旧 M 时才会下移，而那一刻价格已经穿过止损线了。但有两个例外必须靠
    //   显式棘轮兜住：
    //     ① 信号根自己的低点不在初始窗口里（初始窗口是 1..N，不含 bar 0）。
    //        bar 0 若是长下影的锤子线，它进入窗口时会把 M 拉下去，而那根
    //        下影发生在【入场之前】，没打到我们
    //     ② 引擎按 tick 采样价格，而K线的 low 记录的是连续极值。
    //        一根两秒的插针采样看不见，但下一根K线的 low 会记下来
    int    swing_bars = 3;

    // ── 反手（共享）─────────────────────────────────────────────────────────
    ReverseMode reverse = ReverseMode::None;

    // 连续反手上限（0 = 不限）与触顶后的冷却K线数。
    //
    // 为什么默认是 0（不限）：这两项只对 ③ 裸K 暴露在界面上，而它的默认反手模式
    // 是 None——不反手就不会有"连续反手"。
    // ⚠ 但 ② 抛物线SAR 是【无条件翻转】的，震荡市里可以一直翻下去没有刹车。
    //   那是 PSAR 最著名的弱点，不是实现缺陷。给它配个非 0 的上限是唯一的刹车
    int    max_consecutive_reverses = 0;
    int    cooldown_bars            = 0;
};

struct State {
    Pos    pos         = Pos::Flat;
    double entry_price = 0;
    // 持仓期极值：多=最高价，空=最低价。
    // ② 抛物线SAR 里它就是 EP（极值点），不需要额外字段
    double peak        = 0;
    // 棘轮止损线，只朝有利方向移动。
    // ② 抛物线SAR 里它就是 SAR 值本身
    double stop        = 0;
    // ② 专用：当前加速因子。只有 PSAR 用得上，别的策略恒为 0
    double af          = 0;
    // ② 专用：上一次【K线边界】结算时的 EP。AF 是否加速看的是"这根K线有没有
    //    刷新 EP"，所以要记下上一根结算时的值。不落盘：恢复后为 0，见 step 里的处理
    double psar_ep_mark = 0;
    int    consec_reverses = 0;
    int    cooldown_left   = 0;   // 剩余冷却K线数
    // ③ 专用：最后一次开仓所在K线的开盘时间。once_per_bar 靠它判"这根开过了"
    int64_t last_entry_bar_ms = 0;
};

struct Inputs {
    double price  = 0;
    double atr    = 0;       // 0 = 数据不足（indicators::atr 的约定）
    bool   dc_ok  = false;   // 唐奇安通道是否可用
    double dc_up  = 0;
    double dc_dn  = 0;
    bool   new_bar = false;  // 本 tick 是否跨入新K线（冷却按K线计数）
    int64_t bar_open_ms = 0; // 当前这根K线的开盘时间

    // ── 裸K / PSAR 的K线输入 ───────────────────────────────────────────────
    bool   bar_ok      = false;  // K线数据是否就绪
    double bar_open    = 0;      // 【当前这根（未收盘）】的开盘价 —— Immediate 模式用
    double prev_close  = 0;      // 刚收盘那根的收盘价     —— BreakPrevBar 用
    double prev_high   = 0;      // 上一根（收盘根之前那根）的最高价
    double prev_low    = 0;      // 上一根的最低价
    double swing_low   = 0;      // bar 0 之前 N 根的最低价
    double swing_high  = 0;      // bar 0 之前 N 根的最高价
    // ② PSAR 需要【前两根】的高低点做夹逼（Wilder 原版规则）
    double psar_prev_high  = 0, psar_prev_low  = 0;
    double psar_prev2_high = 0, psar_prev2_low = 0;
};

enum class Action {
    Hold,
    OpenLong,
    OpenShort,
    Close,              // 平掉当前仓位，转观望
    CloseReverseLong,   // 平空并反手做多
    CloseReverseShort,  // 平多并反手做空
};

struct Verdict {
    Action      action = Action::Hold;
    const char* reason = "";
    double      stop   = 0;   // 本 tick 结束后的止损线（供 UI/日志显示）
    bool        profitable_exit = false;   // 本次出场是否 >= 成本价
};

// 数据是否足以【开新仓】。三个策略要的东西不同：
//   海龟：ATR（止损线靠它）+ 通道
//   PSAR：前两根的高低点（SAR 的递推与夹逼都要），不需要 ATR
//   裸K：K线本身 + 摆动极值，不需要 ATR
// 已有仓位不受影响，仍按最后一条有效止损线守着
inline bool data_ready(const Inputs& in, const Config& cfg) {
    if (!(in.price > 0 && std::isfinite(in.price))) return false;
    switch (cfg.strategy) {
    case Strategy::BareK:
        if (!(in.bar_ok && in.swing_low > 0 && in.swing_high > 0)) return false;
        return (cfg.bare_entry == BareEntry::Immediate)
                   ? in.bar_open > 0
                   : (in.prev_close > 0 && in.prev_high > 0 && in.prev_low > 0);
    case Strategy::ParabolicSar:
        return in.bar_ok && in.psar_prev_high > 0 && in.psar_prev_low > 0;
    case Strategy::Turtle:
    default:
        return in.atr > 0 && in.dc_ok;
    }
}

// 突破判定。用 >= / <=：通道沿本身就算突破（价格刚好打平前高即视为创新高）
inline bool break_up  (const Inputs& in) { return in.dc_ok && in.price >= in.dc_up; }
inline bool break_down(const Inputs& in) { return in.dc_ok && in.price <= in.dc_dn; }

// 同一根K线是否已经开过仓（③ 的 once_per_bar 护栏）
inline bool bar_already_used(const State& st, const Config& cfg, const Inputs& in) {
    return cfg.strategy == Strategy::BareK && cfg.once_per_bar &&
           in.bar_open_ms != 0 && st.last_entry_bar_ms == in.bar_open_ms;
}

// 入场信号（策略无关的统一入口）。
inline bool entry_long(const Inputs& in, const Config& cfg) {
    switch (cfg.strategy) {
    case Strategy::BareK:
        if (!in.bar_ok) return false;
        if (cfg.bare_entry == BareEntry::Immediate)
            return in.bar_open > 0 && in.price > in.bar_open;
        // BreakPrevBar：只在【跨新K线】那一拍成立——规格是"本根收盘时判定"，
        // 不加这道门的话同一根K线的每个 tick 都会重复触发
        return in.new_bar && in.prev_close > 0 && in.prev_high > 0 &&
               in.prev_close > in.prev_high;
    case Strategy::ParabolicSar:
        // PSAR 空仓时的入场：价格站上 SAR 值即做多。
        // 首次建仓没有历史 SAR，退化成"上破前一根最高价"
        return in.bar_ok && in.psar_prev_high > 0 && in.price > in.psar_prev_high;
    case Strategy::Turtle:
    default:
        return break_up(in);
    }
}
inline bool entry_short(const Inputs& in, const Config& cfg) {
    switch (cfg.strategy) {
    case Strategy::BareK:
        if (!in.bar_ok) return false;
        if (cfg.bare_entry == BareEntry::Immediate)
            return in.bar_open > 0 && in.price < in.bar_open;
        return in.new_bar && in.prev_close > 0 && in.prev_low > 0 &&
               in.prev_close < in.prev_low;
    case Strategy::ParabolicSar:
        return in.bar_ok && in.psar_prev_low > 0 && in.price < in.psar_prev_low;
    case Strategy::Turtle:
    default:
        return break_down(in);
    }
}

// Chandelier 止损线。多头挂在极值下方，空头挂在极值上方
inline double chandelier(Pos p, double peak, double atr, double k) {
    return p == Pos::Long ? peak - k * atr : peak + k * atr;
}

// ② 抛物线 SAR 的递推：给出【下一根】的 SAR 值。
//
// ⚠ Wilder 原版的夹逼规则容易被漏掉，漏了会在震荡里提前被打：
//   SAR 不得落进【前两根】的价格范围内。多头时取 min(SAR, 前一根最低, 前两根最低)，
//   空头时取 max(...)。直觉是"止损不该设在价格刚刚走过的地方"。
inline double psar_next(Pos p, double sar, double ep, double af, const Inputs& in) {
    double next = sar + af * (ep - sar);
    if (p == Pos::Long) {
        if (in.psar_prev_low  > 0) next = std::min(next, in.psar_prev_low);
        if (in.psar_prev2_low > 0) next = std::min(next, in.psar_prev2_low);
    } else {
        if (in.psar_prev_high  > 0) next = std::max(next, in.psar_prev_high);
        if (in.psar_prev2_high > 0) next = std::max(next, in.psar_prev2_high);
    }
    return next;
}

// 本 tick 的止损【候选线】。棘轮由调用方施加——三个策略都需要它：
//   海龟：ATR 抖动会让候选线来回跳
//   PSAR：夹逼可能让候选线倒退
//   裸K：窗口最小值理论上自带棘轮，但信号根的下影与采样这两个例外会让它下移
// 返回 0 = 本 tick 没有有效候选，调用方应沿用旧线（绝不撤保护）
inline double stop_candidate(Pos p, const State& st, const Config& cfg,
                             const Inputs& in) {
    switch (cfg.strategy) {
    case Strategy::BareK: {
        if (!in.bar_ok) return 0;
        const double sw = (p == Pos::Long) ? in.swing_low : in.swing_high;
        return sw > 0 ? sw : 0;
    }
    case Strategy::ParabolicSar:
        if (!(st.stop > 0) || !in.bar_ok) return 0;
        return psar_next(p, st.stop, st.peak, st.af, in);
    case Strategy::Turtle:
    default:
        if (!(in.atr > 0)) return 0;
        return chandelier(p, st.peak, in.atr, cfg.atr_mult);
    }
}

// 推进一个 tick：更新极值与棘轮止损线，然后给出动作。
// st 会被就地修改（极值/止损/AF/冷却计数），仓位状态的变更由【调用方】在订单
// 真正成交后写入——决策函数不假定下单一定成功。
inline Verdict step(State& st, const Config& cfg, const Inputs& in) {
    Verdict v;

    if (in.new_bar && st.cooldown_left > 0) --st.cooldown_left;

    if (!(in.price > 0 && std::isfinite(in.price))) {
        v.reason = "价格非法";
        v.stop   = st.stop;
        return v;
    }

    // ── 持仓中：先推极值和止损线，再判是否触线 ──────────────────────────────
    if (st.pos != Pos::Flat) {
        const bool is_long = (st.pos == Pos::Long);

        st.peak = is_long ? std::max(st.peak, in.price)
                          : std::min(st.peak, in.price);

        // ② PSAR：递推与 AF 加速【每根K线只做一次】，在新K线开始的那一拍。
        //
        // ⚠ v5.9.10 之前这里每个 tick 都递推（引擎 3 秒一拍）。SAR 公式是"每根K线
        //   朝 EP 靠近 AF"，一根K线里被递推上百次，止损线很快就贴到夹逼上限
        //   ——前两根K线的最低价（做空时最高价）。"刚开仓时每根只靠拢 2%，给趋势
        //   留呼吸空间"这个机制在实盘里完全失效；AF 也会因为K线内每个新高都 +step
        //   而远快于设计地封顶。单元测试一直是"每根K线调用一次"，所以没抓到。
        //   实测（同一根K线内调用 100 次）：SAR 从 90 到 99，按设计应为 ≈ 90.3。
        //
        //   EP 仍然每拍更新（K线内的新高要算进去），只是 AF 是否加速、SAR 推多少，
        //   留到下一根K线开始时按"这根K线有没有刷新 EP"一次性结算。
        //   新K线那一拍的价格会先并进 EP 再结算——只差一个 3 秒的采样，可以忽略
        if (cfg.strategy == Strategy::ParabolicSar) {
            if (in.new_bar) {
                // 标记为 0：刚从落盘恢复（这个字段不落盘），以当前 EP 起算，
                // 不因为"跟 0 比"就白加一次 AF
                if (!(st.psar_ep_mark > 0)) st.psar_ep_mark = st.peak;
                const bool new_ep = is_long ? (st.peak > st.psar_ep_mark)
                                            : (st.peak < st.psar_ep_mark);
                if (new_ep) st.af = std::min(cfg.af_max, st.af + cfg.af_step);
                st.psar_ep_mark = st.peak;
                if (const double cand = stop_candidate(st.pos, st, cfg, in); cand > 0)
                    st.stop = is_long ? std::max(st.stop, cand) : std::min(st.stop, cand);
            }
        } else if (const double cand = stop_candidate(st.pos, st, cfg, in); cand > 0) {
            // 数据缺失时不推新线，沿用上一条有效止损线——绝不因为数据断流就撤掉保护
            st.stop = is_long ? std::max(st.stop, cand)    // 棘轮：只上不下
                              : std::min(st.stop, cand);
        }
        v.stop = st.stop;

        const bool hit = is_long ? (in.price <= st.stop) : (in.price >= st.stop);
        if (!hit) {
            v.reason = "持仓中";
            return v;
        }

        // 触线出场。盈亏以【止损线】而非现价衡量：现价可能已经穿过线更多，
        // 但决策语义是"回撤到线就走"，用线判定才和事前预期一致
        const double exit_px = st.stop;
        v.profitable_exit = is_long ? (exit_px >= st.entry_price)
                                    : (exit_px <= st.entry_price);

        // 盈利出场 → 趋势跑完了，但【力竭不等于反转】，不反手，等新信号。
        // ⚠ PSAR 是例外：它的定义就是"触及即翻转"，盈利触及同样翻转
        if (v.profitable_exit && cfg.strategy != Strategy::ParabolicSar) {
            v.action = Action::Close;
            v.reason = "追踪止盈出场，等待新信号";
            st.consec_reverses = 0;
            return v;
        }

        if (cfg.reverse == ReverseMode::None) {
            v.action = Action::Close;
            v.reason = v.profitable_exit ? "触线出场（不反手）" : "止损出场（不反手）";
            return v;
        }
        // 连续反手上限（0 = 不限）。震荡市里这是唯一的刹车
        if (cfg.max_consecutive_reverses > 0 &&
            st.consec_reverses >= cfg.max_consecutive_reverses) {
            v.action = Action::Close;
            v.reason = "出场：连续反手已达上限，转冷却";
            st.cooldown_left = cfg.cooldown_bars;
            st.consec_reverses = 0;
            return v;
        }
        // 反手要开新仓，而新仓需要一条止损线。拿不到就只平不反手
        if (!data_ready(in, cfg)) {
            v.action = Action::Close;
            v.reason = "出场：数据缺失，不反手";
            return v;
        }
        v.action = is_long ? Action::CloseReverseShort : Action::CloseReverseLong;
        v.reason = "出场并反手";
        return v;
    }

    // ── 空仓：等入场信号 ──────────────────────────────────────────────────────
    v.stop = 0;
    if (st.cooldown_left > 0)   { v.reason = "冷却中"; return v; }
    if (!data_ready(in, cfg))   { v.reason = "数据不足，不开新仓"; return v; }
    // 每根K线最多开一次：被止损出场后不在同一根里重新入场
    if (bar_already_used(st, cfg, in)) { v.reason = "本根已开过（每根一次）"; return v; }

    if (entry_long(in, cfg))  { v.action = Action::OpenLong;  v.reason = "多头信号"; return v; }
    if (entry_short(in, cfg)) { v.action = Action::OpenShort; v.reason = "空头信号"; return v; }

    v.reason = "无信号";
    return v;
}

// 建仓时的初始止损线。三个策略各算各的：
//   海龟：Chandelier（极值就是成交价本身）
//   PSAR：前一根的反向极值——SAR 的起点是"趋势开始前的那个极值"
//   裸K：bar 0 之【前】N 根的摆动极值，不含信号根自己
inline double initial_stop(Pos p, double fill_price, const Config& cfg,
                           const Inputs& in) {
    switch (cfg.strategy) {
    case Strategy::BareK: {
        const double sw = (p == Pos::Long) ? in.swing_low : in.swing_high;
        return sw > 0 ? sw : 0;
    }
    case Strategy::ParabolicSar: {
        const double s = (p == Pos::Long) ? in.psar_prev_low : in.psar_prev_high;
        return s > 0 ? s : 0;
    }
    case Strategy::Turtle:
    default:
        return chandelier(p, fill_price, in.atr, cfg.atr_mult);
    }
}

inline void on_filled(State& st, Pos p, double fill_price, double stop_price,
                      const Config& cfg, bool from_reverse, int64_t bar_open_ms = 0) {
    st.pos         = p;
    st.entry_price = fill_price;
    st.peak        = fill_price;
    st.stop        = stop_price;
    st.consec_reverses = from_reverse ? st.consec_reverses + 1 : 0;
    // 新一轮的 AF 从初始值重新加速。反手开出来的仓位同样——上一轮攒到的
    // 加速度属于上一段趋势，带过来会让新仓一开始就被贴得很紧
    st.af = (cfg.strategy == Strategy::ParabolicSar) ? cfg.af_start : 0.0;
    st.psar_ep_mark = fill_price;   // 建仓那一刻的 EP 就是成交价
    if (bar_open_ms != 0) st.last_entry_bar_ms = bar_open_ms;
}

inline void on_closed(State& st) {
    st.pos = Pos::Flat;
    st.entry_price = st.peak = st.stop = 0;
    st.af = 0;
    st.psar_ep_mark = 0;
    // ⚠ 不清 last_entry_bar_ms：once_per_bar 的语义正是"这一根【开过了】"，
    //   平仓后清掉的话同一根里会立刻再开一次，护栏等于没有
}

inline const char* pos_name(Pos p) {
    return p == Pos::Long ? "多" : (p == Pos::Short ? "空" : "空仓");
}

inline const char* strategy_name(Strategy s) {
    switch (s) {
    case Strategy::ParabolicSar: return "抛物线SAR";
    case Strategy::BareK:        return "纯裸K";
    case Strategy::Turtle:
    default:                     return "海龟";
    }
}

} // namespace ccbot::trend
