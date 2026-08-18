#pragma once
#include <chrono>
#include <mutex>
#include <string>
#include <cstdint>

namespace ccbot {

// 币安 REST 限流闸门。
//
// 设计取向：**以服务器回报为准，不靠本地权重表**。
// 每个响应头里 X-MBX-USED-WEIGHT-1M 就是交易所自己算的本分钟已用权重——比在本地
// 维护一张"哪个端点几分权重"的表可靠得多：那张表会随币安调整而过期，而且一旦某处
// 漏算，本地账面永远偏低，闸门形同虚设。本地只做两件服务器给不了的事：
//   ① 请求间隔铺平（突发是本项目的真实风险：31个品种×每5分钟的指标+趋势批次会
//      挤在同一秒发出去）
//   ② 被封禁期间（429/418）主动拦住，不再往墙上撞
//
// 订单请求享有优先权：权重逼近上限时只延后【非订单】请求；订单只在真正被封禁时
// 才会被拦。理由是平仓/止损的延迟直接对应资金损失，而拉一次指标晚几秒无所谓。
class RateGate {
public:
    struct Limits {
        int    weight_per_min   = 2400;   // fapi 默认；papi 为 6000，构造时按账户类型给
        double soft_ratio       = 0.75;   // 超过此比例开始给非订单请求限速
        double hard_ratio       = 0.92;   // 超过此比例非订单请求直接等到下一分钟窗口
        int    min_gap_ms       = 40;     // 相邻请求最小间隔（铺平突发）
        int    order_min_gap_ms = 0;      // 订单不额外铺平
    };

    // 不用 `RateGate(Limits lim = {})`：那个默认实参要求在类定义内部就取到
    // Limits 的默认成员初始化器，Clang 严格执行这条规则并报错（MSVC 放行）。
    // 拆成两个构造函数就没有这个问题
    RateGate() = default;
    explicit RateGate(Limits lim) : lim_(lim) {}

    // 发请求【前】调用。会在必要时阻塞当前线程。
    // is_order=true 的请求只在封禁期间被拦，不参与软限速。
    void acquire(bool is_order);

    // 收到响应【后】调用。http_code / 响应头驱动状态更新。
    // headers 是原始响应头文本（大小写不敏感地查找）；resp_body 用于识别 -1003。
    void observe(long http_code, const std::string& headers, const std::string& resp_body);

    // 供界面/日志展示
    struct Snapshot {
        int     used_weight = 0;      // 服务器回报的本分钟已用权重
        int     limit       = 0;
        bool    banned      = false;
        int64_t ban_left_ms = 0;
        int     throttled   = 0;      // 累计被本地限速推迟的次数
        int     rejected    = 0;      // 累计收到的 429/418/-1003 次数
    };
    Snapshot snapshot() const;

private:
    using clock = std::chrono::steady_clock;
    mutable std::mutex mtx_;
    Limits  lim_{};
    int     used_weight_ = 0;
    clock::time_point weight_at_{};
    clock::time_point ban_until_{};
    clock::time_point last_send_{};
    int     throttled_ = 0;
    int     rejected_  = 0;
};

} // namespace ccbot
