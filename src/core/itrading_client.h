#pragma once
#include <string>

// 引擎依赖的交易接口（引擎只用这4个方法）。
// 实盘由 TradingClient 实现；回测由 SimClient 实现模拟撮合——
// 这样回测跑的是【引擎本尊】而不是仿制品，策略逻辑零复制。
namespace ccbot {

struct OrderOutcome {
    bool        ok = false;
    std::string order_id;
    std::string error;
    double      avg_price    = 0;
    double      executed_qty = 0;
    bool        uncertain    = false;   // 状态不明（超时且查单失败）
};

class ITradingClient {
public:
    virtual ~ITradingClient() = default;

    virtual OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                            double qty, bool reduce_only) = 0;
    virtual double round_qty(const std::string& symbol, double qty) = 0;
    virtual bool   set_leverage(const std::string& symbol, int lev)  = 0;
    virtual bool   is_dual_mode() const = 0;

    // ── 交易所侧硬止损（进程外兜底，"保命单"）────────────────────────────────
    // 本地的移动止损活在进程里，程序崩溃、断电、误关窗口之后仓位就完全裸奔。
    // 这两个方法在交易所挂一张 STOP_MARKET + closePosition 的单子，
    // 开仓时挂一次、此后不动，只防瀑布，进程死了它还在。
    struct StopPlacement {
        std::string order_id;          // 非空 = 挂上了
        std::string error;             // 失败原因，给人看
        // 重试有没有意义。区分它的理由：参数类错误（精度不对、触发价在错误的
        // 一侧）重试十次也是同样的结果，而引擎的重试阶梯要跑二十多秒——
        // 那二十多秒里仓位没有进程外保护。分流之后这类错误直接走到兜底处置。
        //
        // ⚠ 默认 true。判错的两个方向代价不对称：
        //   误判成可重试  → 白等二十多秒，然后照样走兜底（只是慢）
        //   误判成不可重试 → 一个本来能恢复的抖动直接把仓位平掉（丢单）
        //   所以只对【确定无望】的错误码标 false
        bool retryable = true;
        bool ok() const { return !order_id.empty(); }
    };

    // 默认实现返回"不支持且不必重试"：回测的 SimClient 用不到它（回测里进程
    // 不会崩）。标 retryable=false 是有意的——否则模拟环境会白跑一遍重试阶梯
    virtual StopPlacement place_disaster_stop(const std::string& /*symbol*/,
                                              double /*stop_price*/,
                                              const std::string& /*entry_side*/) {
        return StopPlacement{"", "该客户端不支持交易所侧硬止损", false};
    }
    virtual bool cancel_disaster_stop(const std::string& /*symbol*/,
                                      const std::string& /*order_id*/) { return true; }
};

} // namespace ccbot
