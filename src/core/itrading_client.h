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

    // ── 交易所侧灾难止损单（进程外兜底）──────────────────────────────────────
    // 本地的追踪止盈/硬止损全都活在进程里，程序崩溃、断电、误关窗口之后仓位就
    // 完全裸奔。这两个方法在交易所挂一张 STOP_MARKET + closePosition 的单子，
    // 只防瀑布、不参与正常止盈，进程死了它还在。
    //
    // 默认空实现：回测的 SimClient 不需要它（回测里进程不会崩），只有实盘
    // TradingClient 覆盖。返回空串 = 未挂上（不支持或失败）。
    virtual std::string place_disaster_stop(const std::string& /*symbol*/,
                                            double /*stop_price*/,
                                            const std::string& /*entry_side*/) { return ""; }
    virtual bool cancel_disaster_stop(const std::string& /*symbol*/,
                                      const std::string& /*order_id*/) { return true; }
};

} // namespace ccbot
