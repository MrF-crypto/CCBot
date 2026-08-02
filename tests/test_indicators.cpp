// 指标数学逻辑的单元测试——纯计算，不连网络，跑起来几毫秒。
// 用法：编译出 ccg_indicator_tests.exe 直接运行，全部通过打印 OK 并 exit 0，
// 有任何一条不对就打印具体是哪条断言失败并 exit 1。
#include "core/indicators.h"
#include "core/dynamic_params.h"
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

    if (g_fail == 0) {
        std::printf("OK: 全部指标单元测试通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
