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

struct Config {
    int    donchian_period = 20;    // 入场通道周期（海龟原版 20）
    int    atr_period      = 14;    // ATR 周期
    double atr_mult        = 3.0;   // Chandelier k：止损距离 = k×ATR

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
};

struct State {
    Pos    pos         = Pos::Flat;
    double entry_price = 0;
    double peak        = 0;   // 持仓期极值：多=最高价，空=最低价
    double stop        = 0;   // 棘轮止损线，只朝有利方向移动
    int    consec_reverses = 0;
    int    cooldown_left   = 0;   // 剩余冷却K线数
};

struct Inputs {
    double price  = 0;
    double atr    = 0;       // 0 = 数据不足（indicators::atr 的约定）
    bool   dc_ok  = false;   // 唐奇安通道是否可用
    double dc_up  = 0;
    double dc_dn  = 0;
    bool   new_bar = false;  // 本 tick 是否跨入新K线（冷却按K线计数）
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

// 数据是否足以做任何决策。缺 ATR 就没有止损线，缺通道就没有信号——
// 两者任一缺失都【不开新仓】，但已有仓位仍按最后一条有效止损线守着
inline bool data_ready(const Inputs& in) {
    return in.price > 0 && std::isfinite(in.price) && in.atr > 0 && in.dc_ok;
}

// 突破判定。用 >= / <=：通道沿本身就算突破（价格刚好打平前高即视为创新高）
inline bool break_up  (const Inputs& in) { return in.dc_ok && in.price >= in.dc_up; }
inline bool break_down(const Inputs& in) { return in.dc_ok && in.price <= in.dc_dn; }

// Chandelier 止损线。多头挂在极值下方，空头挂在极值上方
inline double chandelier(Pos p, double peak, double atr, double k) {
    return p == Pos::Long ? peak - k * atr : peak + k * atr;
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

        // ATR 缺失时不推新线，沿用上一条有效止损线——绝不因为数据断流就撤掉保护
        if (in.atr > 0) {
            double cand = chandelier(st.pos, st.peak, in.atr, cfg.atr_mult);
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
        const bool sig = is_long ? break_down(in) : break_up(in);
        if (cfg.reverse_needs_signal && !sig) {
            v.action = Action::Close;
            v.reason = "止损出场：反向信号未成立，转观望";
            return v;
        }
        if (in.atr <= 0) {   // 反手要开新仓，没有 ATR 就没有新止损线
            v.action = Action::Close;
            v.reason = "止损出场：ATR 缺失，不反手";
            return v;
        }
        v.action = is_long ? Action::CloseReverseShort : Action::CloseReverseLong;
        v.reason = "止损出场并反手";
        return v;
    }

    // ── 空仓：等入场信号 ──────────────────────────────────────────────────────
    v.stop = 0;
    if (st.cooldown_left > 0) { v.reason = "冷却中"; return v; }
    if (!data_ready(in))      { v.reason = "数据不足，不开新仓"; return v; }

    if (break_up(in))   { v.action = Action::OpenLong;  v.reason = "上破通道"; return v; }
    if (break_down(in)) { v.action = Action::OpenShort; v.reason = "下破通道"; return v; }

    v.reason = "无信号";
    return v;
}

// 建仓成交后由调用方调用：写入仓位状态并布下初始止损线。
// 初始线同样是 Chandelier，只是极值就是成交价本身
inline void on_filled(State& st, Pos p, double fill_price, double atr,
                      const Config& cfg, bool from_reverse) {
    st.pos         = p;
    st.entry_price = fill_price;
    st.peak        = fill_price;
    st.stop        = chandelier(p, fill_price, atr, cfg.atr_mult);
    st.consec_reverses = from_reverse ? st.consec_reverses + 1 : 0;
}

inline void on_closed(State& st) {
    st.pos = Pos::Flat;
    st.entry_price = st.peak = st.stop = 0;
}

inline const char* pos_name(Pos p) {
    return p == Pos::Long ? "多" : (p == Pos::Short ? "空" : "空仓");
}

} // namespace ccbot::sar
