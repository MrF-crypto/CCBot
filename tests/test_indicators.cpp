// 指标数学逻辑的单元测试——纯计算，不连网络，跑起来几毫秒。
// 用法：编译出 ccg_indicator_tests.exe 直接运行，全部通过打印 OK 并 exit 0，
// 有任何一条不对就打印具体是哪条断言失败并 exit 1。
#include "core/indicators.h"
#include "core/dynamic_params.h"
#include "core/sr_zones.h"
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
    // ── Bollinger ────────────────────────────────────────────────────────────
    {
        // 常数序列：标准差为0，三条线重合
        std::vector<double> closes(5, 10.0);
        auto r = bollinger(closes, 5, 2.0);
        CHECK(r.ok, "常数序列 boll 应该 ok");
        CHECK(near(r.mb, 10.0), "常数序列均值应为10");
        CHECK(near(r.ub, 10.0), "常数序列标准差为0，上轨应等于均值");
        CHECK(near(r.lb, 10.0), "常数序列标准差为0，下轨应等于均值");
    }
    {
        // 1,2,3,4,5：手算 mean=3, 总体方差=2, sd=sqrt(2)
        std::vector<double> closes = {1, 2, 3, 4, 5};
        auto r = bollinger(closes, 5, 1.0);
        double sd = std::sqrt(2.0);
        CHECK(r.ok, "1..5 boll 应该 ok");
        CHECK(near(r.mb, 3.0), "1..5 均值应为3");
        CHECK(near(r.ub, 3.0 + sd), "1..5 上轨应为 mean+sd");
        CHECK(near(r.lb, 3.0 - sd), "1..5 下轨应为 mean-sd");
    }
    {
        // 数据不够 period 根，应该返回 ok=false
        std::vector<double> closes = {1, 2, 3};
        auto r = bollinger(closes, 20, 2.0);
        CHECK(!r.ok, "数据不足时 boll 应返回 ok=false");
    }

    // ── RSI ──────────────────────────────────────────────────────────────────
    {
        // 单调上涨：全是涨，avg_loss=0 → RSI=100
        std::vector<double> closes;
        for (int i = 1; i <= 15; ++i) closes.push_back((double)i);
        double v = rsi(closes, 14);
        CHECK(near(v, 100.0), "单调上涨 RSI 应为100");
    }
    {
        // 单调下跌：全是跌，avg_gain=0 → RSI=0
        std::vector<double> closes;
        for (int i = 15; i >= 1; --i) closes.push_back((double)i);
        double v = rsi(closes, 14);
        CHECK(near(v, 0.0), "单调下跌 RSI 应为0");
    }
    {
        // 数据不够 period+1 根，应返回中性值 50
        std::vector<double> closes = {1, 2, 3};
        double v = rsi(closes, 14);
        CHECK(near(v, 50.0), "数据不足时 RSI 应返回中性值50");
    }
    {
        // 手算精确值：44,44.5,43.5,44.5 period=3 → RSI=60
        std::vector<double> closes = {44.0, 44.5, 43.5, 44.5};
        double v = rsi(closes, 3);
        CHECK(near(v, 60.0), "手算样例 RSI 应为60");
    }

    // ── EMA / 带偏移SMA（v2.5 趋势状态机）────────────────────────────────────
    {
        // 常数序列：EMA 应等于该常数
        std::vector<double> closes(250, 7.5);
        CHECK(near(ema(closes, 200), 7.5), "常数序列 EMA 应等于常数本身");
        // 数据不足：返回 0
        std::vector<double> few = {1, 2, 3};
        CHECK(near(ema(few, 200), 0.0), "数据不足 EMA 应返回0");
        // 单调上涨：EMA 应落后于最新价但高于起点
        std::vector<double> up;
        for (int i = 1; i <= 250; ++i) up.push_back((double)i);
        double e = ema(up, 200);
        CHECK(e > 100.0 && e < 250.0, "单调上涨 EMA 应在起点和最新价之间且滞后");
    }
    {
        // sma_at 手算：1..10，period=3
        std::vector<double> c = {1,2,3,4,5,6,7,8,9,10};
        CHECK(near(sma_at(c, 3, 0), 9.0), "sma_at 末尾3根 (8+9+10)/3=9");
        CHECK(near(sma_at(c, 3, 2), 7.0), "sma_at 偏移2 (6+7+8)/3=7");
        CHECK(near(sma_at(c, 3, 8), 0.0), "sma_at 偏移出界应返回0");
        CHECK(near(sma_at(c, 0, 0), 0.0), "sma_at period=0 应返回0");
        // 斜率语义：上涨序列 now > prev
        CHECK(sma_at(c, 3, 0) > sma_at(c, 3, 3), "上涨序列中轨斜率应为正");
    }

    // ── 动态W参数推导（v2.3 动态W模式）───────────────────────────────────────
    {
        namespace dp = ccbot::dynparams;
        // 带宽：LB=99000, UB=101000 → W = 2000/99000*100 ≈ 2.0202%
        CHECK(near(dp::band_width_pct(99000, 101000), 2000.0 / 99000 * 100.0),
              "band_width_pct 手算样例");
        // 非法输入：下轨<=0 或上下轨倒挂应返回 0
        CHECK(near(dp::band_width_pct(0, 101000), 0.0),   "band_width lb=0 应返回0");
        CHECK(near(dp::band_width_pct(-1, 100), 0.0),      "band_width lb<0 应返回0");
        CHECK(near(dp::band_width_pct(101000, 99000), 0.0),"band_width 倒挂应返回0");
        CHECK(near(dp::band_width_pct(100, 100), 0.0),     "band_width ub==lb 应返回0");

        // 正常区间：W=2.1% → 间隔=0.7、追踪止盈=0.315、追踪建仓=0.21（都在夹逼范围内）
        CHECK(near(dp::interval_pct(2.1),    0.7),   "W=2.1 间隔应为 W/3=0.7");
        CHECK(near(dp::trail_tp_pct(2.1),    0.315), "W=2.1 追踪止盈应为 0.15W=0.315");
        CHECK(near(dp::trail_entry_pct(2.1), 0.21),  "W=2.1 追踪建仓应为 0.1W=0.21");

        // 下限夹逼：极窄带宽 W=0.6% → W/3=0.2 被抬到 0.3；0.15W=0.09→0.2；0.1W=0.06→0.15
        CHECK(near(dp::interval_pct(0.6),    0.3),  "窄带宽间隔应被夹到下限0.3");
        CHECK(near(dp::trail_tp_pct(0.6),    0.2),  "窄带宽追踪止盈应被夹到下限0.2");
        CHECK(near(dp::trail_entry_pct(0.6), 0.15), "窄带宽追踪建仓应被夹到下限0.15");

        // 上限夹逼：极宽带宽 W=6% → W/3=2.0 被压到 1.5；0.15W=0.9→0.6；0.1W=0.6→0.4
        CHECK(near(dp::interval_pct(6.0),    1.5), "宽带宽间隔应被夹到上限1.5");
        CHECK(near(dp::trail_tp_pct(6.0),    0.6), "宽带宽追踪止盈应被夹到上限0.6");
        CHECK(near(dp::trail_entry_pct(6.0), 0.4), "宽带宽追踪建仓应被夹到上限0.4");
    }

    // ── 支撑/阻力区域检测（v2.6 SR雷达）─────────────────────────────────────
    {
        namespace sz = ccbot::srzones;
        // ATR：恒定波幅 高105/低95/收100 → TR恒为10 → ATR=10
        std::vector<sz::Bar> flat;
        for (int i = 0; i < 30; ++i) flat.push_back({100, 105, 95, 100, 1000});
        CHECK(near(sz::atr(flat, 14), 10.0), "恒定波幅 ATR 应为10");
        CHECK(near(sz::atr({}, 14), 0.0), "空数据 ATR 应为0");

        // 摆动点：平坦序列中间插一根深低点，应被识别为唯一摆动低点
        std::vector<sz::Bar> vshape;
        for (int i = 0; i < 21; ++i) vshape.push_back({105, 106, 104, 105, 1000});
        vshape[10] = {104, 105, 98, 100, 2000};
        auto plows = sz::pivot_lows(vshape, 3);
        CHECK(plows.size() == 1 && plows[0] == 10, "V形序列应识别出唯一摆动低点@10");

        // 聚类成区域：三次探底到 ~100 附近，应聚成一个含100的支撑区，touches=3
        std::vector<sz::Bar> bounce;
        for (int i = 0; i < 60; ++i) bounce.push_back({105, 106, 104, 105, 1000});
        bounce[12] = {104, 105, 100.0, 101, 1500};
        bounce[28] = {104, 105, 100.3, 101, 1500};
        bounce[44] = {104, 105,  99.8, 101, 1500};
        auto zones = sz::detect_zones(bounce, 3, 0.5, 12);
        CHECK(sz::nearest_support(zones, 105.0) != nullptr, "现价下方应存在支撑区");
        // 区域池里必须有覆盖 ~100 探底价位的摆动聚类区（v2.7起池里还有POC/斐波等
        // 其他来源，"最近的支撑"未必是它，所以按覆盖价位查找而不是取最近）
        const sz::Zone* cluster = nullptr;
        for (const auto& z : zones)
            if (z.contains(100.0) && (z.src_mask & sz::SrcSwing)) cluster = &z;
        CHECK(cluster != nullptr, "三次探底应聚类出覆盖~100的摆动区");
        if (cluster) CHECK(cluster->touches >= 3, "摆动区触碰次数应>=3");

        // FVG：bar2 低点12 > bar0 高点10 → 看涨缺口 [10,12]，未回补应被保留
        std::vector<sz::Bar> gap = {
            {9.5, 10, 9, 9.8, 1000}, {10, 15, 9.5, 14.5, 3000}, {14, 16, 12, 15, 2000},
            {15, 16, 13, 15.5, 1000},
        };
        auto fvgs = sz::detect_fvg(gap, 1.0);
        CHECK(fvgs.size() == 1, "应检测出1个看涨FVG");
        if (!fvgs.empty()) {
            CHECK(near(fvgs[0].lo, 10.0) && near(fvgs[0].hi, 12.0), "FVG区间应为[10,12]");
        }
        // 回补后不应保留：加一根低点跌破10的K线
        gap.push_back({13, 13.5, 9.9, 10.5, 1000});
        CHECK(sz::detect_fvg(gap, 1.0).empty(), "被回补的FVG不应保留");

        // nearest_resistance：把探底例子的现价放到99，支撑区应变成上方阻力（攻防互换）
        const sz::Zone* res = sz::nearest_resistance(zones, 99.0);
        CHECK(res != nullptr && res->lo > 99.0, "现价跌破后原支撑区应作为上方阻力被查到");

        // POC：成交量高度集中在 ~50 的价位，主筹码峰应覆盖 50
        std::vector<sz::Bar> pocbars;
        for (int i = 0; i < 20; ++i) pocbars.push_back({50, 51, 49, 50, 5000});
        for (int i = 0; i < 10; ++i) pocbars.push_back({80, 81, 79, 80, 500});
        auto pocs = sz::detect_poc(pocbars, 2.0);
        CHECK(!pocs.empty() && pocs[0].contains(50.0), "主POC应覆盖成交量密集价位50");
        CHECK(!pocs.empty() && pocs[0].src_mask == sz::SrcPOC, "POC来源掩码应为SrcPOC");

        // 斐波那契：低点100(早)→高点200(晚)的上升波段，0.618回撤位=138.2 应在池中
        std::vector<sz::Bar> fibbars;
        for (int i = 0; i < 20; ++i) {
            double p = 100 + i * (100.0 / 19.0);
            fibbars.push_back({p, p + 1, p - 1, p, 1000});
        }
        auto fibs = sz::detect_fib(fibbars, 5.0);
        bool found618 = false;
        for (const auto& z : fibs) if (z.contains(138.2)) found618 = true;
        CHECK(fibs.size() == 3, "斐波那契应产生3档回撤位");
        CHECK(found618, "上升波段0.618回撤位138.2应在区域池中");

        // 共振合并：摆动区[99,101]与斐波区[100.5,101.5]重叠 → 合并为共振2、评分加权
        sz::Zone a; a.lo = 99;    a.hi = 101;   a.score = 2.0; a.src_mask = sz::SrcSwing; a.touches = 3;
        sz::Zone b; b.lo = 100.5; b.hi = 101.5; b.score = 0.9; b.src_mask = sz::SrcFib;   b.touches = 1;
        sz::Zone c; c.lo = 200;   c.hi = 201;   c.score = 1.0; c.src_mask = sz::SrcPOC;   c.touches = 1;
        auto mg = sz::merge_zones({a, b, c});
        CHECK(mg.size() == 2, "重叠的两区应合并，不重叠的独立保留");
        const sz::Zone* top = nullptr;
        for (const auto& z : mg) if (z.confluence() == 2) top = &z;
        CHECK(top != nullptr, "合并区共振数应为2");
        if (top) {
            CHECK(near(top->score, (2.0 + 0.9 * 0.8) * 1.5), "共振区评分应为(2+0.72)×1.5");
            CHECK(near(top->lo, 99.0) && near(top->hi, 101.5), "合并区间应取并集[99,101.5]");
            CHECK((top->src_mask & sz::SrcSwing) && (top->src_mask & sz::SrcFib),
                  "合并区来源掩码应含摆动+斐波");
        }
    }

    if (g_fail == 0) {
        std::printf("OK: 全部指标单元测试通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
