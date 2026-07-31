#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <chrono>
#include <memory>
#include <atomic>

namespace ccbot {

class TradingClient;
class ThreadPool;

// ─── CCG 策略配置 ──────────────────────────────────────────────────────────────
struct CcgConfig {
    enum class StratType {
        Flat,           // 平推：1,1,1,1,1,1
        Martingale,     // 倍投：1,1,2,2,4,4
        MartPlus,       // 倍投Plus（类斐波那契）
        Triple,         // 三倍：1,3,9,27
        Square,         // 平方：1,4,9,16
        Fibonacci,      // 斐波那契：1,1,2,3,5,8
        Lucas,          // 卢卡斯：2,1,3,4,7,11
        Linear          // 递增：1,2,3,4,5,6
    };
    enum class Direction { Long, Short, Both };
    // 首单入场模式：Immediate=一开监控就立刻市价开首仓（原有行为）；
    // Indicator=等 BOLL+RSI 信号满足才开首仓，之后网格加仓/止盈止损逻辑不变
    enum class EntryMode { Immediate, Indicator };

    std::string symbol;
    StratType   strat_type   = StratType::Martingale;
    Direction   direction    = Direction::Long;
    double      budget_usdt  = 3000.0;   // 预算资金（每个方向）
    int         max_entries  = 6;        // 最大加仓层数（共做单数）
    int         leverage     = 3;
    double      interval_pct = 8.0;      // 间隔比例 %（下跌多少触发下一仓条件）
    double      trail_entry  = 1.0;      // 追踪建仓 %（到达间隔后反弹多少才真正入场）
    double      tp_pct       = 5.0;      // 整体止盈 %（均价回升多少止盈）
    double      trail_tp     = 2.0;      // 追踪止盈 %（达到止盈后回落多少才平仓）
    bool        auto_restart  = true;
    int         cooldown_secs = 60;
    double      stop_loss_pct = 0.0;   // 均价跌幅超过此值强制平仓（0=禁用）

    // RSI 确认方式：Snapshot=当前这一刻 RSI 到没到阈值就行；
    // CrossFromOversold=必须先探底跌破 rsi_oversold_th，之后再回穿 rsi_threshold 才算数
    // （更严格的"动能反转"确认，避免在强趋势下跌中过早进场）
    enum class RsiConfirmMode { Snapshot, CrossFromOversold };

    // 指标信号首单（entry_mode == Indicator 时才生效）
    EntryMode   entry_mode     = EntryMode::Immediate;
    std::string kline_interval = "1h";
    int         boll_period    = 20;
    double      boll_mult      = 2.0;
    bool        use_rsi_filter = true;
    int         rsi_period     = 14;
    double      rsi_threshold  = 30.0;   // 多：RSI≥此值；空：RSI≤(100-此值)
    RsiConfirmMode rsi_confirm_mode = RsiConfirmMode::Snapshot;
    double         rsi_oversold_th  = 25.0;  // CrossFromOversold 模式：先跌破这个值才算探过底

    // 动态W模式（v2.3+）：把整个策略的锚点从"固定百分比"换成"布林带本身"——
    //   补仓锚定下轨：除了跌够动态间隔，价格还必须在带外（统计超卖位）才补
    //   止盈锚定上轨：价格触及上轨 且 盈利>=保底利润 才激活追踪止盈（不再用固定tp_pct）
    //   间距自适应：间隔/追踪参数由实时带宽W推导（见 dynamic_params.h），不再用配置里的固定值
    // 带子在下跌趋势中整体下移时梯子跟着走，均价贴着下轨，带内震荡就能完成多头周期。
    // 指标数据超时（>180s无更新）时冻结新补仓/止盈激活，宁可错过不可乱做。
    bool   dynamic_band_mode = false;
    double min_profit_floor  = 0.3;   // 动态模式止盈激活的最低盈利%（覆盖手续费+微利）
};

// ─── 单笔加仓记录 ──────────────────────────────────────────────────────────────
struct CcgEntry {
    int    level     = 0;
    double price     = 0;
    double qty       = 0;
    double cost_usdt = 0;
    std::string order_id;
    std::chrono::system_clock::time_point time;
};

// ─── 策略实例（一个 symbol × 一个方向 = 一个 Bot）────────────────────────────
struct CcgBot {
    enum class State { Running, Cooldown, Stopped };

    std::string bot_id;
    CcgConfig   cfg;
    State       state   = State::Running;
    bool        pending = false;   // 正在异步执行，本 tick 跳过

    // 持仓状态
    std::vector<CcgEntry> entries;
    double avg_price    = 0;
    double total_qty    = 0;
    double total_cost   = 0;
    double current_price= 0;

    // DCA 追踪（多：跌幅低点；空：涨幅高点）
    double last_entry_price = 0;
    double dca_extreme      = 0;
    bool   interval_hit     = false;

    // 止盈追踪
    bool   tp_reached       = false;
    double tp_extreme       = 0;

    // 指标信号快照（entry_mode==Indicator 时，由 UI 每个 tick 异步拉取后写入）
    bool   ind_ok      = false;
    double ind_boll_lb = 0;
    double ind_boll_ub = 0;
    double ind_rsi      = 50.0;
    // CrossFromOversold 模式：本轮"等待首单信号"期间，RSI 是否已经探底跌破过 rsi_oversold_th
    bool   ind_dipped   = false;
    // 最近一次指标写入时间（steady_clock，默认epoch=从未更新过=视为过期）。
    // 动态W模式和指标首单都用它做数据新鲜度检查，避免拿几小时前的旧轨道值做决策
    std::chrono::steady_clock::time_point ind_time{};

    // 统计
    double realized_pnl = 0;
    int    cycle_count  = 0;

    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point cooldown_until;
    std::string last_action;
};

// ─── 交易明细：每次平仓（止盈/止损/手动）产生一条记录 ─────────────────────────
struct TradeRecord {
    std::string           symbol;
    CcgConfig::Direction   direction    = CcgConfig::Direction::Long;
    double                 entry_price  = 0;
    double                 exit_price   = 0;
    double                 qty          = 0;
    double                 pnl          = 0;
    std::string            reason;   // "追踪止盈" / "硬止损" / "手动平仓" ...
    std::chrono::system_clock::time_point close_time;
};

// ─── CCG 引擎 ─────────────────────────────────────────────────────────────────
class CcgEngine {
public:
    using LogCb   = std::function<void(const std::string&)>;
    using TradeCb = std::function<void(const TradeRecord&)>;

    CcgEngine(std::shared_ptr<TradingClient> client,
              std::shared_ptr<ThreadPool>    pool);

    // Bot 管理
    // 返回 bot_id；若已存在同品种同方向的非停止 bot 则返回空串
    std::string add_bot   (const CcgConfig& cfg);
    // 从持久化数据恢复一个 Bot（含历史仓位/均价），跟 add_bot 不同，不会清零持仓状态；
    // 用于 App 重启后把本地跟踪的均价/持仓量对齐回重启前的状态，避免和交易所实际仓位脱节
    std::string restore_bot(CcgBot snapshot);
    // 修改一个已存在 bot 的可调策略参数（品种/方向不变），保留持仓/加仓记录等运行时状态；
    // 用于策略配置弹窗里"编辑正在运行的 bot"。杠杆只有在下一次从零开仓（level==0）时才会
    // 真正下发给交易所，如果 bot 已有持仓，改杠杆不会立刻对交易所生效
    bool update_bot_cfg(const std::string& bot_id, const CcgConfig& new_cfg);
    void        stop_bot  (const std::string& bot_id);   // 暂停监控/交易，保留当前持仓跟踪状态
    void        resume_bot(const std::string& bot_id);   // 恢复监控，从暂停前的状态继续，不清空持仓
    void        close_bot (const std::string& bot_id);  // 立即市价平仓（手动触发，非策略止盈/止损）
    void        remove_bot(const std::string& bot_id);
    void        stop_all  ();
    void        remove_all();
    std::vector<CcgBot> get_bots() const;

    void set_log_cb(LogCb cb);
    void set_trade_cb(TradeCb cb);

    // 账户级总保证金上限（所有 bot 加起来），0=不限。超过时暂缓开新的首仓，
    // 已有仓位的加仓/止盈止损不受影响——防止同时配置太多品种时风险失控
    void   set_max_total_margin(double usdt);
    double max_total_margin() const;
    double total_margin_used() const;   // 当前所有 bot 已用保证金合计

    // 由 UI 定时器每 tick 调用（传入最新价格）
    void tick(const std::string& symbol, double price);

    // 写入指标信号快照（UI 异步拉取 BOLL/RSI 后回调，仅用于 entry_mode==Indicator 的首单判定）
    void update_indicator(const std::string& bot_id, double boll_lb, double boll_ub, double rsi);

    // ── 工具 ──────────────────────────────────────────────────────────────────
    static std::vector<double> entry_usdt(const CcgConfig& cfg);  // 各层 USDT 分配
    static std::string         strat_name(CcgConfig::StratType t);
    static std::string         dir_name  (CcgConfig::Direction d);

private:
    // 本 tick 实际生效的策略参数：静态模式=配置里的固定值；动态W模式=由实时带宽推导。
    // fresh=false 表示指标数据过期（动态模式下会冻结新补仓/止盈激活）
    struct EffParams {
        double interval_pct;
        double trail_entry;
        double trail_tp;
        bool   dyn;     // 动态W模式已启用且指标数据可用
        bool   fresh;   // 指标数据是否在有效期内
    };
    EffParams eff_params(const CcgBot& bot) const;

    void update_tracking  (CcgBot& bot, double price);
    bool should_enter     (const CcgBot& bot, double price) const;
    bool should_close     (const CcgBot& bot, double price) const;
    bool should_stop_loss (const CcgBot& bot, double price) const;
    void submit_entry   (const std::string& bot_id);
    void submit_close   (const std::string& bot_id, const std::string& reason);
    void log            (const std::string& msg);

    std::shared_ptr<TradingClient> client_;
    std::shared_ptr<ThreadPool>    pool_;
    mutable std::recursive_mutex   mtx_;
    std::map<std::string, CcgBot>  bots_;
    LogCb                          log_cb_;
    TradeCb                        trade_cb_;
    std::atomic<int>               id_seq_{0};
    std::atomic<double>            max_total_margin_{0.0};
};

} // namespace ccbot
