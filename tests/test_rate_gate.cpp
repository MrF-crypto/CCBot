// 限流闸门测试。
//
// 这东西全是时间相关的逻辑，而且它坐在【每一个】HTTP 出口上——写错的后果是
// 要么形同虚设（该拦不拦），要么把交易卡死（不该拦也拦）。后者更可怕：
// 平仓请求被自己的限流器挡住，比被交易所限流还糟。
#include "net/rate_gate.h"
#include <cstdio>
#include <chrono>
#include <string>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// 计时一次 acquire 花了多久
static long long timed_acquire(RateGate& g, bool is_order) {
    auto t0 = std::chrono::steady_clock::now();
    g.acquire(is_order);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

int main() {
    std::printf("── 用例1：权重以【服务器回报】为准 ──\n");
    {
        RateGate g;
        g.observe(200, "HTTP/1.1 200 OK\r\nX-MBX-USED-WEIGHT-1M: 1234\r\n", "{}");
        check(g.snapshot().used_weight == 1234, "从 X-MBX-USED-WEIGHT-1M 读到权重");

        // 大小写不敏感——不同网关返回的大小写并不一致
        RateGate g2;
        g2.observe(200, "x-mbx-used-weight-1m: 777\r\n", "{}");
        check(g2.snapshot().used_weight == 777, "响应头大小写不敏感");

        // 没有该头时不应把权重清零（否则一次异常响应就让闸门失明）
        RateGate g3;
        g3.observe(200, "X-MBX-USED-WEIGHT-1M: 500\r\n", "{}");
        g3.observe(200, "Content-Type: application/json\r\n", "{}");
        check(g3.snapshot().used_weight == 500, "缺失该头时保留上次读数");
    }

    std::printf("\n── 用例2：429 / 418 进入封禁，Retry-After 生效 ──\n");
    {
        RateGate g;
        g.observe(429, "Retry-After: 2\r\n", "");
        auto s = g.snapshot();
        check(s.banned, "429 → 进入封禁");
        check(s.ban_left_ms > 1000 && s.ban_left_ms <= 2000, "封禁时长取 Retry-After（2秒）");
        check(s.rejected == 1, "计入 rejected");

        // 没给 Retry-After 时要有保底，且 418 必须比 429 更狠
        RateGate g429, g418;
        g429.observe(429, "", "");
        g418.observe(418, "", "");
        check(g429.snapshot().ban_left_ms > 20000, "429 无 Retry-After → 保底退避 ≥20s");
        check(g418.snapshot().ban_left_ms > g429.snapshot().ban_left_ms,
              "418(已封IP) 的退避必须比 429 更长");

        // -1003 出现在响应体里（HTTP 码可能仍是 200）
        RateGate g1003;
        g1003.observe(200, "", "{\"code\":-1003,\"msg\":\"Too many requests\"}");
        check(g1003.snapshot().banned, "响应体里的 -1003 同样触发封禁");
    }

    std::printf("\n── 用例3：封禁期间【订单也要等】──\n");
    {
        // 这条容易写错成"订单永远放行"。封禁期间继续发只会延长封禁，
        // 而且交易所根本不会受理——放行等于白白撞墙
        RateGate g;
        g.observe(429, "Retry-After: 1\r\n", "");
        long long ms = timed_acquire(g, /*is_order=*/true);
        check(ms >= 900, "封禁中订单请求被拦住等待（实测 " + std::to_string(ms) + "ms）");
        check(!g.snapshot().banned, "等待结束后封禁自动解除");
    }

    std::printf("\n── 用例4：正常情况下订单不被限速 ──\n");
    {
        // 反面保护：平仓/止损被自己的限流器拖延，比被交易所限流更糟
        RateGate::Limits lim;
        lim.min_gap_ms = 200;          // 非订单铺平 200ms
        lim.order_min_gap_ms = 0;      // 订单不铺平
        RateGate g(lim);
        g.acquire(true);
        long long ms = timed_acquire(g, true);
        check(ms < 50, "连续两个订单请求几乎无延迟（实测 " + std::to_string(ms) + "ms）");
    }

    std::printf("\n── 用例5：非订单请求被铺平（防突发）──\n");
    {
        // 31品种×指标+趋势的批次会把几十个请求挤在同一秒，这是本项目的真实风险
        RateGate::Limits lim;
        lim.min_gap_ms = 150;
        RateGate g(lim);
        g.acquire(false);
        long long ms = timed_acquire(g, false);
        check(ms >= 120, "相邻非订单请求被拉开到最小间隔（实测 " + std::to_string(ms) + "ms）");
    }

    std::printf("\n── 用例6：权重逼近上限时只限速非订单 ──\n");
    {
        RateGate::Limits lim;
        lim.weight_per_min = 1000;
        lim.soft_ratio = 0.75; lim.hard_ratio = 0.92;
        lim.min_gap_ms = 20; lim.order_min_gap_ms = 0;
        RateGate g(lim);
        g.observe(200, "X-MBX-USED-WEIGHT-1M: 850\r\n", "{}");   // 85% —— 软硬阈值之间

        g.acquire(false);
        long long slow = timed_acquire(g, false);
        check(slow > 20, "软阈值以上：非订单间隔被放大（实测 " + std::to_string(slow) + "ms）");
        check(g.snapshot().throttled > 0, "计入 throttled");

        long long fast = timed_acquire(g, true);
        check(fast < 50, "同一时刻订单请求不受影响（实测 " + std::to_string(fast) + "ms）");
    }

    std::printf("\n── 用例7：权重读数过期后不再限速 ──\n");
    {
        // weight_at_ 为空（从未收到过响应头）时不能当成"权重为0"以外的任何值，
        // 否则程序刚启动、还没发过请求就先把自己限死
        RateGate::Limits lim;
        lim.weight_per_min = 1000; lim.min_gap_ms = 0;
        RateGate g(lim);
        long long ms = timed_acquire(g, false);
        check(ms < 50, "从未收到过权重头时不限速（实测 " + std::to_string(ms) + "ms）");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
