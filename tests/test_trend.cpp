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

int main() {
    test_turtle();
    test_psar();
    test_bare_k();

    if (g_fail == 0) {
        std::printf("OK: 全部趋势决策测试通过（海龟 / 抛物线SAR / 纯裸K）\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
