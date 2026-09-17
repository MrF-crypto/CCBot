// SAR（趋势跟随 + 止损反转）决策核心的单元测试——纯计算，不连网络。
// 全部通过打印 OK 并 exit 0，否则打印失败断言并 exit 1。
#include "core/sar_decision.h"
#include "core/indicators.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace ccbot::sar;
using ccbot::indicators::Ohlc;

static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail; \
        } \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) < eps;
}

// 构造一个"有信号"的输入：通道 [90,110]，ATR=2
static Inputs mk(double price, double atr = 2.0,
                 double up = 110, double dn = 90) {
    Inputs in;
    in.price = price; in.atr = atr;
    in.dc_ok = true;  in.dc_up = up; in.dc_dn = dn;
    return in;
}

int main() {
    Config cfg;   // 默认：20/14/k=3，反手开启且需信号，上限2次，冷却3根

    // ── 入场 ─────────────────────────────────────────────────────────────────
    {
        State st;
        auto v = step(st, cfg, mk(100));
        CHECK(v.action == Action::Hold, "区间内不应开仓");

        v = step(st, cfg, mk(110));
        CHECK(v.action == Action::OpenLong, "触及上沿即算上破，应开多");

        State st2;
        v = step(st2, cfg, mk(90));
        CHECK(v.action == Action::OpenShort, "触及下沿应开空");
    }

    // ── 数据缺失不开新仓，但不撤已有保护 ───────────────────────────────────────
    {
        State st;
        auto no_atr = mk(115); no_atr.atr = 0;
        CHECK(step(st, cfg, no_atr).action == Action::Hold, "ATR 缺失不应开仓");

        auto no_dc = mk(115); no_dc.dc_ok = false;
        CHECK(step(st, cfg, no_dc).action == Action::Hold, "通道缺失不应开仓");

        // 持仓中 ATR 断流：止损线必须原样保留
        State h;
        on_filled(h, Pos::Long, 100, 2.0, cfg, false);
        const double line0 = h.stop;                 // 100 - 3*2 = 94
        CHECK(near(line0, 94.0), "初始止损线应为 成交价-k*ATR");
        auto drop = mk(101); drop.atr = 0;
        auto v = step(h, cfg, drop);
        CHECK(near(h.stop, line0), "ATR 断流时止损线必须沿用，不得撤销");
        CHECK(v.action == Action::Hold, "未触线应继续持有");
    }

    // ── 棘轮：止损线只朝有利方向移动 ───────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 2.0, cfg, false);
        CHECK(near(st.stop, 94.0), "初始线 94");

        step(st, cfg, mk(120));                      // 极值 120 → 线 114
        CHECK(near(st.stop, 114.0), "新高后线应上移到 114");

        step(st, cfg, mk(115));                      // 回落，极值不变
        CHECK(near(st.stop, 114.0), "回落时线不得下移");

        // ATR 放大也不能让线回退
        step(st, cfg, mk(116, 10.0));                // 极值仍120 → 候选 90
        CHECK(near(st.stop, 114.0), "ATR 放大导致候选线更低时，棘轮必须挡住");
    }

    // ── 盈利出场：不反手，等新信号 ─────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 2.0, cfg, false);
        step(st, cfg, mk(120));                      // 线上移到 114（> 成本100）
        // 价格回落到线，且此刻恰好也下破通道（制造"反向信号成立"的诱惑）
        auto v = step(st, cfg, mk(114, 2.0, 130, 114));
        CHECK(v.action == Action::Close, "盈利出场必须只平不反手");
        CHECK(v.profitable_exit, "出场价 114 > 成本 100，应判为盈利出场");
        CHECK(st.consec_reverses == 0, "盈利出场应清零连续反手计数");
    }

    // ── 亏损出场 + 反向信号成立 → 反手 ────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 2.0, cfg, false);   // 线 94
        auto v = step(st, cfg, mk(94, 2.0, 130, 94));     // 触线，且同时下破通道
        CHECK(v.action == Action::CloseReverseShort, "亏损且反向信号成立应反手");
        CHECK(!v.profitable_exit, "94 < 100 应判为亏损出场");
    }

    // ── 亏损出场 + 反向信号未成立 → 只平不反手（震荡市保命符）────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 2.0, cfg, false);
        auto v = step(st, cfg, mk(94, 2.0, 130, 80));     // 触线但未下破通道
        CHECK(v.action == Action::Close, "反向信号未成立时不得反手");
    }

    // ── 关掉信号闸：无条件反手 ────────────────────────────────────────────────
    {
        Config c2 = cfg; c2.reverse_needs_signal = false;
        State st;
        on_filled(st, Pos::Long, 100, 2.0, c2, false);
        auto v = step(st, c2, mk(94, 2.0, 130, 80));
        CHECK(v.action == Action::CloseReverseShort, "关闭信号闸后应无条件反手");
    }

    // ── 连续反手上限 + 冷却 ───────────────────────────────────────────────────
    {
        Config c3 = cfg; c3.reverse_needs_signal = false;   // 便于连续触发
        State st;
        on_filled(st, Pos::Long, 100, 2.0, c3, false);
        CHECK(st.consec_reverses == 0, "首次由信号入场，计数应为0");

        auto v = step(st, c3, mk(94));
        CHECK(v.action == Action::CloseReverseShort, "第1次反手");
        on_closed(st);
        on_filled(st, Pos::Short, 94, 2.0, c3, true);
        CHECK(st.consec_reverses == 1, "反手入场应累加计数");

        v = step(st, c3, mk(100));                   // 空头线 94+6=100，触线
        CHECK(v.action == Action::CloseReverseLong, "第2次反手");
        on_closed(st);
        on_filled(st, Pos::Long, 100, 2.0, c3, true);
        CHECK(st.consec_reverses == 2, "计数应为2");

        v = step(st, c3, mk(94));
        CHECK(v.action == Action::Close, "达到上限应停止反手");
        CHECK(st.cooldown_left == c3.cooldown_bars, "应进入冷却");
        CHECK(st.consec_reverses == 0, "冷却时应清零计数");
        on_closed(st);

        // 冷却期间即使有信号也不开仓，且必须按【新K线】递减
        auto sig = mk(115);
        CHECK(step(st, c3, sig).action == Action::Hold, "冷却中不得开仓");
        sig.new_bar = false;
        for (int i = 0; i < 50; ++i) step(st, c3, sig);
        CHECK(st.cooldown_left == c3.cooldown_bars, "同一根K线内多个tick不应消耗冷却");
        sig.new_bar = true;
        for (int i = 0; i < c3.cooldown_bars; ++i) step(st, c3, sig);
        CHECK(st.cooldown_left == 0, "跨过冷却根数后应解除");
        sig.new_bar = false;
        CHECK(step(st, c3, sig).action == Action::OpenLong, "冷却结束后应恢复开仓");
    }

    // ── 空头镜像 ──────────────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Short, 100, 2.0, cfg, false);
        CHECK(near(st.stop, 106.0), "空头初始线 = 成交价 + k*ATR");
        step(st, cfg, mk(80));
        CHECK(near(st.stop, 86.0), "空头创新低后线应下移");
        step(st, cfg, mk(90));
        CHECK(near(st.stop, 86.0), "空头反弹时线不得上移");
        auto v = step(st, cfg, mk(86, 2.0, 86, 50));
        CHECK(v.action == Action::Close && v.profitable_exit,
              "空头 86 < 成本 100 应为盈利出场且不反手");
    }

    // ── NaN 防御 ──────────────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 2.0, cfg, false);
        auto bad = mk(std::nan(""));
        auto v = step(st, cfg, bad);
        CHECK(v.action == Action::Hold, "NaN 价格必须被拦住");
        CHECK(near(st.stop, 94.0), "NaN 不得污染止损线");
        CHECK(near(st.peak, 100.0), "NaN 不得污染极值");
    }

    // ── 端到端：单边上涨行情应吃到整段 ────────────────────────────────────────
    {
        State st;
        // 100 涨到 200，ATR=2 → 线一路跟到 194，中途任何回调 <6 都不出场
        auto v = step(st, cfg, mk(110));
        CHECK(v.action == Action::OpenLong, "突破开多");
        on_filled(st, Pos::Long, 110, 2.0, cfg, false);
        for (int p = 111; p <= 200; ++p) {
            auto r = step(st, cfg, mk((double)p, 2.0, 210, 50));
            CHECK(r.action == Action::Hold, "上涨途中不应出场");
        }
        CHECK(near(st.stop, 194.0), "线应跟到 200-6=194");
        auto r = step(st, cfg, mk(194, 2.0, 210, 50));
        CHECK(r.action == Action::Close && r.profitable_exit,
              "回撤到线应盈利出场");
    }

    if (g_fail == 0) {
        std::printf("OK: 全部 SAR 决策测试通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
