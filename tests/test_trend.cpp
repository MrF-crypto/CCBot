// 趋势决策核心的单元测试——纯计算，不连网络。
// 三个策略（① 海龟 ② 抛物线SAR ③ 纯裸K）各一段，外加一段共享语义。
// 全部通过打印 OK 并 exit 0，否则打印失败断言并 exit 1。
#include "core/trend_decision.h"
#include "core/indicators.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace ccbot::trend;
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

// ① 海龟的输入：通道 [90,110]，ATR=2
// 注：on_filled 的第4参是【止损线】而非 ATR（k=3、ATR=2 ⇒ 距离 6）
static Inputs mk(double price, double atr = 2.0,
                 double up = 110, double dn = 90) {
    Inputs in;
    in.price = price; in.atr = atr;
    in.dc_ok = true;  in.dc_up = up; in.dc_dn = dn;
    return in;
}

// ─────────────────────────────────────────────────────────────────────────────
// ① 海龟（唐奇安突破 + Chandelier ATR）
// ─────────────────────────────────────────────────────────────────────────────
static void test_turtle() {
    Config cfg;   // 默认：Turtle / 20 / 14 / k=3 / 不反手 / 不限次数 / 不冷却
    CHECK(cfg.strategy == Strategy::Turtle, "默认策略应是海龟");
    CHECK(cfg.reverse == ReverseMode::None, "默认应【不反手】——震荡市的保命默认值");
    CHECK(cfg.max_consecutive_reverses == 0 && cfg.cooldown_bars == 0,
          "反手刹车默认关闭（不反手就不会有连续反手）");

    // ── 入场 ────────────────────────────────────────────────────────────────
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

    // ── 数据缺失不开新仓，但不撤已有保护 ─────────────────────────────────────
    {
        State st;
        auto no_atr = mk(115); no_atr.atr = 0;
        CHECK(step(st, cfg, no_atr).action == Action::Hold, "ATR 缺失不应开仓");

        auto no_dc = mk(115); no_dc.dc_ok = false;
        CHECK(step(st, cfg, no_dc).action == Action::Hold, "通道缺失不应开仓");

        State h;
        on_filled(h, Pos::Long, 100, 94, cfg, false);
        const double line0 = h.stop;                 // 100 - 3*2 = 94
        CHECK(near(line0, 94.0), "初始止损线应为 成交价-k*ATR");
        auto drop = mk(101); drop.atr = 0;
        auto v = step(h, cfg, drop);
        CHECK(near(h.stop, line0), "ATR 断流时止损线必须沿用，不得撤销");
        CHECK(v.action == Action::Hold, "未触线应继续持有");
    }

    // ── 棘轮：止损线只朝有利方向移动 ─────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 94, cfg, false);
        CHECK(near(st.stop, 94.0), "初始线 94");

        step(st, cfg, mk(120));                      // 极值 120 → 线 114
        CHECK(near(st.stop, 114.0), "新高后线应上移到 114");

        step(st, cfg, mk(115));                      // 回落，极值不变
        CHECK(near(st.stop, 114.0), "回落时线不得下移");

        step(st, cfg, mk(116, 10.0));                // 极值仍120 → 候选 90
        CHECK(near(st.stop, 114.0), "ATR 放大导致候选线更低时，棘轮必须挡住");
    }

    // ── 盈利出场：永不反手（即便开着反手）────────────────────────────────────
    {
        Config c = cfg; c.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        step(st, c, mk(120));                        // 线上移到 114（> 成本100）
        auto v = step(st, c, mk(114, 2.0, 130, 114));
        CHECK(v.action == Action::Close, "盈利出场必须只平不反手——力竭不等于反转");
        CHECK(v.profitable_exit, "出场价 114 > 成本 100，应判为盈利出场");
        CHECK(st.consec_reverses == 0, "盈利出场应清零连续反手计数");
    }

    // ── 亏损出场 · 不反手（默认）─────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 94, cfg, false);
        auto v = step(st, cfg, mk(94, 2.0, 130, 94));   // 触线，且同时下破通道
        CHECK(v.action == Action::Close,
              "ReverseMode::None：即便反向信号同时成立也只平不反手");
        CHECK(!v.profitable_exit, "94 < 100 应判为亏损出场");
    }

    // ── 亏损出场 · 立即反手 ─────────────────────────────────────────────────
    {
        Config c = cfg; c.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        // 反向信号【没有】成立（未下破通道），Immediate 依然反手
        auto v = step(st, c, mk(94, 2.0, 130, 80));
        CHECK(v.action == Action::CloseReverseShort,
              "ReverseMode::Immediate 是无条件的，不查反向信号");
    }

    // ── 反手时数据缺失 ⇒ 只平不反手（新仓没有止损线就不开）────────────────────
    {
        Config c = cfg; c.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        auto bad = mk(94, 2.0, 130, 80);
        bad.atr = 0;                       // 触线的同一拍 ATR 断流
        auto v = step(st, c, bad);
        CHECK(v.action == Action::Close, "反手需要止损线，拿不到就只平");
    }

    // ── 连续反手上限 + 冷却 ─────────────────────────────────────────────────
    {
        Config c3 = cfg;
        c3.reverse = ReverseMode::Immediate;
        c3.max_consecutive_reverses = 2;
        c3.cooldown_bars = 3;
        State st;
        on_filled(st, Pos::Long, 100, 94, c3, false);
        CHECK(st.consec_reverses == 0, "首次由信号入场，计数应为0");

        auto v = step(st, c3, mk(94));
        CHECK(v.action == Action::CloseReverseShort, "第1次反手");
        on_closed(st);
        on_filled(st, Pos::Short, 94, 100, c3, true);
        CHECK(st.consec_reverses == 1, "反手入场应累加计数");

        v = step(st, c3, mk(100));                   // 空头线 94+6=100，触线
        CHECK(v.action == Action::CloseReverseLong, "第2次反手");
        on_closed(st);
        on_filled(st, Pos::Long, 100, 94, c3, true);
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

    // ── 上限 0 = 不限：反手可以一直进行，永不进入冷却 ────────────────────────
    {
        Config c = cfg;
        c.reverse = ReverseMode::Immediate;
        c.max_consecutive_reverses = 0;
        c.cooldown_bars = 5;               // 就算配了冷却，够不着上限也用不上
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        for (int i = 0; i < 8; ++i) {
            auto v = step(st, c, mk(st.pos == Pos::Long ? 94 : 100));
            CHECK(v.action == (st.pos == Pos::Long ? Action::CloseReverseShort
                                                   : Action::CloseReverseLong),
                  "上限为 0 时应无限反手");
            const bool was_long = (st.pos == Pos::Long);
            on_closed(st);
            on_filled(st, was_long ? Pos::Short : Pos::Long,
                      was_long ? 94 : 100, was_long ? 100 : 94, c, true);
        }
        CHECK(st.cooldown_left == 0, "上限为 0 时永不进入冷却");
        CHECK(st.consec_reverses == 8, "连续反手计数应照常累加（只是不设闸）");
    }

    // ── 空头镜像 ────────────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Short, 100, 106, cfg, false);
        CHECK(near(st.stop, 106.0), "空头初始线 = 成交价 + k*ATR");
        step(st, cfg, mk(80));
        CHECK(near(st.stop, 86.0), "空头创新低后线应下移");
        step(st, cfg, mk(90));
        CHECK(near(st.stop, 86.0), "空头反弹时线不得上移");
        auto v = step(st, cfg, mk(86, 2.0, 86, 50));
        CHECK(v.action == Action::Close && v.profitable_exit,
              "空头 86 < 成本 100 应为盈利出场且不反手");
    }

    // ── NaN 防御 ────────────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 94, cfg, false);
        auto bad = mk(std::nan(""));
        auto v = step(st, cfg, bad);
        CHECK(v.action == Action::Hold, "NaN 价格必须被拦住");
        CHECK(near(st.stop, 94.0), "NaN 不得污染止损线");
        CHECK(near(st.peak, 100.0), "NaN 不得污染极值");
    }

    // ── 端到端：单边上涨行情应吃到整段 ───────────────────────────────────────
    {
        State st;
        auto v = step(st, cfg, mk(110));
        CHECK(v.action == Action::OpenLong, "突破开多");
        on_filled(st, Pos::Long, 110, 104, cfg, false);
        for (int p = 111; p <= 200; ++p) {
            auto r = step(st, cfg, mk((double)p, 2.0, 210, 50));
            CHECK(r.action == Action::Hold, "上涨途中不应出场");
        }
        CHECK(near(st.stop, 194.0), "线应跟到 200-6=194");
        auto r = step(st, cfg, mk(194, 2.0, 210, 50));
        CHECK(r.action == Action::Close && r.profitable_exit, "回撤到线应盈利出场");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 抛物线 SAR（Wilder）
// ─────────────────────────────────────────────────────────────────────────────
// newbar 默认 true：这个文件里的 PSAR 用例都是"每调用一次 = 过了一根K线"。
// ⚠ 实盘不是这样 —— 引擎 3 秒调用一次，同一根K线里 new_bar 只有第一拍是 true。
//   v5.9.10 之前这里默认 false 而 step 不看 new_bar，于是测试和实盘恰好用了两种
//   不同的节奏，"每拍都递推"的 bug 就藏在这个差别里。同一根K线多次调用的情形
//   见下面"同一根K线内多次调用"那组用例
static Inputs psar_in(double price, double ph, double pl,
                      double p2h = 0, double p2l = 0, bool newbar = true) {
    Inputs in;
    in.price = price;
    in.bar_ok = true;
    in.psar_prev_high = ph; in.psar_prev_low = pl;
    in.psar_prev2_high = p2h; in.psar_prev2_low = p2l;
    in.new_bar = newbar;
    return in;
}

static void test_psar() {
    Config cfg;
    cfg.strategy = Strategy::ParabolicSar;
    cfg.reverse  = ReverseMode::Immediate;   // PSAR 的定义
    // af_start=0.02, af_step=0.02, af_max=0.20（Wilder 原版）

    // ── 递推公式本身 ────────────────────────────────────────────────────────
    {
        // SAR=100, EP=120, AF=0.02 ⇒ next = 100 + 0.02*(120-100) = 100.4
        // 夹逼：前两根最低都在 100.4 之上 ⇒ 不生效
        Inputs in = psar_in(118, 125, 115, 124, 112);
        const double n = psar_next(Pos::Long, 100, 120, 0.02, in);
        CHECK(near(n, 100.4), "多头 SAR 递推：100 + 0.02×(120−100) = 100.4");
    }
    {
        // 空头镜像：SAR=100, EP=80, AF=0.02 ⇒ 100 + 0.02*(80-100) = 99.6
        Inputs in = psar_in(82, 85, 78, 88, 76);
        const double n = psar_next(Pos::Short, 100, 80, 0.02, in);
        CHECK(near(n, 99.6), "空头 SAR 递推：100 + 0.02×(80−100) = 99.6");
    }

    // ── Wilder 夹逼：SAR 不得落进【前两根】的价格范围 ────────────────────────
    // 漏掉这条规则的实现会在震荡里被提前打掉，而且从"看起来能跑"上完全看不出来
    {
        // 递推值 110，但前一根最低 105 ⇒ 必须夹到 105
        Inputs in = psar_in(120, 130, 105, 132, 108);
        const double n = psar_next(Pos::Long, 100, 600, 0.02, in);  // 100+0.02*500=110
        CHECK(near(n, 105.0), "多头：SAR 必须 ≤ 前一根最低价");
    }
    {
        // 前两根最低更低（102），取两者较小 ⇒ 102
        Inputs in = psar_in(120, 130, 105, 132, 102);
        const double n = psar_next(Pos::Long, 100, 600, 0.02, in);
        CHECK(near(n, 102.0), "多头：两根都要夹，取 min");
    }
    {
        Inputs in = psar_in(80, 95, 70, 98, 68);
        const double n = psar_next(Pos::Short, 100, -400, 0.02, in);  // 100+0.02*(-500)=90
        CHECK(near(n, 98.0), "空头：SAR 必须 ≥ 前两根最高价中的较大者");
    }
    {
        // 前两根的极值是 0（数据不足）时不参与夹逼，否则 min(x, 0) 会把 SAR
        // 拽到 0——多头止损线掉到 0 等于没有止损
        Inputs in = psar_in(120, 0, 0, 0, 0);
        in.bar_ok = true;
        const double n = psar_next(Pos::Long, 100, 120, 0.02, in);
        CHECK(near(n, 100.4), "前高前低为 0（缺数据）时不得参与夹逼");
    }

    // ── AF 只在【刷新 EP】时递增 ────────────────────────────────────────────
    // 这是 PSAR 最容易写错的一处：每根都加的话 AF 三四根就封顶，
    // SAR 立刻贴上价格，一点正常回调就出场
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        CHECK(near(st.af, cfg.af_start), "建仓时 AF = af_start");

        // 创新高 ⇒ AF 递增
        step(st, cfg, psar_in(110, 108, 94, 107, 93));
        CHECK(near(st.af, 0.04), "刷新 EP ⇒ AF += af_step");
        CHECK(near(st.peak, 110.0), "EP 应跟到 110");

        // 不创新高（价格回落）⇒ AF 不动
        const double af_before = st.af;
        step(st, cfg, psar_in(105, 108, 94, 107, 93));
        CHECK(near(st.af, af_before), "未刷新 EP ⇒ AF 必须【不变】");
        step(st, cfg, psar_in(108, 108, 94, 107, 93));
        CHECK(near(st.af, af_before), "仍未超过 110 ⇒ AF 依旧不变");

        // 再创新高 ⇒ 再涨一档
        step(st, cfg, psar_in(111, 108, 94, 107, 93));
        CHECK(near(st.af, 0.06), "再次刷新 EP ⇒ 再 += af_step");
    }

    // ── AF 封顶 ─────────────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        for (int i = 1; i <= 40; ++i)
            step(st, cfg, psar_in(100.0 + i, 90, 80, 89, 79));
        CHECK(near(st.af, cfg.af_max), "AF 必须封顶在 af_max，不能一直涨");
        CHECK(st.af <= cfg.af_max + 1e-12, "AF 绝不得超过上限");
    }

    // ── 同一根K线内多次调用：SAR 只递推一次（v5.9.10 修的实盘 bug）──────────
    // 引擎 3 秒调用一次 step。v5.9.10 之前每次都递推，一根K线里推上百次，止损线
    // 很快贴到前两根K线的低点。实测：SAR 起点 90，同一根K线调用 100 次后到 99，
    // 按设计应约为 90.3
    {
        State st;
        on_filled(st, Pos::Long, 100, 90, cfg, false);
        // 新K线第一拍：结算一次
        step(st, cfg, psar_in(105, 106, 101, 104, 99, /*newbar=*/true));
        const double after_first = st.stop;
        const double af_first    = st.af;
        CHECK(after_first < 92.0, "新K线第一拍只递推一次（远离夹逼上限 99）");
        // 同一根K线里再来 99 拍，价格还在涨
        for (int i = 0; i < 99; ++i)
            step(st, cfg, psar_in(105 + i * 0.01, 106, 101, 104, 99, /*newbar=*/false));
        CHECK(near(st.stop, after_first),
              "⚠ 同一根K线内不再递推 —— 否则 SAR 会一路贴到前两根K线低点");
        CHECK(near(st.af, af_first),
              "  AF 也不得在K线内随每个新高反复加速");
        CHECK(st.peak > 105.9, "  但 EP 要照常跟到K线内的新高");

        // 下一根K线开始：这根刷新过 EP ⇒ AF 加一档，SAR 再推一次
        step(st, cfg, psar_in(106, 106.98, 104, 106, 101, /*newbar=*/true));
        CHECK(near(st.af, af_first + cfg.af_step), "下一根开始时按「这根刷新过 EP」加速一次");
        CHECK(st.stop > after_first, "  SAR 往上推一次");
        CHECK(st.stop < 101.0, "  仍然受夹逼约束（前两根最低 101）");
    }
    {
        // 恢复后 psar_ep_mark 为 0（不落盘）：第一根K线不能因为"跟 0 比"白加一次 AF
        State st;
        on_filled(st, Pos::Long, 100, 90, cfg, false);
        st.psar_ep_mark = 0;
        const double af0 = st.af;
        step(st, cfg, psar_in(99, 106, 101, 104, 99, /*newbar=*/true));
        CHECK(near(st.af, af0), "从落盘恢复后的第一根K线不得白加一次 AF");
    }

    // ── 初始止损 = 前一根的反向极值 ──────────────────────────────────────────
    {
        Inputs in = psar_in(110, 108, 94, 107, 93);
        CHECK(near(initial_stop(Pos::Long, 110, cfg, in), 94.0),
              "多头初始 SAR = 前一根最低价");
        CHECK(near(initial_stop(Pos::Short, 90, cfg, in), 108.0),
              "空头初始 SAR = 前一根最高价");
    }

    // ── 触及即翻转，【盈利也翻】──────────────────────────────────────────────
    // 这是 PSAR 与另外两个策略最本质的差别：它没有"盈利出场不反手"这条规则，
    // 因为它的入场信号【只有】翻转这一个来源，不翻就永远空仓
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        // 让 SAR 爬到成本之上。前低必须跟着涨——夹逼规则封死了 SAR 的上限，
        // 前低钉在 98 的话 SAR 永远爬不过 98，这个场景根本造不出来
        for (int i = 0; i < 30; ++i) {
            const double p = 100.0 + i;
            step(st, cfg, psar_in(p, p, p - 5, p, p - 5));
        }
        CHECK(st.stop > st.entry_price, "SAR 应已爬过成本价（前置条件）");

        const double line = st.stop;
        auto v = step(st, cfg, psar_in(line, line + 10, line - 1, line + 10, line - 1));
        CHECK(v.profitable_exit, "出场价 > 成本，确实是盈利出场");
        CHECK(v.action == Action::CloseReverseShort,
              "PSAR 盈利触及同样翻转——这是 stop-and-reverse 的定义");
    }

    // ── 对照组：同样的局面，海龟必须只平不反 ─────────────────────────────────
    {
        Config t; t.reverse = ReverseMode::Immediate;   // 连反手都开着
        State st;
        on_filled(st, Pos::Long, 100, 94, t, false);
        step(st, t, mk(120));
        auto v = step(st, t, mk(114, 2.0, 130, 114));
        CHECK(v.action == Action::Close, "海龟盈利出场永不反手（与 PSAR 对照）");
    }

    // ── 亏损触及也翻转 ──────────────────────────────────────────────────────
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        auto v = step(st, cfg, psar_in(95, 99, 94, 99, 93));
        CHECK(!v.profitable_exit, "95 < 100 是亏损出场");
        CHECK(v.action == Action::CloseReverseShort, "亏损触及同样翻转");
    }

    // ── 反手后 AF 重新从 af_start 起算 ──────────────────────────────────────
    // 上一段趋势攒下的加速度属于上一段。带过来的话新仓一开仓就被贴得死紧，
    // 第一个正常回调就被打掉——连续翻转的放大器
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        for (int i = 1; i <= 10; ++i)
            step(st, cfg, psar_in(100.0 + i, 90, 80, 89, 79));
        CHECK(st.af > cfg.af_start, "前置条件：AF 已加速");
        on_closed(st);
        on_filled(st, Pos::Short, 95, 108, cfg, true);
        CHECK(near(st.af, cfg.af_start), "反手开出的新仓，AF 必须重置为 af_start");
    }

    // ── 连续反手上限对 PSAR 同样生效（震荡市唯一的刹车）──────────────────────
    {
        Config c = cfg;
        c.max_consecutive_reverses = 2;
        c.cooldown_bars = 3;
        State st;
        on_filled(st, Pos::Long, 100, 95, c, false);
        auto v = step(st, c, psar_in(95, 99, 94, 99, 93));
        CHECK(v.action == Action::CloseReverseShort, "第1次翻转");
        on_closed(st); on_filled(st, Pos::Short, 95, 101, c, true);
        v = step(st, c, psar_in(101, 102, 94, 102, 93));
        CHECK(v.action == Action::CloseReverseLong, "第2次翻转");
        on_closed(st); on_filled(st, Pos::Long, 101, 95, c, true);
        v = step(st, c, psar_in(95, 99, 94, 99, 93));
        CHECK(v.action == Action::Close, "达到上限：PSAR 也必须停下来");
        CHECK(st.cooldown_left == 3, "并进入冷却");
    }

    // ── 空仓入场：价格站上/跌破前一根极值 ────────────────────────────────────
    {
        State st;
        CHECK(step(st, cfg, psar_in(109, 110, 90)).action == Action::Hold,
              "未上破前一根最高价，不开仓");
        CHECK(step(st, cfg, psar_in(111, 110, 90)).action == Action::OpenLong,
              "上破前一根最高价 ⇒ 开多");
        State s2;
        CHECK(step(s2, cfg, psar_in(89, 110, 90)).action == Action::OpenShort,
              "跌破前一根最低价 ⇒ 开空");
    }

    // ── PSAR 不需要 ATR ─────────────────────────────────────────────────────
    {
        State st;
        Inputs in = psar_in(111, 110, 90);
        in.atr = 0;
        CHECK(step(st, cfg, in).action == Action::OpenLong,
              "PSAR 的止损是 SAR 值，没有 ATR 也应能开仓");
    }

    // ── 数据缺失不开仓，但不撤已有保护 ───────────────────────────────────────
    {
        State st;
        Inputs bad = psar_in(111, 110, 90);
        bad.bar_ok = false;
        CHECK(step(st, cfg, bad).action == Action::Hold, "K线缺失不开仓");

        State h;
        on_filled(h, Pos::Long, 100, 95, cfg, false);
        step(h, cfg, bad);
        CHECK(near(h.stop, 95.0), "数据缺失时 SAR 必须沿用，不得撤销");
    }

    // ── 棘轮：夹逼让候选线倒退时必须挡住 ─────────────────────────────────────
    // psar_next 的夹逼只看前两根，而前两根会随K线右移变化。旧的高点滚出窗口后
    // 夹逼值可能【变低】，此时候选 SAR 比现有 SAR 还低——多头的止损绝不能后退
    {
        State st;
        on_filled(st, Pos::Long, 100, 95, cfg, false);
        for (int i = 1; i <= 10; ++i)
            step(st, cfg, psar_in(100.0 + i, 120, 108, 120, 108));
        const double high_line = st.stop;
        CHECK(high_line > 95.0, "前置条件：SAR 已上移");
        // 下一根的前低骤降到 96（长下影），夹逼把候选拽下去
        step(st, cfg, psar_in(110, 120, 96, 120, 96));
        CHECK(near(st.stop, high_line), "夹逼导致候选倒退时，棘轮必须钉住");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 纯裸K
// ─────────────────────────────────────────────────────────────────────────────
// 顺手把前一根的收盘/高/低也填上（收盘价夹在高低之间 ⇒ 不构成入场信号）。
// 不填的话 data_ready 会失败，于是"亏损触线应反手"会被降级成"数据缺失，只平"——
// 测试看起来是过的，实际测的却是另一条分支
static Inputs bare_in(double price, double lo, double hi) {
    Inputs in;
    in.price = price;
    in.bar_ok = true;
    in.swing_low = lo;
    in.swing_high = hi;
    in.prev_close = price;
    in.prev_high  = hi;
    in.prev_low   = lo;
    return in;
}

static void test_bare_k() {
    // ══ 入场模式 A：立即顺势（实时价 vs 本根开盘价）══════════════════════════
    {
        Config c;
        c.strategy   = Strategy::BareK;
        c.bare_entry = BareEntry::Immediate;
        c.swing_bars = 3;

        auto imm = [](double price, double open, double lo, double hi) {
            Inputs in = bare_in(price, lo, hi);
            in.bar_open = open;
            return in;
        };

        {
            State st;
            CHECK(step(st, c, imm(101, 100, 94, 106)).action == Action::OpenLong,
                  "实时价 > 开盘价 ⇒ 做多");
        }
        {
            State st;
            CHECK(step(st, c, imm(99, 100, 94, 106)).action == Action::OpenShort,
                  "实时价 < 开盘价 ⇒ 做空");
        }
        {
            State st;
            CHECK(step(st, c, imm(100, 100, 94, 106)).action == Action::Hold,
                  "实时价 == 开盘价 ⇒ 无方向，等待");
        }
        {
            // 不等收盘：同一根K线内、new_bar=false 也照样开
            State st;
            Inputs in = imm(101, 100, 94, 106);
            in.new_bar = false;
            CHECK(step(st, c, in).action == Action::OpenLong,
                  "Immediate 模式不看 new_bar，盘中就开");
        }
        {
            // 缺开盘价 ⇒ 不开（data_ready 拦住）
            State st;
            CHECK(step(st, c, imm(101, 0, 94, 106)).action == Action::Hold,
                  "没有开盘价时 Immediate 模式无从判定，不得开仓");
        }

        // ── 护栏：每根K线最多开一次 ─────────────────────────────────────────
        {
            Config g = c; g.once_per_bar = true;
            State st;
            Inputs in = imm(101, 100, 94, 106);
            in.bar_open_ms = 1000;

            CHECK(step(st, g, in).action == Action::OpenLong, "本根第一次：允许");
            on_filled(st, Pos::Long, 101, 94, g, false, in.bar_open_ms);
            CHECK(st.last_entry_bar_ms == 1000, "应记下开仓所在K线");

            // 被止损打掉，同一根里想再开
            on_closed(st);
            CHECK(st.last_entry_bar_ms == 1000,
                  "on_closed 绝不能清掉这个字段，否则护栏等于没有");
            CHECK(step(st, g, in).action == Action::Hold,
                  "同一根K线里被打掉后不得重新入场");

            // 下一根：放行
            Inputs next = in; next.bar_open_ms = 2000;
            CHECK(step(st, g, next).action == Action::OpenLong, "跨到新K线后恢复");
        }
        {
            // 默认关：同一根里可以反复开
            Config g = c; g.once_per_bar = false;
            State st;
            Inputs in = imm(101, 100, 94, 106);
            in.bar_open_ms = 1000;
            step(st, g, in);
            on_filled(st, Pos::Long, 101, 94, g, false, in.bar_open_ms);
            on_closed(st);
            CHECK(step(st, g, in).action == Action::OpenLong,
                  "护栏默认关闭时同一根可以再开");
        }
        {
            // bar_open_ms 为 0（引擎没喂时间戳）时护栏必须【放行】而不是锁死。
            // 反过来的话一个缺字段会让 bot 静默地一单也不开
            Config g = c; g.once_per_bar = true;
            State st;
            Inputs in = imm(101, 100, 94, 106);
            in.bar_open_ms = 0;
            CHECK(step(st, g, in).action == Action::OpenLong,
                  "缺 bar_open_ms 时护栏应放行，不得静默锁死");
        }
    }

    // ══ 入场模式 B：等收盘突破前一根高/低 ═══════════════════════════════════
    {
        Config c;
        c.strategy   = Strategy::BareK;
        c.bare_entry = BareEntry::BreakPrevBar;
        c.swing_bars = 3;

        auto brk = [](double price, double pc, double ph, double pl,
                      double lo, double hi, bool newbar) {
            Inputs in = bare_in(price, lo, hi);
            in.prev_close = pc; in.prev_high = ph; in.prev_low = pl;
            in.new_bar = newbar;
            return in;
        };

        {
            State st;
            // 收盘 112 > 前一根最高 110 ⇒ 做多，但必须等跨新K线那一拍
            CHECK(step(st, c, brk(112, 112, 110, 100, 94, 114, false)).action
                      == Action::Hold,
                  "同一根K线内不得开仓（判定发生在收盘那一拍）");
            CHECK(step(st, c, brk(112, 112, 110, 100, 94, 114, true)).action
                      == Action::OpenLong,
                  "跨新K线 + 收盘价 > 上一根最高价 ⇒ 开多");
        }
        {
            State st;
            CHECK(step(st, c, brk(98, 98, 110, 100, 94, 114, true)).action
                      == Action::OpenShort,
                  "收盘价 < 上一根最低价 ⇒ 开空");
        }
        {
            // 收在区间内 ⇒ 等待。这正是它比旧"阳线就做多"严格的地方：
            // 下跌趋势里一根收在前一根区间内的阳线只是噪音
            State st;
            CHECK(step(st, c, brk(105, 105, 110, 100, 94, 114, true)).action
                      == Action::Hold,
                  "收盘价夹在前一根高低之间 ⇒ 无信号");
        }
        {
            // 恰好打平不算突破（用严格 > / <）
            State st;
            CHECK(step(st, c, brk(110, 110, 110, 100, 94, 114, true)).action
                      == Action::Hold,
                  "收盘价恰好等于前高不算突破");
        }
        {
            // Immediate 才看 bar_open；BreakPrevBar 缺 bar_open 也应能开
            State st;
            Inputs in = brk(112, 112, 110, 100, 94, 114, true);
            in.bar_open = 0;
            CHECK(step(st, c, in).action == Action::OpenLong,
                  "BreakPrevBar 模式不需要 bar_open");
        }
    }

    // ══ 止损：摆动极值 + 棘轮 ══════════════════════════════════════════════
    {
        Config bc;
        bc.strategy   = Strategy::BareK;
        bc.bare_entry = BareEntry::BreakPrevBar;
        bc.swing_bars = 3;
        bc.reverse    = ReverseMode::Immediate;

        // 初始止损是纯函数，不读也不写 State
        {
            Inputs in = bare_in(100, 94, 106);
            CHECK(near(initial_stop(Pos::Long, 100, bc, in), 94.0),
                  "多头初始止损 = 前 N 根最低价");
            CHECK(near(initial_stop(Pos::Short, 100, bc, in), 106.0),
                  "空头初始止损 = 前 N 根最高价");
        }

        {
            State st;
            on_filled(st, Pos::Long, 100, 94, bc, false);
            CHECK(near(st.stop, 94.0), "初始线 94");
            step(st, bc, bare_in(105, 96, 108));
            CHECK(near(st.stop, 96.0), "摆动低点上移 ⇒ 止损线跟上到 96");
            step(st, bc, bare_in(110, 99, 112));
            CHECK(near(st.stop, 99.0), "继续上移到 99");
        }

        // 棘轮：摆动低点下移时止损线不得跟着下移。
        // 两个真实例外——信号根的长下影、3 秒采样漏掉的插针
        {
            State st;
            on_filled(st, Pos::Long, 100, 94, bc, false);
            step(st, bc, bare_in(105, 96, 108));
            CHECK(near(st.stop, 96.0), "线已升到 96");
            step(st, bc, bare_in(104, 90, 108));
            CHECK(near(st.stop, 96.0),
                  "摆动低点下移时止损线必须钉住（棘轮），绝不放松到 90");
        }

        // 触线出场
        {
            State st;
            on_filled(st, Pos::Long, 100, 94, bc, false);
            step(st, bc, bare_in(110, 99, 112));                 // 线 99
            auto v = step(st, bc, bare_in(99, 99, 112));
            CHECK(v.action == Action::CloseReverseShort, "触线且亏损（99<100）⇒ 平多反手");
            CHECK(!v.profitable_exit, "99 < 成本 100 应判为亏损出场");
        }
        {
            State st;
            on_filled(st, Pos::Long, 100, 94, bc, false);
            step(st, bc, bare_in(120, 105, 122));                // 线 105 > 成本
            auto v = step(st, bc, bare_in(105, 105, 122));
            CHECK(v.action == Action::Close, "盈利出场只平不反手");
            CHECK(v.profitable_exit, "105 > 成本 100");
        }

        // 数据缺失不开仓，但已有仓位的线不撤
        {
            State st;
            Inputs no_bar = bare_in(100, 94, 106);
            no_bar.prev_close = 105; no_bar.prev_high = 100; no_bar.prev_low = 90;
            no_bar.new_bar = true;
            no_bar.bar_ok = false;
            CHECK(step(st, bc, no_bar).action == Action::Hold, "K线数据缺失不开仓");

            State h;
            on_filled(h, Pos::Long, 100, 94, bc, false);
            auto v = step(h, bc, no_bar);
            CHECK(near(h.stop, 94.0), "数据缺失时止损线必须沿用，不得撤销");
            CHECK(v.action == Action::Hold, "未触线应继续持有");
        }

        // 裸K 不需要 ATR
        {
            State st;
            Inputs in = bare_in(112, 94, 114);
            in.prev_close = 112; in.prev_high = 110; in.prev_low = 100;
            in.new_bar = true;
            in.atr = 0;
            CHECK(step(st, bc, in).action == Action::OpenLong,
                  "裸K 的止损是摆动极值，没有 ATR 也应能开仓");
        }

        // 空头镜像
        {
            State st;
            on_filled(st, Pos::Short, 100, 106, bc, false);
            CHECK(near(st.stop, 106.0), "空头初始线 = 前 N 根最高价");
            step(st, bc, bare_in(95, 88, 103));
            CHECK(near(st.stop, 103.0), "摆动高点下移 ⇒ 空头线跟着下移到 103");
            step(st, bc, bare_in(96, 88, 108));
            CHECK(near(st.stop, 103.0), "摆动高点上移时空头线必须钉住");
        }
    }

    // ── ReverseMode::None：平掉后回到正常入场流程，【方向不限】────────────────
    // 用户明确要的语义：不是"必须等反向信号"，而是回到和首次入场完全一样的
    // 流程——同向信号先来就同向再进一次
    {
        Config c;
        c.strategy   = Strategy::BareK;
        c.bare_entry = BareEntry::BreakPrevBar;
        c.swing_bars = 3;
        c.reverse    = ReverseMode::None;

        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        auto v = step(st, c, bare_in(94, 94, 106));
        CHECK(v.action == Action::Close, "不反手：只平");
        on_closed(st);

        // 下一根又出现【多头】信号 ⇒ 照常再开多，不需要先反向
        Inputs again = bare_in(112, 94, 114);
        again.prev_close = 112; again.prev_high = 110; again.prev_low = 100;
        again.new_bar = true;
        CHECK(step(st, c, again).action == Action::OpenLong,
              "平掉后回到正常入场流程，同向信号照样开");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ① 海龟 · 横盘突破过滤（箱体 ≤ x ATR、突破量比 ≥ y）
// ─────────────────────────────────────────────────────────────────────────────
static Inputs sq_in(double price, double box_atr, double vol_ratio) {
    Inputs in = mk(price);          // 通道 [90, 110]，ATR 2
    in.box_atr = box_atr;
    in.vol_ratio = vol_ratio;
    return in;
}

static void test_squeeze_filter() {
    // 默认关：读数全是 0 也照常突破入场——老配置的行为不变
    {
        Config c; State st;
        CHECK(!squeeze_filter_on(c), "默认两项都是 0，过滤关闭");
        CHECK(step(st, c, sq_in(111, 0, 0)).action == Action::OpenLong,
              "过滤关闭时读数缺失不影响开仓");
    }

    Config c;
    c.squeeze_box_atr = 4.0;
    c.breakout_vol_ratio = 1.5;

    {
        State st;
        CHECK(step(st, c, sq_in(111, 3.5, 2.0)).action == Action::OpenLong,
              "横盘(3.5≤4) + 放量(2.0≥1.5) ⇒ 开多");
    }
    {
        State st;
        CHECK(step(st, c, sq_in(111, 4.0, 1.5)).action == Action::OpenLong,
              "两项都恰好等于阈值 ⇒ 放行（≤ / ≥）");
    }
    {
        State st;
        const auto v = step(st, c, sq_in(111, 5.0, 3.0));
        CHECK(v.action == Action::Hold && v.filtered, "箱体 5ATR > 4 ⇒ 拦，标记 filtered");
        CHECK(std::string(v.reason).find("箱体过宽") != std::string::npos, "原因写明箱体过宽");
    }
    {
        // 同一根K线：先突破没量被拦，量攒够之后入场
        State st;
        const auto v1 = step(st, c, sq_in(111, 3.0, 1.2));
        CHECK(v1.action == Action::Hold && v1.filtered, "量比 1.2 < 1.5 ⇒ 拦");
        CHECK(std::string(v1.reason).find("等放量") != std::string::npos, "原因写明等放量");
        CHECK(step(st, c, sq_in(112, 3.0, 1.6)).action == Action::OpenLong,
              "同一根里量比涨到 1.6 ⇒ 入场");
    }
    {
        State st;
        CHECK(step(st, c, sq_in(111, 0, 2.0)).action == Action::Hold,
              "开了箱体过滤但读数缺失 ⇒ 拦（宁拦勿放）");
        CHECK(step(st, c, sq_in(111, 3.0, 0)).action == Action::Hold,
              "开了量比过滤但读数缺失 ⇒ 拦");
    }
    {
        // 多空对称
        State st;
        CHECK(step(st, c, sq_in(89, 3.0, 2.0)).action == Action::OpenShort, "下破 + 横盘 + 放量 ⇒ 开空");
        State st2;
        CHECK(step(st2, c, sq_in(89, 3.0, 1.0)).action == Action::Hold, "下破但没放量 ⇒ 拦");
    }
    {
        // 没突破就不是"被过滤"
        State st;
        const auto v = step(st, c, sq_in(100, 9.0, 0.1));
        CHECK(v.action == Action::Hold && !v.filtered, "通道内无信号，不算被过滤");
    }
    {
        // 只开量比：箱体读数缺失不影响
        Config cv; cv.breakout_vol_ratio = 1.5;
        State st;
        CHECK(step(st, cv, sq_in(111, 0, 2.0)).action == Action::OpenLong,
              "只开量比时不看箱体");
        Config cb; cb.squeeze_box_atr = 4.0;
        State st2;
        CHECK(step(st2, cb, sq_in(111, 3.0, 0)).action == Action::OpenLong,
              "只开箱体时不看量比");
    }
    {
        // 亏损止损后的立即反手不经过过滤：它不是突破信号
        Config cr = c;
        cr.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 100, 94, cr, false);
        CHECK(step(st, cr, sq_in(93, 9.0, 0)).action == Action::CloseReverseShort,
              "立即反手不受横盘/量比过滤影响");
    }
    {
        // 效率比：箱体宽时的例外（趋势延续）
        Config ce = c;
        ce.trend_er_min = 0.4;
        auto with_er = [](Inputs in, double er) { in.er_ok = true; in.er = er; return in; };
        State s1;
        CHECK(step(s1, ce, with_er(sq_in(111, 6.0, 1.7), 0.62)).action == Action::OpenLong,
              "箱体 6ATR 但效率比 +0.62（一路涨上来）⇒ 当趋势延续放行");
        State s2;
        const auto v2 = step(s2, ce, with_er(sq_in(111, 6.0, 1.7), 0.25));
        CHECK(v2.action == Action::Hold && v2.filtered && v2.filtered_long,
              "箱体宽、效率比 0.25（来回扫）⇒ 拦");
        CHECK(std::string(v2.reason).find("来回震荡") != std::string::npos, "  原因写明来回震荡");
        State s3;
        CHECK(step(s3, ce, with_er(sq_in(111, 6.0, 1.7), -0.62)).action == Action::Hold,
              "净跌的窗口里向上突破 ⇒ 方向不一致，拦");
        State s4;
        CHECK(step(s4, ce, with_er(sq_in(89, 6.0, 1.7), -0.62)).action == Action::OpenShort,
              "净跌 + 向下突破 ⇒ 放行开空");
        State s5;
        CHECK(step(s5, ce, with_er(sq_in(111, 6.0, 1.7), 0.40)).action == Action::OpenLong,
              "效率比恰好等于阈值 ⇒ 放行");
        State s6;
        CHECK(step(s6, ce, sq_in(111, 6.0, 1.7)).action == Action::Hold,
              "效率比读数缺失 ⇒ 没有例外，按箱体拦");
        State s7;
        CHECK(step(s7, ce, with_er(sq_in(111, 6.0, 1.0), 0.8)).action == Action::Hold,
              "效率比放行了箱体，量比照样要过");
        State s8;
        CHECK(step(s8, c, with_er(sq_in(111, 6.0, 1.7), 0.9)).action == Action::Hold,
              "效率比阈值为 0（关）⇒ 箱体宽一律拦（原行为）");
        Config cnobox = ce; cnobox.squeeze_box_atr = 0;
        State s9;
        CHECK(step(s9, cnobox, with_er(sq_in(111, 9.0, 1.7), 0.0)).action == Action::OpenLong,
              "箱体关时效率比不起作用");
    }
    {
        // ③ 裸K「等收盘突破」的信号K量比
        Config cb;
        cb.strategy = Strategy::BareK;
        cb.bare_entry = BareEntry::BreakPrevBar;
        cb.bare_vol_ratio = 1.8;
        auto brk = [](double vr) {
            Inputs in = bare_in(112, 94, 114);
            in.prev_close = 112; in.prev_high = 110; in.prev_low = 100;
            in.new_bar = true;
            in.signal_vol_ratio = vr;
            return in;
        };
        State b1;
        CHECK(step(b1, cb, brk(2.0)).action == Action::OpenLong, "裸K：收盘突破 + 量比 2.0 ⇒ 开多");
        State b2;
        const auto vb = step(b2, cb, brk(1.1));
        CHECK(vb.action == Action::Hold && vb.filtered, "裸K：量比 1.1 < 1.8 ⇒ 拦");
        CHECK(std::string(vb.reason).find("未放量") != std::string::npos, "  原因写明未放量");
        State b3;
        CHECK(step(b3, cb, brk(1.8)).action == Action::OpenLong, "裸K：恰好等于阈值 ⇒ 放行");
        State b4;
        CHECK(step(b4, cb, brk(0)).action == Action::Hold, "裸K：量比拿不到 ⇒ 拦");

        Config ci = cb;
        ci.bare_entry = BareEntry::Immediate;
        State b5;
        Inputs im = bare_in(105, 94, 114);
        im.bar_open = 100;
        CHECK(!bare_vol_filter_on(ci), "立即顺势下量比过滤不生效");
        CHECK(step(b5, ci, im).action == Action::OpenLong, "立即顺势：量比读数为 0 也照常开");

        Config cr = cb;
        cr.reverse = ReverseMode::Immediate;
        State b6;
        on_filled(b6, Pos::Long, 110, 100, cr, false);
        Inputs hit = bare_in(99, 100, 114);
        CHECK(step(b6, cr, hit).action == Action::CloseReverseShort,
              "裸K：止损后的立即反手不受量比过滤影响");

        Config cz = cb; cz.bare_vol_ratio = 0;
        State b7;
        CHECK(step(b7, cz, brk(0)).action == Action::OpenLong, "裸K：量比 0（关）⇒ 原行为");
    }
    {
        // 其它策略即便字段里有值也不生效
        Config ck = c;
        ck.strategy = Strategy::BareK;
        CHECK(!squeeze_filter_on(ck), "过滤只属于海龟");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 方向选项（三个策略共用）：仅多 / 仅空 / 多空
// ─────────────────────────────────────────────────────────────────────────────
static void test_direction() {
    {
        Config c; State st;
        CHECK(c.direction == Direction::Both, "默认多空");
        CHECK(step(st, c, mk(89)).action == Action::OpenShort, "多空：下破开空");
    }
    {
        Config c; c.direction = Direction::LongOnly;
        State st;
        CHECK(step(st, c, mk(89)).action == Action::Hold, "仅多：下破不开空");
        CHECK(step(st, c, mk(111)).action == Action::OpenLong, "仅多：上破照常开多");
    }
    {
        Config c; c.direction = Direction::ShortOnly;
        State st;
        CHECK(step(st, c, mk(111)).action == Action::Hold, "仅空：上破不开多");
        CHECK(step(st, c, mk(89)).action == Action::OpenShort, "仅空：下破照常开空");
    }
    {
        // 立即反手到不允许的方向 ⇒ 只平
        Config c; c.direction = Direction::LongOnly; c.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);
        const auto v = step(st, c, mk(93));
        CHECK(v.action == Action::Close, "仅多：亏损止损要反手做空 ⇒ 只平");
        CHECK(std::string(v.reason).find("仅做多") != std::string::npos, "  原因写明仅做多");

        Config cs = c; cs.direction = Direction::ShortOnly;
        State st2;
        on_filled(st2, Pos::Short, 100, 106, cs, false);
        CHECK(step(st2, cs, mk(107)).action == Action::Close, "仅空：亏损止损要反手做多 ⇒ 只平");

        Config cb = c; cb.direction = Direction::Both;
        State st3;
        on_filled(st3, Pos::Long, 100, 94, cb, false);
        CHECK(step(st3, cb, mk(93)).action == Action::CloseReverseShort, "多空：照常反手");
    }
    {
        // PSAR 的 stop-and-reverse 同样受方向约束
        Config c;
        c.strategy = Strategy::ParabolicSar;
        c.reverse = ReverseMode::Immediate;
        c.direction = Direction::LongOnly;
        State st;
        on_filled(st, Pos::Long, 100, 95, c, false);
        CHECK(step(st, c, psar_in(94, 101, 96)).action == Action::Close,
              "PSAR 仅多：触线不翻空，只平");
        State st2;
        CHECK(step(st2, c, psar_in(90, 101, 96)).action == Action::Hold,
              "PSAR 仅多：空仓时跌破前低不开空");
    }
    {
        // 改成仅多时手里恰好有空单：照常用止损线管，不强平
        Config c; c.direction = Direction::LongOnly;
        State st;
        on_filled(st, Pos::Short, 100, 106, c, false);
        CHECK(step(st, c, mk(99)).action == Action::Hold, "仅多：已有空单照常持有");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 裸K：震荡态（效率比 + 箱体突破）与信号K力度
// ─────────────────────────────────────────────────────────────────────────────
static void test_bare_regime() {
    // 等收盘突破的基准输入：bar0 收盘 112 > 前一根高 110（平常的多头信号），
    // 箱体 [95, 115]，ATR 2
    auto base = [](double er, bool er_ok = true) {
        Inputs in = bare_in(112, 94, 114);
        in.prev_close = 112; in.prev_high = 110; in.prev_low = 100;
        in.prev_open = 110;
        in.new_bar = true;
        in.bar_atr = 2.0;
        in.er_ok = er_ok; in.er = er;
        in.range_hi = 115; in.range_lo = 95;
        in.range_now_hi = 115; in.range_now_lo = 95;
        return in;
    };
    Config c;
    c.strategy = Strategy::BareK;
    c.bare_entry = BareEntry::BreakPrevBar;
    c.chop_er_max = 0.3;

    {
        State st;
        const auto v = step(st, c, base(0.1));
        CHECK(v.action == Action::Hold, "震荡态：突破前一根不算");
        CHECK(std::string(v.reason).find("等箱体突破") != std::string::npos, "  原因写明等箱体突破");
        CHECK(!v.filtered, "  不算被过滤（是没信号）");
    }
    {
        State st;
        Inputs in = base(0.1);
        in.range_hi = 111;   // 收盘 112 站上箱体
        CHECK(step(st, c, in).action == Action::OpenLong, "震荡态：收盘站上箱体 ⇒ 开多");
    }
    {
        State st;
        Inputs in = base(-0.1);
        in.prev_close = 94; in.prev_low = 100;   // 收盘跌破箱体下沿 95
        in.prev_open = 97;
        CHECK(step(st, c, in).action == Action::OpenShort, "震荡态：收盘跌破箱体 ⇒ 开空（|ER| 判震荡）");
    }
    {
        State st;
        CHECK(step(st, c, base(0.5)).action == Action::OpenLong, "趋势态（ER 0.5）：平常信号照常");
        State st2;
        CHECK(step(st2, c, base(-0.5)).action == Action::OpenLong,
              "ER −0.5 也是趋势态（按绝对值），平常信号照常");
    }
    {
        State st;
        CHECK(step(st, c, base(0, false)).action == Action::Hold, "效率比缺失 ⇒ 按震荡处理");
    }
    {
        Config cz = c; cz.chop_er_max = 0;
        State st;
        CHECK(step(st, cz, base(0.05)).action == Action::OpenLong, "震荡过滤关 ⇒ 原行为");
    }
    {
        // 震荡中止损：只平不反手；趋势中照常反手
        Config cr = c; cr.reverse = ReverseMode::Immediate;
        State st;
        on_filled(st, Pos::Long, 110, 100, cr, false);
        Inputs hit = base(0.1);
        hit.price = 99; hit.swing_low = 100; hit.new_bar = false;
        const auto v = step(st, cr, hit);
        CHECK(v.action == Action::Close, "震荡中亏损止损 ⇒ 只平不反手");
        CHECK(std::string(v.reason).find("震荡") != std::string::npos, "  原因写明震荡中");
        State st2;
        on_filled(st2, Pos::Long, 110, 100, cr, false);
        Inputs hit2 = base(0.6);
        hit2.price = 99; hit2.swing_low = 100; hit2.new_bar = false;
        CHECK(step(st2, cr, hit2).action == Action::CloseReverseShort, "趋势中亏损止损 ⇒ 照常反手");
    }
    {
        // 立即顺势：震荡态看实时价对最近 20 根
        Config ci = c; ci.bare_entry = BareEntry::Immediate;
        Inputs in = base(0.1);
        in.new_bar = false; in.bar_open = 100; in.price = 105;
        State st;
        CHECK(step(st, ci, in).action == Action::Hold, "立即顺势·震荡：在箱体内不开");
        in.price = 116;
        State st2;
        CHECK(step(st2, ci, in).action == Action::OpenLong, "立即顺势·震荡：实时价冲出箱体 ⇒ 开多");
    }
    {
        // 震荡态的箱体突破照样要过量比
        Config cv = c; cv.bare_vol_ratio = 1.8;
        Inputs in = base(0.1);
        in.range_hi = 111;
        in.signal_vol_ratio = 1.0;
        State st;
        const auto v = step(st, cv, in);
        CHECK(v.action == Action::Hold && v.filtered, "震荡态箱体突破没放量 ⇒ 拦");
    }

    // ── 信号K力度 ──
    Config cb;
    cb.strategy = Strategy::BareK;
    cb.bare_entry = BareEntry::BreakPrevBar;
    cb.signal_body_atr = 0.6;
    {
        State st;
        Inputs in = base(0.5);
        in.prev_open = 111;   // 实体 1 = 0.5 ATR
        const auto v = step(st, cb, in);
        CHECK(v.action == Action::Hold && v.filtered, "实体 0.5ATR < 0.6 ⇒ 拦");
        CHECK(std::string(v.reason).find("太弱") != std::string::npos, "  原因写明信号K太弱");
    }
    {
        State st;
        Inputs in = base(0.5);
        in.prev_open = 110;   // 实体 2 = 1.0 ATR
        CHECK(step(st, cb, in).action == Action::OpenLong, "实体 1.0ATR ⇒ 放行");
    }
    {
        State st;
        Inputs in = base(0.5);
        in.prev_open = 114;   // 阴线收在前高之上：方向相反
        CHECK(step(st, cb, in).action == Action::Hold, "阴线向上突破 ⇒ 方向不一致，拦");
    }
    {
        State st;
        Inputs in = base(0.5);
        in.bar_atr = 0;
        CHECK(step(st, cb, in).action == Action::Hold, "ATR 缺失 ⇒ 拦");
    }
    {
        // 立即顺势：死区，不算被过滤
        Config ci = cb; ci.bare_entry = BareEntry::Immediate; ci.signal_body_atr = 0.5;
        Inputs in = base(0.5);
        in.new_bar = false; in.bar_open = 100; in.price = 100.8;   // 死区 = 0.5×2 = 1
        State st;
        const auto v = step(st, ci, in);
        CHECK(v.action == Action::Hold && !v.filtered, "立即顺势：离开盘价 0.8 < 死区 1 ⇒ 没信号（不记日志）");
        in.price = 101.2;
        State st2;
        CHECK(step(st2, ci, in).action == Action::OpenLong, "立即顺势：离开盘价 1.2 > 死区 ⇒ 开多");
        in.price = 98.8;
        State st3;
        CHECK(step(st3, ci, in).action == Action::OpenShort, "立即顺势：向下离开死区 ⇒ 开空");
        in.price = 105; in.bar_atr = 0;
        State st4;
        CHECK(step(st4, ci, in).action == Action::Hold, "立即顺势：开了死区但 ATR 缺失 ⇒ 不开");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ① 海龟出场优化：锁定 ATR / 盈利后加速收紧
// ─────────────────────────────────────────────────────────────────────────────
static void test_exit_opt() {
    auto open_long = [](const Config& c) {
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false, 0, /*entry_atr*/ 2.0);
        return st;
    };
    {
        Config c;   // 都关：ATR 放大到 5，止损按 5 算
        State st = open_long(c);
        CHECK(near(st.entry_atr, 2.0), "on_filled 记下开仓时 ATR");
        step(st, c, mk(110, 5.0));
        CHECK(near(st.stop, 95.0), "普通 Chandelier：110 − 3×5 = 95（ATR 放大，止损变松）");
    }
    {
        Config c; c.lock_atr = true;
        State st = open_long(c);
        step(st, c, mk(110, 5.0));
        CHECK(near(st.stop, 104.0), "锁定 ATR：110 − 3×min(2,5) = 104");
        step(st, c, mk(111, 1.0));
        CHECK(near(st.stop, 108.0), "锁定 ATR：当前 ATR 更小时用当前的（111 − 3×1）");
    }
    {
        Config c; c.accel_exit = true;
        State st = open_long(c);
        step(st, c, mk(110, 2.0));
        CHECK(near(st.stop, 104.0), "浮盈 5ATR < 6：k 仍为 3（110 − 6）");
        step(st, c, mk(112, 2.0));
        CHECK(near(turtle_k(st, c), 2.0) && near(st.stop, 108.0), "浮盈 6ATR：k=2（112 − 4）");
        step(st, c, mk(120, 2.0));
        CHECK(near(turtle_k(st, c), 1.5) && near(st.stop, 117.0), "浮盈 10ATR：k=1.5（120 − 3）");
        step(st, c, mk(118, 2.0));
        CHECK(near(turtle_k(st, c), 1.5) && near(st.stop, 117.0),
              "回落时 k 不回升（按极值算），止损棘轮不退");
    }
    {
        Config c; c.accel_exit = true; c.atr_mult = 1.0;
        State st = open_long(c);
        step(st, c, mk(112, 2.0));
        CHECK(near(turtle_k(st, c), 1.0), "用户 k 本来就比档位小：不放大");
    }
    {
        Config c; c.lock_atr = true; c.accel_exit = true;
        State st;
        on_filled(st, Pos::Long, 100, 94, c, false);   // 没有开仓 ATR（旧存档恢复）
        step(st, c, mk(112, 5.0));
        CHECK(near(st.stop, 97.0), "开仓 ATR 缺失：两项都退回普通 Chandelier（112 − 15）");
    }
    {
        // 空头对称
        Config c; c.accel_exit = true; c.lock_atr = true;
        State st;
        on_filled(st, Pos::Short, 100, 106, c, false, 0, 2.0);
        step(st, c, mk(88, 5.0, 110, 90));
        CHECK(near(turtle_mfe_atr(st), 6.0) && near(st.stop, 92.0),
              "空头：浮盈 6ATR，k=2，锁定 ATR=2 ⇒ 88 + 4 = 92");
    }
    {
        Config c; State st = open_long(c);
        on_closed(st);
        CHECK(st.entry_atr == 0, "平仓清掉开仓 ATR");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ① 海龟假突破提前出场：开仓那根收完后，有K线收盘跌回开仓时的通道边沿以内就走
// ─────────────────────────────────────────────────────────────────────────────
static void test_fail_exit() {
    const int64_t H = 3600000, t0 = 100 * H;   // 开仓那根K线的开盘时间
    auto in_at = [&](double price, int64_t bar_open, double prev_close) {
        Inputs in = mk(price, 2.0, 112, 90);    // 开仓后通道上沿已被突破的那几根抬到 112
        in.bar_open_ms = bar_open;
        in.prev_close  = prev_close;
        return in;
    };
    auto open_long = [&](const Config& c) {
        State st;
        on_filled(st, Pos::Long, 111, 105, c, false, t0, 2.0, /*entry_edge*/ 110);
        return st;
    };
    {
        Config c; c.fail_exit = true;
        State st = open_long(c);
        CHECK(near(st.entry_edge, 110), "on_filled 记下被突破的上沿 110");
        auto v = step(st, c, in_at(109.5, t0, 109.0));
        CHECK(v.action == Action::Hold, "开仓那根还没收完：不判（prev_close 是开仓前那根的）");
        v = step(st, c, in_at(110.6, t0 + H, 110.5));
        CHECK(v.action == Action::Hold, "开仓那根收在 110.5（> 110）：没跌回，继续拿");
        v = step(st, c, in_at(109.8, t0 + 2 * H, 109.7));
        CHECK(v.action == Action::Close && std::string(v.reason).find("假突破") != std::string::npos,
              "下一根收在 109.7（< 开仓时的上沿 110）：直接走，不等 105 的止损线");
        CHECK(near(st.entry_edge, 110), "  判定用的是开仓时的上沿，不是当前抬高到 112 的通道");
        on_closed(st);
        CHECK(st.entry_edge == 0, "平仓清掉入场边沿");
    }
    {
        Config c;   // 没开 fail_exit：跌回通道照样拿着，等止损线
        State st = open_long(c);
        auto v = step(st, c, in_at(109.8, t0 + 2 * H, 109.7));
        CHECK(v.action == Action::Hold, "没开假突破出场：跌回通道内照样拿着");
    }
    {
        Config c; c.fail_exit_short = true;
        State st;
        on_filled(st, Pos::Short, 89, 95, c, false, t0, 2.0, /*entry_edge*/ 90);
        auto v = step(st, c, in_at(90.4, t0 + H, 90.3));
        CHECK(v.action == Action::Close, "做空对称（勾了空单）：收盘 90.3 回到下沿 90 之上 ⇒ 走");
    }
    {
        Config c; c.fail_exit = true;   // 只勾多单
        State st;
        on_filled(st, Pos::Short, 89, 95, c, false, t0, 2.0, /*entry_edge*/ 90);
        auto v = step(st, c, in_at(90.4, t0 + H, 90.3));
        CHECK(v.action == Action::Hold, "只勾多单：空单反弹回通道照样拿着，交给止损线");
    }
    {
        Config c; c.fail_exit_short = true;   // 只勾空单
        State st = open_long(c);
        auto v = step(st, c, in_at(109.8, t0 + 2 * H, 109.7));
        CHECK(v.action == Action::Hold, "只勾空单：多单跌回通道照样拿着");
    }
    {
        Config c; c.fail_exit = true;
        State st;
        on_filled(st, Pos::Long, 111, 105, c, false, t0, 2.0, /*entry_edge*/ 0);
        auto v = step(st, c, in_at(109.8, t0 + 2 * H, 109.7));
        CHECK(v.action == Action::Hold, "不知道开仓时的边沿（老仓位）：不判，交给止损线");
    }
    {
        Config c; c.fail_exit = true;
        State st = open_long(c);
        st.fail_bar_ms = t0 + H;                 // 开仓那根的最后 1/4 才开仓：从下一根开始判
        auto v = step(st, c, in_at(109.8, t0 + H, 109.7));
        CHECK(v.action == Action::Hold, "fail_bar_ms 是下一根：开仓那根收盘跌回也不判");
        v = step(st, c, in_at(109.6, t0 + 2 * H, 109.5));
        CHECK(v.action == Action::Close, "  下一根收完还在通道内：平");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 收益率追踪止盈：最高收益率到过激活线，从最高点回撤 n 个点就平（收益率带杠杆，同主表）
// ─────────────────────────────────────────────────────────────────────────────
static void test_roi_trail() {
    // 3 倍杠杆、开多 @100：收益率 = (价 − 100) ÷ 价 × 300
    //   价 105.27 ⇒ ≈ +15.03%；价 103.45 ⇒ ≈ +10.0%
    CHECK(near(roi_pct(Pos::Long, 100, 105, 3), 5.0 / 105 * 300), "收益率：按现价算保证金（同主表）");
    CHECK(near(roi_pct(Pos::Short, 100, 95, 3), 5.0 / 95 * 300), "  做空对称");
    CHECK(roi_pct(Pos::Long, 100, 105, 0) == 0, "  杠杆不知道（0）⇒ 0，不触发");

    Config c;
    c.roi_trail = true;  c.roi_trail_start = 10;  c.roi_trail_back = 5;
    auto in_lev = [](double price) { Inputs in = mk(price, 2.0, 110, 90); in.leverage = 3; return in; };
    {
        State st;
        on_filled(st, Pos::Long, 100, 90, c, false);
        CHECK(step(st, c, in_lev(104.0)).action == Action::Hold, "+11.5%：激活但没回撤，拿着");
        CHECK(step(st, c, in_lev(105.27)).action == Action::Hold, "+15%：新高，拿着");
        CHECK(step(st, c, in_lev(103.6)).action == Action::Hold, "回到 +10.3%：回撤 4.7 个点 < 5，拿着");
        const auto v = step(st, c, in_lev(103.4));
        CHECK(v.action == Action::Close && std::string(v.reason).find("追踪止盈") != std::string::npos &&
                  v.profitable_exit,
              "回到 +9.9%：从最高 +15% 回撤超过 5 个点 ⇒ 平（盈利出场）");
    }
    {
        State st;
        on_filled(st, Pos::Long, 100, 90, c, false);
        step(st, c, in_lev(103.0));   // 最高 +8.7%，没到激活线
        CHECK(step(st, c, in_lev(100.5)).action == Action::Hold, "最高只到 +8.7%（没激活）：回落也不平，交给止损线");
    }
    {
        State st;
        on_filled(st, Pos::Short, 100, 110, c, false);
        step(st, c, in_lev(95.0));    // 做空 +15.8%
        CHECK(step(st, c, in_lev(96.9)).action == Action::Close, "做空：最高 +15.8% 回到 +9.6% ⇒ 平");
    }
    {
        Config off;   // 没勾：同样的走势照样拿着
        State st;
        on_filled(st, Pos::Long, 100, 90, off, false);
        step(st, off, in_lev(105.27));
        CHECK(step(st, off, in_lev(103.4)).action == Action::Hold, "没勾追踪止盈：不平");
    }
}

int main() {
    test_turtle();
    test_exit_opt();
    test_fail_exit();
    test_roi_trail();
    test_bare_regime();
    test_squeeze_filter();
    test_direction();
    test_psar();
    test_bare_k();

    if (g_fail == 0) {
        std::printf("OK: 全部趋势决策测试通过（海龟 / 抛物线SAR / 纯裸K）\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
