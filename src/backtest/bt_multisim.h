#pragma once
#include "core/itrading_client.h"
#include <string>
#include <map>
#include <cmath>
#include <algorithm>

// 多品种模拟撮合器：组合回测用，按品种分别记仓位，账户级汇总盈亏。
// 滑点用每品种当分钟的真实买卖价差，手续费 taker 双边，资金费按结算点扣收。
namespace ccbot::bt {

class MultiSimClient : public ccbot::ITradingClient {
public:
    struct Pos { double qty = 0, avg = 0; bool is_long = true; };

    explicit MultiSimClient(double taker_fee = 0.0005, double extra_slip = 0.0002)
        : fee_(taker_fee), extra_slip_(extra_slip) {}

    void set_market(const std::string& sym, double price, double spread) {
        auto& m = mkt_[sym];
        m.first = price; m.second = spread;
    }
    double price_of(const std::string& sym) const {
        auto it = mkt_.find(sym);
        return it == mkt_.end() ? 0.0 : it->second.first;
    }

    ccbot::OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                           double qty, bool reduce_only) override {
        ccbot::OrderOutcome r;
        auto mit = mkt_.find(symbol);
        if (mit == mkt_.end() || mit->second.first <= 0 || qty <= 0) {
            r.error = "无市场数据"; return r;
        }
        const double px = mit->second.first, spread = mit->second.second;
        auto& p = pos_[symbol];
        const bool is_buy = (side == "BUY");

        if (reduce_only && p.qty <= 1e-12) { r.error = "[-2022] ReduceOnly Order is rejected."; return r; }
        double fill_qty = reduce_only ? std::min(qty, p.qty) : qty;
        if (fill_qty <= 0) { r.error = "[-2022] ReduceOnly Order is rejected."; return r; }

        const double slip = spread * 0.5 + extra_slip_;
        const double fill_px = px * (is_buy ? (1.0 + slip) : (1.0 - slip));
        const double fee = fill_qty * fill_px * fee_;

        if (reduce_only) {
            double pnl = p.is_long ? (fill_px - p.avg) * fill_qty : (p.avg - fill_px) * fill_qty;
            realized_ += pnl - fee;
            p.qty -= fill_qty;
            if (p.qty <= 1e-12) { p.qty = 0; p.avg = 0; }
        } else {
            double nq = p.qty + fill_qty;
            p.avg = (p.qty > 0) ? (p.avg * p.qty + fill_px * fill_qty) / nq : fill_px;
            p.qty = nq;
            p.is_long = is_buy;
            realized_ -= fee;
        }
        fees_ += fee;
        ++orders_;
        r.ok = true;
        r.order_id = "sim-" + std::to_string(orders_);
        r.avg_price = fill_px;
        r.executed_qty = fill_qty;
        return r;
    }

    double round_qty(const std::string&, double qty) override {
        if (qty <= 0) return 0;
        double step = qty >= 10000 ? 1.0 : qty >= 1000 ? 0.1 : qty >= 100 ? 0.01
                    : qty >= 1 ? 0.001 : qty >= 0.1 ? 0.0001
                    : qty >= 0.01 ? 0.00001 : 0.000001;
        double v = std::floor(qty / step + 1e-9) * step;
        return v < step ? 0.0 : v;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }

    void settle_funding(const std::string& sym, double rate) {
        auto pit = pos_.find(sym);
        if (pit == pos_.end() || pit->second.qty <= 0 || rate == 0) return;
        double px = price_of(sym);
        double pay = pit->second.qty * px * rate * (pit->second.is_long ? 1.0 : -1.0);
        realized_ -= pay;
        funding_ += pay;
    }

    // 账户级汇总
    double realized() const { return realized_; }
    double fees()     const { return fees_; }
    double funding()  const { return funding_; }
    int    orders()   const { return orders_; }
    double unrealized() const {
        double u = 0;
        for (const auto& [sym, p] : pos_) {
            if (p.qty <= 0) continue;
            double px = price_of(sym);
            if (px <= 0) continue;
            u += p.is_long ? (px - p.avg) * p.qty : (p.avg - px) * p.qty;
        }
        return u;
    }
    int open_positions() const {
        int n = 0;
        for (const auto& [s, p] : pos_) if (p.qty > 0) ++n;
        return n;
    }
    double total_notional() const {
        double n = 0;
        for (const auto& [sym, p] : pos_) {
            if (p.qty <= 0) continue;
            double px = price_of(sym);
            n += p.qty * (px > 0 ? px : p.avg);
        }
        return n;
    }

private:
    double fee_, extra_slip_;
    std::map<std::string, std::pair<double,double>> mkt_;   // sym → {price, spread}
    std::map<std::string, Pos> pos_;
    double realized_ = 0, fees_ = 0, funding_ = 0;
    int    orders_ = 0;
};

} // namespace ccbot::bt
