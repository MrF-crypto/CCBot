#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ccbot {

class TradingClient;

// 手续费账本。
//
// 显示在顶部栏"资金费"旁边：交易所【实际扣掉】的手续费累计（资金流水里的
// COMMISSION），不是按费率估算出来的数。
//
// ── 统计口径 ────────────────────────────────────────────────────────────────
// 只统计"这个品种在本程序里有 bot 的那些时间段"里产生的手续费：
//   · 每个品种记一组时间段 [加 bot, 删 bot]；删了再加就是两段，都算
//   · 删掉 bot 之后照常再同步一次（到删除时刻之后 kGraceMs），删除前最后那笔
//     平仓的手续费不会因为"删得太快、还没轮到同步"而漏掉
//   · 交易所的流水只按"账户 × 品种"记，分不出哪笔是 bot 下的、哪笔是你在 App 上
//     手动下的 —— 时间段内的手动交易也会算进来。这是数据源决定的，不是遗漏
//   · 只累加 USDT 计价的流水；其它币种计价的单独计数、不混进合计
//
// ── 为什么不照搬资金费账本 ───────────────────────────────────────────────────
//   · 去重：资金费一个品种一次结算只有一条，"时间戳不大于游标就是重复"就够了。
//     手续费不行 —— 一笔市价单分多次成交，会在【同一毫秒】产生多条记录。
//     这里用"游标时刻 + 该毫秒里已处理的 tranId"去重
//   · 翻页：资金费 7 天才几十条，一页 1000 条够用；手续费按高频配置一个品种
//     7 天就可能上千条。这里一页拉满就把时间区间对半切开重拉，不依赖交易所
//     按什么顺序返回
//   · 同步状态：多一个 synced_to_ms（"已完整同步到哪一刻"）。没拉成的那段
//     不会被标成已同步，下次接着拉 —— 当成"这段没有手续费"就永久漏掉了
//
// 它是账本不是风控：只记录和展示，不参与任何交易决策。
class FeeLedger {
public:
    struct Span { int64_t from_ms = 0; int64_t to_ms = 0; };   // to_ms==0 ⇒ bot 仍在
    struct Entry {
        double  total         = 0;    // 计入的手续费合计（负数 = 付出去的）
        int     count         = 0;    // 计入的流水条数
        int     skipped_asset = 0;    // 非 USDT 计价、没有计入合计的条数
        std::vector<Span>    spans;
        int64_t              cursor_ms = 0;     // 已处理到的最后一条流水的时间
        std::vector<int64_t> ids_at_cursor;     // cursor_ms 那一毫秒里已处理的 tranId
        int64_t              synced_to_ms = 0;  // 已完整同步到的时刻
    };
    struct Job { std::string symbol; int64_t from_ms = 0; int64_t to_ms = 0; };

    static constexpr int64_t kGraceMs       = 5LL * 60 * 1000;          // 删 bot 后再同步到 +5 分钟
    static constexpr int64_t kOverlapMs     = 60LL * 1000;              // 增量同步往回重叠 1 分钟
    static constexpr int64_t kMaxLookbackMs = 90LL * 24 * 3600 * 1000;  // 交易所只能往回查有限一段

    // ── bot 存在的时间段 ──
    // 已有开着的时间段就不动（改策略参数是"删了再建"，不能把一段切成两段）
    void open_span(const std::string& symbol, int64_t from_ms);
    void close_span(const std::string& symbol, int64_t at_ms);
    // 补算历史用：直接记一段已经结束的时间段
    void add_closed_span(const std::string& symbol, int64_t from_ms, int64_t to_ms);
    // 拿当前 bot 列表对一遍：新出现的品种开一段，消失的品种关掉。返回是否有变化
    bool reconcile_active(const std::set<std::string>& active, int64_t now_ms);
    bool covers(const std::string& symbol, int64_t time_ms) const;

    // 记一笔流水。返回是否计入了合计。
    // ⚠ 调用方必须按 (time, tran_id) 升序喂入 —— 去重靠"游标 + 该毫秒的 tranId"
    bool apply(const std::string& symbol, int64_t tran_id, const std::string& asset,
               double income, int64_t time_ms);

    // 哪些品种需要同步、从哪到哪
    std::vector<Job> plan(int64_t now_ms) const;
    void mark_synced(const std::string& symbol, int64_t to_ms);

    double total() const;
    Entry  get(const std::string& symbol) const;
    std::vector<std::string> symbols() const;

    bool backfilled() const;
    void set_backfilled(bool v);

    bool load(const std::string& path);
    bool save(const std::string& path) const;

private:
    static bool covers_locked(const Entry& e, int64_t t);
    mutable std::mutex            mtx_;
    std::map<std::string, Entry>  map_;
    bool                          backfilled_ = false;
};

// 把交易所的手续费流水同步进账本。返回本次计入合计的条数。
// all_ok=false 表示至少一个品种这次没拉完（那部分不会被标成已同步，下次接着拉）。
// page_limit 平时用 1000（交易所上限），测试里调小以覆盖"一页拉满要切分"的路径
int sync_fee_ledger(TradingClient& client, FeeLedger& ledger, int64_t now_ms,
                    bool* all_ok = nullptr, int page_limit = 1000, int max_windows = 14);

} // namespace ccbot
