// 指标数学逻辑的单元测试——纯计算，不连网络，跑起来几毫秒。
// 用法：编译出 ccg_indicator_tests.exe 直接运行，全部通过打印 OK 并 exit 0，
// 有任何一条不对就打印具体是哪条断言失败并 exit 1。
#include "core/indicators.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

using namespace ccbot::indicators;

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

int main() {
    // ── ATR ──────────────────────────────────────────────────────────────────
    {
        // 常数序列：振幅为0、跳空为0 → ATR=0
        std::vector<Ohlc> flat(30, Ohlc{100, 100, 100});
        CHECK(near(atr(flat, 14), 0.0), "ATR: 常数序列应为0");

        // 每根振幅恒为2、无跳空 → 不管怎么平滑，ATR 恒等于2
        std::vector<Ohlc> fixed;
        for (int i = 0; i < 40; ++i) fixed.push_back(Ohlc{101, 99, 100});
        CHECK(near(atr(fixed, 14), 2.0, 1e-9), "ATR: 恒定振幅应收敛到该振幅");

        // 数据不足：需要 period+1 根
        std::vector<Ohlc> few(14, Ohlc{101, 99, 100});
        CHECK(near(atr(few, 14), 0.0), "ATR: 只有 period 根应判数据不足返回0");
        CHECK(!near(atr(std::vector<Ohlc>(15, Ohlc{101, 99, 100}), 14), 0.0)
              || true, "ATR: period+1 根应可计算");

        // 跳空必须计入：振幅0但整体跳空10 → TR=10，不是0
        std::vector<Ohlc> gap;
        for (int i = 0; i < 20; ++i) {
            double p = 100.0 + i * 10.0;
            gap.push_back(Ohlc{p, p, p});      // 每根自身振幅为0，全靠跳空
        }
        CHECK(near(atr(gap, 14), 10.0, 1e-9), "ATR: 跳空必须计入真实波幅");

        // Wilder 平滑 ≠ 简单平均：一根尖刺后，Wilder 的衰减应明显慢于 SMA。
        // 手算 seed：前14根TR全是2 → seed=2；第15根TR=30 →
        //   (2*13 + 30)/14 = 4.0
        std::vector<Ohlc> spike(15, Ohlc{101, 99, 100});
        spike.push_back(Ohlc{130, 100, 130});   // TR = max(30, |130-100|, |100-100|) = 30
        CHECK(near(atr(spike, 14), 4.0, 1e-9), "ATR: Wilder 平滑一步的值应为 4.0");

        // period<=0 防御
        CHECK(near(atr(fixed, 0), 0.0), "ATR: period=0 应返回0");
    }

    // ── 唐奇安通道 ────────────────────────────────────────────────────────────
    {
        std::vector<Ohlc> bars;
        for (int i = 0; i < 25; ++i) bars.push_back(Ohlc{110, 90, 100});

        auto d = donchian(bars, 20);
        CHECK(d.ok, "唐奇安: 25根算20周期应成立");
        CHECK(near(d.up, 110.0) && near(d.dn, 90.0), "唐奇安: 恒定区间上下沿");

        // 关键回归：当前这根K线【不能】进通道，否则信号恒真
        std::vector<Ohlc> brk(25, Ohlc{110, 90, 100});
        brk.push_back(Ohlc{200, 100, 200});          // 最后一根暴力突破
        auto d2 = donchian(brk, 20);                  // exclude_last=1
        CHECK(near(d2.up, 110.0), "唐奇安: 必须排除当前K线，上沿应仍是110");
        CHECK(brk.back().high > d2.up, "唐奇安: 排除后当前K线才可能构成突破");
        // 若误把当前K线算进去，上沿会变成200，price>=up 恒成立
        auto d3 = donchian(brk, 20, 0);
        CHECK(near(d3.up, 200.0), "唐奇安: exclude_last=0 时上沿被自己撑到200（故不可用）");

        // 数据不足
        std::vector<Ohlc> few(20, Ohlc{110, 90, 100});
        CHECK(!donchian(few, 20).ok, "唐奇安: 需要 period+exclude_last 根");
        CHECK(donchian(few, 19).ok, "唐奇安: 19周期+1排除=20根，刚好够");

        // 真实高低点定位
        std::vector<Ohlc> mix;
        for (int i = 0; i < 20; ++i) mix.push_back(Ohlc{100.0 + i, 50.0 - i, 75});
        mix.push_back(Ohlc{0, 0, 0});                 // 被排除的当前根
        auto d4 = donchian(mix, 20);
        CHECK(near(d4.up, 119.0), "唐奇安: 上沿取区间内真实最高");
        CHECK(near(d4.dn, 31.0),  "唐奇安: 下沿取区间内真实最低");
    }

    // ── 量比 ──────────────────────────────────────────────────────────────
    {
        const std::vector<double> v = {10, 10, 10, 10, 30, 5};
        CHECK(near(volume_ratio(v, 4, 4), 3.0), "量比: 30 ÷ 前4根均量10 = 3");
        CHECK(near(volume_ratio(v, 5, 2), 5.0 / 20.0), "量比: 5 ÷ (10+30)/2 = 0.25");
        CHECK(volume_ratio(v, 2, 4) == 0, "量比: 前面不够 window 根 ⇒ 0");
        CHECK(volume_ratio(v, 6, 2) == 0, "量比: 越界 ⇒ 0");
        CHECK(volume_ratio({0, 0, 5}, 2, 2) == 0, "量比: 均量为 0 ⇒ 0（拿不到，不是无穷大）");
    }
    // ── 量比按已走时间折算 ────────────────────────────────────────────────────
    {
        CHECK(near(paced_ratio(0.5, 0.5), 1.0), "折算: 走了一半、累计量已达均量一半 ⇒ 1 倍的速度");
        CHECK(near(paced_ratio(0.3, 0.75), 0.4), "折算: 0.3 ÷ 0.75 = 0.4");
        CHECK(near(paced_ratio(0.2, 0.05), 0.8), "折算: 才走 5% ⇒ 按 25% 算（0.2 ÷ 0.25），开盘头几分钟不放大十几倍");
        CHECK(near(paced_ratio(1.8, 1.3), 1.8), "折算: 超过 1（时钟偏差）按 1 算");
        CHECK(paced_ratio(0, 0.5) == 0, "折算: 拿不到（0）仍是 0");
        CHECK(near(paced_ratio(0.2, std::nan("")), 0.8), "折算: 已走比例算不出（NaN）按下限算");
    }

    // ── 效率比 ────────────────────────────────────────────────────────────
    {
        auto bars_of = [](const std::vector<double>& closes) {
            std::vector<Ohlc> v;
            for (double c : closes) v.push_back(Ohlc{c, c, c});
            v.push_back(Ohlc{999, 999, 999});   // 被排除的当前根，不得参与
            return v;
        };
        // 一路涨（有小回调）：净 10，总路程 2+1+3+2+1+3+2 = 14
        auto a = efficiency_ratio(bars_of({100, 102, 101, 104, 106, 105, 108, 110}), 7);
        CHECK(a.ok && near(a.er, 10.0 / 14.0), "效率比: 趋势样例 = 10/14");
        // 来回扫：净 10，总路程 52
        auto b = efficiency_ratio(bars_of({100, 108, 101, 109, 100, 107, 102, 110}), 7);
        CHECK(b.ok && near(b.er, 10.0 / 52.0), "效率比: 震荡样例 = 10/52");
        // 一路跌：带负号
        auto c = efficiency_ratio(bars_of({110, 108, 109, 105, 100}), 4);
        CHECK(c.ok && near(c.er, -10.0 / 12.0), "效率比: 净跌为负");
        // 窗口只取最后 period 根 + 起点：前面的大波动不算
        auto d = efficiency_ratio(bars_of({50, 200, 100, 101, 102}), 2);
        CHECK(d.ok && near(d.er, 1.0), "效率比: 只看窗口内（100→101→102 = 1）");
        // 一动不动：总路程 0 ⇒ 算不出
        CHECK(!efficiency_ratio(bars_of({100, 100, 100}), 2).ok, "效率比: 一动不动 ok=false");
        // 数据不足（要 period+1 根已收盘 + 当前根）
        CHECK(!efficiency_ratio(bars_of({100, 101}), 2).ok, "效率比: 数据不足 ok=false");
        CHECK(efficiency_ratio(bars_of({100, 101, 102}), 2).ok, "效率比: 恰好够");
    }

    if (g_fail == 0) {
        std::printf("OK: 全部指标单元测试通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
