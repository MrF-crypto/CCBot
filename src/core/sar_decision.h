#pragma once
#include <algorithm>
#include <cmath>
#include <string>

// 趋势跟随 + 止损反转（SAR）的决策核心。
//
// 和 CcgEngine（DCA 网格）是【两套并列的策略】，不是一套的两种模式：
//   DCA   —— 不止损、靠分层摊薄，核心不变量是 名义仓位 ≤ 权益 ⇒ 不可强平
//   SAR   —— 每笔必止损、单仓位、靠少数几笔跑远覆盖大量小亏
// 两者的风险形状是镜像的，混在一个引擎里只会让两边都变形。
//
// 本文件和 decision.h 一样：纯函数、无状态依赖、不碰网络，便于单测穷举。
//
// ── 策略构成 ────────────────────────────────────────────────────────────────
//   入场信号  唐奇安通道突破（价格上破 N 根最高 → 多；下破 N 根最低 → 空）
//   出场      Chandelier ATR 追踪止损（持仓期极值 ∓ k×ATR，棘轮，只朝有利方向移动）
//   反手      亏损止损后按规则反向入场；盈利止盈后【不反手】，等新信号
//
// 为什么出场只有追踪止损、没有固定止盈：趋势跟随的胜率天然只有 30~40%，
// 全部收益来自少数几笔跑得很远的单子。任何固定止盈都会把这些单子提前砍断，
// 而亏损笔的大小不变——等于单方面砍掉盈利分布的右尾，期望必然转负。
namespace ccbot::sar {

enum class Pos { Flat, Long, Short };

// 入场与止损的两套算法。刻意做成一个开关而不是两个：它们是成套的，
// 混搭（比如唐奇安入场 + 摆动低点止损）没有实证依据，多开一个维度只会
// 让参数空间翻倍而没人知道该怎么填。
enum class Mode {
    Donchian,    // 唐奇安通道突破入场 + Chandelier ATR 止损（海龟）
    BarPattern,  // 裸K线：阳线入场 + 最近 N 根摆动低点止损
};

struct Config {
    Mode   mode = Mode::Donchian;

    int    donchian_period = 20;    // 入场通道周期（海龟原版 20）
    int    atr_period      = 14;    // ATR 周期
    double atr_mult        = 3.0;   // Chandelier k：止损距离 = k×ATR

    // ── 裸K线模式 ───────────────────────────────────────────────────────────
    // 入场：刚收盘那根（bar 0）收盘价 > 开盘价 ⇒ 做多；反之做空。
    // 止损：bar 0 之【前】的 swing_bars 根的最低价（做多）/ 最高价（做空）。
    //       窗口随K线右移，棘轮只朝有利方向。
    //
    // ⚠ 窗口最小值的止损，棘轮【理论上是自带的】：M 只有在新K线的最低价
    //   跌破旧 M 时才会下移，而那一刻价格已经穿过止损线了。但有两个例外
    //   必须靠显式棘轮兜住：
    //     ① 信号根自己的低点不在初始窗口里（初始窗口是 1..N，不含 bar 0）。
    //        bar 0 若是长下影的锤子线，它进入窗口时会把 M 拉下去，而那根
    //        下影发生在【入场之前】，没打到我们
    //     ② 引擎每 3 秒采样一次标记价，而K线的 low 记录的是连续极值。
    //        一根 2 秒的插针引擎看不见，但下一根K线的 low 会记下来
    int    swing_bars = 3;

    // ── 反手规则 ────────────────────────────────────────────────────────────
    bool   allow_reverse   = true;  // 亏损止损后是否反向入场

    // ⚠ 强烈建议保持 true。无条件反手在震荡市里是绞肉机：
    //   亏损止损【在震荡市最频繁】，而无条件反手恰好在那时最激进——
    //   开多 → 跌 k×ATR 止损 → 反手开空 → 涨回来 k×ATR 止损 → 反手开多 …
    //   ATR 常态 2% 时单次绞杀约 6% 名义，3 倍杠杆即保证金的 18%。
    //   要求反向信号成立，等于只在【趋势真的翻了】时反手，而不是"我被打了所以反着来"。
    bool   reverse_needs_signal = true;

    // 连续反手上限，超过即强制冷却。真趋势不需要连续反手多次，
    // 连续反手本身就是"当前是震荡市"的信号
    int    max_consecutive_reverses = 2;
    int    cooldown_bars            = 3;   // 触顶后冷却多少根K线（0=不冷却）

    // ── 金字塔加仓（0=关）──────────────────────────────────────────────────
    // 探测仓开出来之后，每朝有利方向再走 pyramid_step_atr 个 ATR 就加一档，
    // 最多加 pyramid_max_adds 档。海龟原版的做法。
    //
    // 它回答的是"怎么低成本试出单边大行情"：错了只亏第一档，对了越骑越重。
    // 与 DCA 的补仓【方向相反】——DCA 是跌了加（摊薄），这里是涨了加（顺势）。
    //
    // ⚠ 加仓不额外挪止损线：Chandelier 棘轮本来就跟着极值走，加仓时极值已经
    //   推上去了，线自然在更高的位置。再单独挪一次等于把两套逻辑叠在一起。
    // ⚠ 加完之后 entry_price 变成【加权均价】，否则"出场价≥成本"会按第一档
    //   的价格判，把一笔实际亏损的出场误判成盈利出场（进而不反手）
    int    pyramid_max_adds  = 0;
    double pyramid_step_atr  = 0.5;
};

struct State {
    Pos    pos         = Pos::Flat;
    double entry_price = 0;
    double peak        = 0;   // 持仓期极值：多=最高价，空=最低价
    double stop        = 0;   // 棘轮止损线，只朝有利方向移动
    int    consec_reverses = 0;
    int    cooldown_left   = 0;   // 剩余冷却K线数
    // 金字塔加仓状态。last_add_price 是【最近一档的成交价】，不是首档——
    // 加仓间距从上一档量起（海龟口径），否则越加越密
    int    adds_done       = 0;
    double last_add_price  = 0;
};

struct Inputs {
    double price  = 0;
    double atr    = 0;       // 0 = 数据不足（indicators::atr 的约定）
    bool   dc_ok  = false;   // 唐奇安通道是否可用
    double dc_up  = 0;
    double dc_dn  = 0;
    bool   new_bar = false;  // 本 tick 是否跨入新K线（冷却按K线计数）

    // ── 裸K线模式的输入 ────────────────────────────────────────────────────
    bool   bar_ok      = false;  // 裸K线数据是否就绪
    bool   bar_bullish = false;  // 刚收盘那根是阳线（收盘>开盘）
    bool   bar_bearish = false;  // 刚收盘那根是阴线。十字星两者皆 false
    double swing_low   = 0;      // bar 0 之前 N 根的最低价
    double swing_high  = 0;      // bar 0 之前 N 根的最高价
};

enum class Action {
    Hold,
    OpenLong,
    OpenShort,
    Close,              // 平掉当前仓位，转观望
    CloseReverseLong,   // 平空并反手做多
    CloseReverseShort,  // 平多并反手做空
    Add,                // 金字塔加仓（方向同当前持仓）
};

struct Verdict {
    Action      action = Action::Hold;
    const char* reason = "";
    double      stop   = 0;   // 本 tick 结束后的止损线（供 UI/日志显示）
    bool        profitable_exit = false;   // 本次出场是否 >= 成本价
};

// 数据是否足以【开新仓】。两种模式要的东西不同：
//   唐奇安：要 ATR（止损线靠它）+ 通道
//   裸K线：只要K线本身。止损线是摆动低点，不用 ATR
// 已有仓位不受影响，仍按最后一条有效止损线守着
inline bool data_ready(const Inputs& in, const Config& cfg) {
    if (!(in.price > 0 && std::isfinite(in.price))) return false;
    if (cfg.mode == Mode::BarPattern)
        return in.bar_ok && in.swing_low > 0 && in.swing_high > 0;
    return in.atr > 0 && in.dc_ok;
}

// 突破判定。用 >= / <=：通道沿本身就算突破（价格刚好打平前高即视为创新高）
inline bool break_up  (const Inputs& in) { return in.dc_ok && in.price >= in.dc_up; }
inline bool break_down(const Inputs& in) { return in.dc_ok && in.price <= in.dc_dn; }

// 入场信号（模式无关的统一入口）。
// ⚠ 裸K线模式只在【跨新K线】那一拍成立：规格是"bar 0 收盘时判定"，
//   不加这道门的话，同一根K线的每个 tick 都会重复开仓
inline bool entry_long(const Inputs& in, const Config& cfg) {
    if (cfg.mode == Mode::BarPattern) return in.new_bar && in.bar_ok && in.bar_bullish;
    return break_up(in);
}
inline bool entry_short(const Inputs& in, const Config& cfg) {
    if (cfg.mode == Mode::BarPattern) return in.new_bar && in.bar_ok && in.bar_bearish;
    return break_down(in);
}

// Chandelier 止损线。多头挂在极值下方，空头挂在极值上方
inline double chandelier(Pos p, double peak, double atr, double k) {
    return p == Pos::Long ? peak - k * atr : peak + k * atr;
}

// 本 tick 的止损【候选线】。棘轮由调用方施加——两种模式都需要它：
//   唐奇安：ATR 抖动会让候选线来回跳
//   裸K线：窗口最小值理论上自带棘轮，但信号根的下影与 3 秒采样这两个
//          例外会让它下移（详见 Config::swing_bars 的说明）
// 返回 0 = 本 tick 没有有效候选，调用方应沿用旧线（绝不撤保护）
inline double stop_candidate(Pos p, const State& st, const Config& cfg,
                             const Inputs& in) {
    if (cfg.mode == Mode::BarPattern) {
        if (!in.bar_ok) return 0;
        const double sw = (p == Pos::Long) ? in.swing_low : in.swing_high;
        return sw > 0 ? sw : 0;
    }
    if (!(in.atr > 0)) return 0;
    return chandelier(p, st.peak, in.atr, cfg.atr_mult);
}

// 推进一个 tick：更新极值与棘轮止损线，然后给出动作。
// st 会被就地修改（极值/止损/冷却计数），仓位状态的变更由【调用方】在订单
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

        // 数据缺失时不推新线，沿用上一条有效止损线——绝不因为数据断流就撤掉保护
        if (const double cand = stop_candidate(st.pos, st, cfg, in); cand > 0) {
            st.stop = is_long ? std::max(st.stop, cand)    // 棘轮：只上不下
                              : std::min(st.stop, cand);
        }
        v.stop = st.stop;

        const bool hit = is_long ? (in.price <= st.stop) : (in.price >= st.stop);
        if (!hit) {
            // 没触线 → 看要不要金字塔加仓。顺序不能反：先判出场再判加仓，
            // 否则同一 tick 里既触线又够加仓间距时会先加一档再被平掉
            if (cfg.pyramid_max_adds > 0 && st.adds_done < cfg.pyramid_max_adds &&
                in.atr > 0 && cfg.pyramid_step_atr > 0 && st.last_add_price > 0) {
                const double step_px = cfg.pyramid_step_atr * in.atr;
                const bool far_enough = is_long
                    ? (in.price >= st.last_add_price + step_px)
                    : (in.price <= st.last_add_price - step_px);
                if (far_enough) {
                    v.action = Action::Add;
                    v.reason = "顺势加仓";
                    return v;
                }
            }
            v.reason = "持仓中";
            return v;
        }

        // 触线出场。盈亏以【止损线】而非现价衡量：现价可能已经穿过线更多，
        // 但决策语义是"回撤到线就走"，用线判定才和事前预期一致
        const double exit_px = st.stop;
        v.profitable_exit = is_long ? (exit_px >= st.entry_price)
                                    : (exit_px <= st.entry_price);

        // 盈利出场 → 趋势跑完了，但【力竭不等于反转】，不反手，等新信号
        if (v.profitable_exit) {
            v.action = Action::Close;
            v.reason = "追踪止盈出场，等待新信号";
            st.consec_reverses = 0;
            return v;
        }

        // 亏损出场 → 可能是判断错了方向
        if (!cfg.allow_reverse) {
            v.action = Action::Close;
            v.reason = "止损出场（反手已关闭）";
            return v;
        }
        if (st.consec_reverses >= cfg.max_consecutive_reverses) {
            v.action = Action::Close;
            v.reason = "止损出场：连续反手已达上限，转冷却";
            st.cooldown_left = cfg.cooldown_bars;
            st.consec_reverses = 0;
            return v;
        }
        // 反手是否需要反向信号确认
        const bool sig = is_long ? entry_short(in, cfg) : entry_long(in, cfg);
        if (cfg.reverse_needs_signal && !sig) {
            v.action = Action::Close;
            v.reason = "止损出场：反向信号未成立，转观望";
            return v;
        }
        // 反手要开新仓，而新仓需要一条止损线。拿不到就只平不反手
        if (!data_ready(in, cfg)) {
            v.action = Action::Close;
            v.reason = "止损出场：数据缺失，不反手";
            return v;
        }
        v.action = is_long ? Action::CloseReverseShort : Action::CloseReverseLong;
        v.reason = "止损出场并反手";
        return v;
    }

    // ── 空仓：等入场信号 ──────────────────────────────────────────────────────
    v.stop = 0;
    if (st.cooldown_left > 0)   { v.reason = "冷却中"; return v; }
    if (!data_ready(in, cfg))   { v.reason = "数据不足，不开新仓"; return v; }

    const bool bar_mode = (cfg.mode == Mode::BarPattern);
    if (entry_long(in, cfg)) {
        v.action = Action::OpenLong;
        v.reason = bar_mode ? "阳线收盘" : "上破通道";
        return v;
    }
    if (entry_short(in, cfg)) {
        v.action = Action::OpenShort;
        v.reason = bar_mode ? "阴线收盘" : "下破通道";
        return v;
    }

    v.reason = bar_mode ? "等本根收盘" : "无信号";
    return v;
}

// 建仓成交后由调用方调用：写入仓位状态并布下初始止损线。
// 初始线同样是 Chandelier，只是极值就是成交价本身
// 建仓时的初始止损线。唐奇安模式用 Chandelier（极值就是成交价本身）；
// 裸K线模式用 bar 0 之【前】N 根的摆动低点——注意它不含信号根自己
inline double initial_stop(Pos p, double fill_price, const Config& cfg,
                           const Inputs& in) {
    if (cfg.mode == Mode::BarPattern) {
        const double sw = (p == Pos::Long) ? in.swing_low : in.swing_high;
        if (sw > 0) return sw;
    }
    return chandelier(p, fill_price, in.atr, cfg.atr_mult);
}

inline void on_filled(State& st, Pos p, double fill_price, double stop_price,
                      const Config& cfg, bool from_reverse) {
    st.pos         = p;
    st.entry_price = fill_price;
    st.peak        = fill_price;
    st.stop        = stop_price;
    st.consec_reverses = from_reverse ? st.consec_reverses + 1 : 0;
    // 新一轮的金字塔计数归零。反手开出来的仓位同样从第 0 档重新算——
    // 否则上一轮加过的档数会让这一轮少加几档
    st.adds_done      = 0;
    st.last_add_price = fill_price;
}

// 加仓成交后调用。new_avg_entry 由引擎按数量加权算出并传进来：
// 决策核心不跟踪持仓数量，但 entry_price 必须是加权均价，否则
// "出场价≥成本"会拿第一档的价格去判，把实际亏损的出场误判成盈利出场
inline void on_added(State& st, double fill_price, double new_avg_entry) {
    ++st.adds_done;
    st.last_add_price = fill_price;
    if (new_avg_entry > 0) st.entry_price = new_avg_entry;
}

inline void on_closed(State& st) {
    st.pos = Pos::Flat;
    st.entry_price = st.peak = st.stop = 0;
    st.adds_done = 0;
    st.last_add_price = 0;
}

inline const char* pos_name(Pos p) {
    return p == Pos::Long ? "多" : (p == Pos::Short ? "空" : "空仓");
}

} // namespace ccbot::sar
