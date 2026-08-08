#pragma once
#include "core/itrading_client.h"
#include "backtest/bt_data.h"
#include <string>
#include <vector>
#include <cstdio>

// 模拟撮合器：实现 ITradingClient，让回测跑【引擎本尊】。
// 成交模型：市价单在当前价上按【该分钟的真实买卖价差】滑点成交（数据里的
// Spread 列），再扣 taker 手续费。资金费由回放器在结算点扣收。
namespace ccbot::bt {

struct SimConfig {
    double taker_fee   = 0.0005;   // 双边各 0.05%
    double slip_mult   = 0.5;      // 滑点 = spread × 此系数（半个价差，市价吃单的典型成本）
    double extra_slip  = 0.0002;   // 额外滑点补偿（bar内路径近似造成的乐观偏差）
    double qty_step    = 0.0;      // 0=按价格量级自动推断精度
    double min_qty     = 0.0;
    int    leverage    = 3;
};

// 一笔成交记录
struct Fill {
    int64_t     ts_ms = 0;
    std::string symbol;
    std::string side;        // BUY / SELL
    double      qty   = 0;
    double      price = 0;   // 含滑点的实际成交价
    double      fee   = 0;
    bool        reduce_only = false;
};

class SimClient : public ccbot::ITradingClient {
public:
    explicit SimClient(SimConfig cfg) : cfg_(cfg) {}

    // 回放器每个子tick更新：当前价与当前价差
    void set_market(const std::string& symbol, double price, double spread) {
        symbol_ = symbol; price_ = price; spread_ = spread;
    }

    // ── ITradingClient ──────────────────────────────────────────────────────
    ccbot::OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                           double qty, bool reduce_only) override {
        ccbot::OrderOutcome r;
        if (price_ <= 0 || qty <= 0) { r.error = "无效价格/数量"; return r; }

        const bool is_buy = (side == "BUY");
        // reduceOnly 平仓：交易所侧没有仓位时会被拒（-2022），模拟同款行为
        if (reduce_only && pos_qty_ <= 1e-12) { r.error = "[-2022] ReduceOnly Order is rejected."; return r; }
        double fill_qty = reduce_only ? std::min(qty, pos_qty_) : qty;
        if (fill_qty <= 0) { r.error = "[-2022] ReduceOnly Order is rejected."; return r; }

        // 滑点：买单向上、卖单向下偏离（真实价差的一半 + 路径近似补偿）
        double slip = (spread_ * cfg_.slip_mult + cfg_.extra_slip);
        double fill_price = price_ * (is_buy ? (1.0 + slip) : (1.0 - slip));
        double fee = fill_qty * fill_price * cfg_.taker_fee;

        if (reduce_only) {
            // 平仓：结算已实现盈亏（多头：卖价-均价；空头相反）
            double pnl = pos_long_ ? (fill_price - pos_avg_) * fill_qty
                                   : (pos_avg_ - fill_price) * fill_qty;
            realized_ += pnl - fee;
            pos_qty_  -= fill_qty;
            if (pos_qty_ <= 1e-12) { pos_qty_ = 0; pos_avg_ = 0; }
        } else {
            // 开/加仓：更新持仓均价
            double new_qty = pos_qty_ + fill_qty;
            pos_avg_  = (pos_qty_ > 0) ? (pos_avg_ * pos_qty_ + fill_price * fill_qty) / new_qty
                                       : fill_price;
            pos_qty_  = new_qty;
            pos_long_ = is_buy;
            realized_ -= fee;   // 开仓手续费立即计入
        }
        fees_ += fee;
        ++order_count_;
        fills_.push_back({ now_ms_, symbol, side, fill_qty, fill_price, fee, reduce_only });

        r.ok = true;
        r.order_id     = "sim-" + std::to_string(order_count_);
        r.avg_price    = fill_price;
        r.executed_qty = fill_qty;
        return r;
    }

    double round_qty(const std::string&, double qty) override {
        // 按【数量本身】的量级推断精度（不能用价格——引擎首次调用时市场价可能还没设，
        // 而且真实交易所的 LOT_SIZE 本就是按数量量级定的：BTC 0.001、小币 1 或 10）
        double step = cfg_.qty_step > 0 ? cfg_.qty_step : auto_step_for(qty);
        double v = std::floor(qty / step + 1e-9) * step;
        double minq = cfg_.min_qty > 0 ? cfg_.min_qty : step;
        return v < minq ? 0.0 : v;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }

    // ── 回放器用 ────────────────────────────────────────────────────────────
    void set_now(int64_t ms) { now_ms_ = ms; }
    // 资金费结算：持仓过结算点，多头付正费率（费率为负则收）
    void settle_funding(double rate) {
        if (pos_qty_ <= 0 || rate == 0) return;
        double notional = pos_qty_ * price_;
        double pay = notional * rate * (pos_long_ ? 1.0 : -1.0);
        realized_ -= pay;
        funding_paid_ += pay;
    }

    double position_qty()   const { return pos_qty_; }
    double position_avg()   const { return pos_avg_; }
    bool   position_long()  const { return pos_long_; }
    double realized()       const { return realized_; }
    double fees()           const { return fees_; }
    double funding_paid()   const { return funding_paid_; }
    int    order_count()    const { return order_count_; }
    // 浮动盈亏（按当前价）
    double unrealized() const {
        if (pos_qty_ <= 0) return 0;
        return pos_long_ ? (price_ - pos_avg_) * pos_qty_ : (pos_avg_ - price_) * pos_qty_;
    }
    const std::vector<Fill>& fills() const { return fills_; }

private:
    // 数量精度自动推断：取比目标数量小 3~4 个数量级的步长，既保留精度
    // 又模拟真实 LOT_SIZE 的取整损失（如 0.0019 BTC → 步长0.001 → 0.001）
    static double auto_step_for(double qty) {
        if (qty <= 0) return 1.0;
        if (qty >= 10000) return 1.0;
        if (qty >= 1000)  return 0.1;
        if (qty >= 100)   return 0.01;
        if (qty >= 10)    return 0.001;
        if (qty >= 1)     return 0.001;
        if (qty >= 0.1)   return 0.0001;
        if (qty >= 0.01)  return 0.00001;
        return 0.000001;
    }

    SimConfig   cfg_;
    std::string symbol_;
    double price_  = 0;
    double spread_ = 0;
    int64_t now_ms_ = 0;

    double pos_qty_ = 0, pos_avg_ = 0;
    bool   pos_long_ = true;
    double realized_ = 0, fees_ = 0, funding_paid_ = 0;
    int    order_count_ = 0;
    std::vector<Fill> fills_;
};

} // namespace ccbot::bt
