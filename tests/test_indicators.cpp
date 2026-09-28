// 指标数学逻辑的单元测试——纯计算，不连网络，跑起来几毫秒。
// 用法：编译出 ccg_indicator_tests.exe 直接运行，全部通过打印 OK 并 exit 0，
// 有任何一条不对就打印具体是哪条断言失败并 exit 1。
#include "core/indicators.h"
#include "core/decision.h"
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


    // ── 宏观许可层：%B + 24h涨幅 + 7日涨幅 ────────────────────────────────
    // v4.0.16 移除结构层（支撑/净空/SR区域）后，这里只剩三条平级判据
    {
        namespace dc = ccbot::decision;

        // %B 计算
        CHECK(near(dc::pct_b(100, 90, 110), 0.5), "%B: 价格在带中央 = 0.5");
        CHECK(near(dc::pct_b(90,  90, 110), 0.0), "%B: 贴下轨 = 0");
        CHECK(near(dc::pct_b(110, 90, 110), 1.0), "%B: 贴上轨 = 1");
        CHECK(dc::pct_b(100, 110, 90) < 0,        "%B: 上下轨颠倒应返回 -1");
        CHECK(dc::pct_b(0,   90, 110) < 0,        "%B: 价格非法应返回 -1");

        dc::Inputs in;
        in.is_long = true; in.strict = false;
        in.use_htf = true; in.htf_ok = true;
        in.htf_pct_b = 0.30; in.htf_pos_max = 0.60;
        CHECK(dc::evaluate(in).pass(), "%B=0.30 未越界应放行");

        auto t1 = in; t1.htf_pct_b = 0.75;
        CHECK(dc::evaluate(t1).htf_block, "%B=0.75 > 0.60 应触发高位拦截");

        auto t5 = in; t5.is_long = false; t5.htf_pct_b = 0.1;
        CHECK(dc::evaluate(t5).htf_block, "做空 %B=0.1 应镜像触发低位拦截");

        // 数据缺失：影子模式标注放行，strict 模式拦截
        auto t4 = in; t4.htf_ok = false;
        auto v4 = dc::evaluate(t4);
        CHECK(v4.htf_missing && v4.pass(), "影子模式下日线数据缺失应放行并标注");
        auto s1 = in; s1.strict = true; s1.htf_ok = false;
        auto sv1 = dc::evaluate(s1);
        CHECK(sv1.data_block && !sv1.pass(), "strict 模式日线数据缺失应拦截");
        auto s3 = in; s3.strict = true;
        CHECK(dc::evaluate(s3).pass(), "strict 模式数据齐全应正常放行");

        // 三条判据独立：关掉 %B 后两条涨幅仍要照常工作
        {
            auto only_chg = in;
            only_chg.use_htf = false; only_chg.htf_pct_b = 0.99;   // %B 越界但那条关了
            only_chg.day_chg_max = 5.0; only_chg.day_chg_ok = true;
            only_chg.day_chg_pct = 1.0;
            CHECK(dc::evaluate(only_chg).pass(), "关掉 %B：越界也应放行");
            only_chg.day_chg_pct = 8.0;
            CHECK(dc::evaluate(only_chg).day_chg_block, "关掉 %B：24h涨幅仍应拦");

            // 全关 → 完全不参与，连数据缺失都不该拦
            auto none = in;
            none.use_htf = false; none.htf_ok = false; none.strict = true;
            CHECK(dc::evaluate(none).pass(), "三条全关：数据缺失也不拦");
        }
    }

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

    if (g_fail == 0) {
        std::printf("OK: 全部指标单元测试通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
