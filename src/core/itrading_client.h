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
};

} // namespace ccbot
