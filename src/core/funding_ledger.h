#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

namespace ccbot {

class TradingClient;

// 资金费账本。
//
// 为什么需要它：永续合约每 8 小时结算一次资金费，这是**真实划走的现金**，不是浮亏。
// 对"套住就长期持有"的用法，它是唯一一笔不会因为价格涨回来而消失的成本——
// 一个被套一年的多单，光资金费就可能吃掉 10% 以上，而这笔账在界面上完全看不见。
//
// 它是【账本】不是风控：只记录和展示，不参与任何交易决策，也不会因为费用高就平仓。
//
// 归属口径：**按品种记，不按 bot**。交易所是按"账户×品种"收费的，同品种若有多个
// bot 无法真实归属——硬做归属就是编数字。
class FundingLedger {
public:
    struct Entry {
        double  total       = 0;   // 从开始追踪起累计（负数=已付出）
        double  since_open  = 0;   // 本轮持仓累计，仓位归零时重置
        int64_t cursor_ms   = 0;   // 已处理到的最后一条流水时间
        double  rate        = 0;   // 最新一期费率（正=多头付）
        int64_t next_ms     = 0;   // 下次结算时间
        int64_t rate_ms     = 0;   // 费率的获取时间（判断新鲜度）
    };

    // 记一笔流水。同一时间戳只记一次（重复调用幂等），防止分页重叠导致重复计数
    void apply(const std::string& symbol, double income, int64_t time_ms);
    // 仓位归零：本轮累计清零，总累计保留
    void reset_position(const std::string& symbol);
    void set_rate(const std::string& symbol, double rate, int64_t next_ms, int64_t now_ms);

    Entry   get(const std::string& symbol) const;
    int64_t cursor(const std::string& symbol) const;
    std::vector<std::string> symbols() const;

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // 年化持有成本%：费率 × 3次/天 × 365天。费率为负时结果为负（你在收钱）
    static double annualized_pct(double rate) { return rate * 3.0 * 365.0 * 100.0; }

    // 资金费把回本价推高了多少。
    // 已付资金费摊到每一份持仓上，就是均价之外还要多涨的部分——这是把"利息"
    // 换算成使用者真正关心的单位（还要涨多少才回本）
    static double effective_breakeven(double avg_price, double qty,
                                      double funding_since_open, bool is_long) {
        if (avg_price <= 0 || qty <= 0) return avg_price;
        const double per_unit = -funding_since_open / qty;   // 付出为正
        return is_long ? (avg_price + per_unit) : (avg_price - per_unit);
    }

private:
    mutable std::mutex          mtx_;
    std::map<std::string, Entry> map_;
};

// 把交易所的历史流水同步进账本。
//
// 币安的 income 接口不带时间范围时只返回最近 7 天，所以补历史必须按 7 天窗口分页。
// backfill_from_ms=0 时只做增量（从账本游标往后拉）。
// 返回本次新记录的条数。
int sync_funding_ledger(TradingClient& client, FundingLedger& ledger,
                        const std::vector<std::string>& symbols,
                        int64_t backfill_from_ms,
                        int max_windows = 30);

} // namespace ccbot
