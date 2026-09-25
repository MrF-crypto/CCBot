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
    // 仓位大小的两种算法：
    //   Notional  —— 固定名义价值（budget_usdt 就是名义）
    //   RiskBased —— 固定【单次愿亏金额】，名义由 ATR 反推：
    //                  名义 = risk_usdt / (k × ATR%)
    //
    // 为什么要有 RiskBased：同一份名义价值，在 4h ATR 1.6% 的 LTC 和 5.3% 的
    // COTI 上，单次止损亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，
    // 而"铺开品种分散风险"这件事就此失效。等风险下单才让多品种配置真正成立。
    // 这是海龟的"单位"概念。
    enum class SizeMode { Notional, RiskBased };

    std::string symbol;
    SizeMode    size_mode   = SizeMode::Notional;
    // RiskBased 时：单次止损愿意亏多少钱。名义上限仍由 budget_usdt 兜住——
    // ATR 极小时 risk/(k×ATR) 会算出一个荒谬的大仓位，必须有帽子
    double      risk_usdt   = 0;
    double      budget_usdt = 1000;   // Notional：名义价值；RiskBased：名义上限
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
    // 裸K线模式的快照
    bool    bar_ok      = false;
    bool    bar_bullish = false;
    bool    bar_bearish = false;
    double  swing_low   = 0;
    double  swing_high  = 0;
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
    // 从落盘快照恢复：cfg 用传入的最新配置，仓位/止损线/统计用快照里的值。
    // 与 add_bot 的区别是它【不清零持仓状态】——重启后本地跟踪必须对齐回
    // 重启前，否则引擎以为自己空仓，看到信号会再开一笔，变成双倍敞口
    std::string restore_bot(SarBot snapshot);
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
    // 裸K线模式的快照（与 update_signal 分开：两种模式要的数据不同，
    // 合成一个大函数会让调用方被迫为用不到的参数填占位值）
    void update_bars(const std::string& bot_id, bool bullish, bool bearish,
                     double swing_low, double swing_high, int64_t bar_open_ms);

    // 该多久拉一次信号（秒）。短周期必须拉得更密：3m 的K线若 60 秒才查一次，
    // 最坏情况要等 60 秒才发现它收盘了——那是整根K线的 1/3，入场点会明显漂移。
    // 取周期的 1/4 并夹在 [15, 60] 秒之间
    static int signal_period_sec(const std::string& interval);

    // 由价格流每 tick 调用
    void tick(const std::string& symbol, double price);

    // ── 与交易所对账 ────────────────────────────────────────────────────────
    // 交易所实际持仓的精简视图（应用层从 TradingClient::fetch_positions 转换）
    struct ExchangePos {
        std::string symbol;
        int         direction   = 0;   // 1=多 -1=空
        double      qty         = 0;   // 绝对值
        double      entry_price = 0;
    };
    // 判定规则（比 DCA 版保守，因为 SAR 没有"层"的概念可供收敛）：
    //   本地有仓、交易所没有  → 外部已平：清空本地并【停止】该 bot。
    //                          不自动续跑：分不清是人工平的还是被强平的，
    //                          后者继续开仓是在往坑里跳
    //   本地qty > 交易所qty   → 外部部分平仓：本地数量收敛到交易所值，
    //                          止损线与开仓价保留（它们仍然成立）
    //   本地qty < 交易所qty   → 交易所多出：仅告警，不动本地状态
    //   本地空仓、交易所有仓  → 孤儿仓：【停止】该 bot 并告警。
    //                          这是最危险的一种——不停的话引擎以为自己空仓，
    //                          下一个突破信号会再开一笔，净敞口翻倍
    // 返回每条不一致的可读描述（空 = 完全一致）
    std::vector<std::string> reconcile_positions(const std::vector<ExchangePos>& exchange);

    // 仅测试用：见 CcgEngine::set_pending_for_test 的理由
    void set_pending_for_test(const std::string& bot_id, bool v);

private:
    // 平仓（可选紧接着反手）。两笔单在同一任务里顺序执行，全程持 pending
    void submit_close(const std::string& bot_id, const std::string& reason,
                      sar::Pos reverse_to);
    void submit_open (const std::string& bot_id, sar::Pos dir, bool from_reverse);
    void submit_add  (const std::string& bot_id);
    void clear_pending_after_throw(const std::string& bot_id, const std::string& what);
    void log(const std::string& msg) const;

    // 计算下单数量。Notional 用固定名义；RiskBased 用【真实止损距离】反推
    // 并受 budget 封顶。
    // ⚠ 用 stop_price 而不是 k×ATR：裸K线模式的止损是摆动低点，和 ATR 无关。
    //   拿 ATR 去算那个模式的仓位，算出来的"单次愿亏"是假的
    // stop_price<=0 或与 price 重合时无法计算，返回 0（调用方跳过下单）
    double plan_qty(const SarConfig& cfg, double price, double stop_price) const;

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
