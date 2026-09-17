#pragma once
#include "core/sar_decision.h"
#include "core/itrading_client.h"
#include "core/engine_host.h"
#include "core/thread_pool.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// SAR 引擎：趋势跟随 + 止损反转的执行层。
//
// 与 CcgEngine 并列、互不依赖，只共用 ITradingClient 和 ThreadPool。
// 决策全在 sar_decision.h（纯函数、已穷举单测），这里只负责"把决策变成订单"
// 以及所有和交易所打交道才会遇到的脏事：零成交、部分成交、状态不明、
// reduceOnly 被拒、数量取整后归零。
//
// ── 反手采用【方案A：两笔单】 ────────────────────────────────────────────────
// 先 reduceOnly 平掉，再开反向，两笔都在【同一个异步任务】里顺序执行，
// 全程 pending=true，中间不会被别的 tick 插进来。
//
// 相比"一笔双倍单直接翻仓"多了几百毫秒空窗，换来的是：成交记录、盈亏归属、
// 资金费都能一笔一笔对上。这个程序的记账正确性一直是硬要求（幂等 clientOrderId
// 那一整套就是为此），对账错乱的代价远大于空窗。
//
// ⚠ 铁律：平仓没成功就【绝不】开反向。否则原仓位还在、又加一个反向仓，
//   净敞口翻倍且方向不明——这是方案A唯一的致命失败模式，代码里单独守着。
namespace ccbot {

struct SarConfig {
    std::string symbol;
    double      budget_usdt = 1000;   // 每笔仓位的名义价值
    int         leverage    = 3;
    std::string interval    = "4h";   // 信号K线周期
    sar::Config rule;                 // 通道/ATR/k/反手规则
    // 数据新鲜度上限：超过这个时长的指标快照不参与【开新仓】判定。
    // 已持仓的止损线不受影响（沿用最后一条有效线），见 sar::step
    int         signal_max_age_sec = 900;
};

struct SarBot {
    enum class State { Running, Stopped };

    std::string bot_id;
    SarConfig   cfg;
    sar::State  st;
    State       state   = State::Running;
    bool        pending = false;   // 有在途订单，本 tick 跳过

    double current_price = 0;
    double qty           = 0;      // 当前持仓数量（绝对值，0=空仓）

    // 信号快照（由应用层拉取K线后喂入）
    bool    sig_ok  = false;
    double  atr     = 0;
    double  atr_pct = 0;
    bool    dc_ok   = false;
    double  dc_up   = 0;
    double  dc_dn   = 0;
    std::chrono::steady_clock::time_point sig_time{};
    int64_t bar_open_ms = 0;       // 最新信号快照所属K线的开盘时间
    // tick 里已经把上面那根算进冷却的K线。两者分开是因为 update_signal 与 tick
    // 在不同线程按不同节奏跑：只有 tick 能安全地递减冷却，而它必须知道
    // "这根K线我数过没有"——否则同一根K线内的每个 tick 都会扣一次
    int64_t last_counted_bar_ms = 0;

    double realized_pnl = 0;
    int    trade_count  = 0;
    int    win_count    = 0;

    std::chrono::system_clock::time_point start_time{};
    std::string last_action;
    std::string last_decision;
};

// 成交记录。刻意不复用 CcgEngine::TradeRecord——那个带 layers（层数），
// 是 DCA 的概念，SAR 里没有层；混用会让统计口径悄悄串味
struct SarTrade {
    std::string symbol;
    sar::Pos    side = sar::Pos::Flat;   // 本笔的方向
    double      entry_price = 0;
    double      exit_price  = 0;
    double      qty         = 0;
    double      pnl         = 0;
    std::string reason;
    bool        reversed    = false;     // 本次平仓后是否反手了
    std::chrono::system_clock::time_point close_time{};
};

class SarEngine {
public:
    using LogCb   = std::function<void(const std::string&)>;
    using TradeCb = std::function<void(const SarTrade&)>;

    SarEngine(std::shared_ptr<ITradingClient> client,
              std::shared_ptr<ThreadPool>     pool);
    // 测试/回测构造：注入内联执行器与虚拟时钟
    SarEngine(std::shared_ptr<ITradingClient> client, EngineHost host);

    std::string add_bot(const SarConfig& cfg);   // 同品种已存在则返回空串
    void stop_bot  (const std::string& bot_id);
    void resume_bot(const std::string& bot_id);
    void close_bot (const std::string& bot_id);  // 手动市价平仓
    void remove_bot(const std::string& bot_id);
    void stop_all();
    std::vector<SarBot> get_bots() const;

    void set_log_cb(LogCb cb)     { log_cb_   = std::move(cb); }
    void set_trade_cb(TradeCb cb) { trade_cb_ = std::move(cb); }

    // 应用层拉到K线后喂入信号快照（ATR + 唐奇安通道 + 当前K线开盘时间）
    void update_signal(const std::string& bot_id, double atr, double atr_pct,
                       bool dc_ok, double dc_up, double dc_dn, int64_t bar_open_ms);

    // 由价格流每 tick 调用
    void tick(const std::string& symbol, double price);

    // 仅测试用：见 CcgEngine::set_pending_for_test 的理由
    void set_pending_for_test(const std::string& bot_id, bool v);

private:
    // 平仓（可选紧接着反手）。两笔单在同一任务里顺序执行，全程持 pending
    void submit_close(const std::string& bot_id, const std::string& reason,
                      sar::Pos reverse_to);
    void submit_open (const std::string& bot_id, sar::Pos dir, bool from_reverse);
    void clear_pending_after_throw(const std::string& bot_id, const std::string& what);
    void log(const std::string& msg) const;

    // 计算下单数量。名义预算 / 价格，再按交易所步长取整
    double plan_qty(const SarConfig& cfg, double price) const;

    std::shared_ptr<ITradingClient> client_;
    std::shared_ptr<ThreadPool>     pool_;
    EngineHost                      host_;
    mutable std::recursive_mutex    mtx_;
    std::map<std::string, SarBot>   bots_;
    LogCb                           log_cb_;
    TradeCb                         trade_cb_;
    int                             seq_ = 0;
};

} // namespace ccbot
