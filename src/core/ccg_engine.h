#pragma once
#include "core/itrading_client.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <chrono>
#include <memory>
#include <atomic>

namespace ccbot {

class ThreadPool;

// 引擎的外部时间源与执行器（回测注入虚拟实现，实盘用默认真实实现）。
// 时钟必须可注入：回放一年数据只需几秒，用真实时钟的话冷却永远走不完、
// 指标新鲜度检查永远通过，回测结果完全失真
struct EngineHost {
    // 墙钟（冷却计时、成交时间戳）
    std::function<std::chrono::system_clock::time_point()> now_wall =
        []{ return std::chrono::system_clock::now(); };
    // 单调时钟（指标/趋势/结构数据的新鲜度检查）
    std::function<std::chrono::steady_clock::time_point()> now_steady =
        []{ return std::chrono::steady_clock::now(); };
    // 异步执行器（实盘=线程池；回测=内联同步，保证确定性可复现）
    std::function<void(std::function<void()>)> submit;
};

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

    // 趋势状态机（v2.5+）：高周期（默认4h EMA200 + 中轨斜率）判定空头态时
    //   1) 暂停开新首仓（网格最怕的单边下跌里不接飞刀）
    //   2) 已有仓位的补仓间隔自动放大 1.5 倍（子弹省着打）
    // 趋势数据缺失/过期时不拦（fail-open）：这是增强过滤，不该因为断数据把交易卡死
    bool        use_trend_filter = false;
    std::string trend_interval   = "4h";
    int         trend_ema_period = 200;

    // SR雷达（v2.6+，影子模式）：自动检测支撑/阻力区域（摆动点聚类+FVG），
    // 价格触区时告警+记录，【不参与下单决策】——先积累"程序的眼睛"的准确率数据，
    // 执行接线是后续阶段的事。检测计算在应用层（GUI/headless），引擎只存配置
    bool        sr_radar    = false;
    std::string sr_interval = "4h";

    // ── v3.0 三层决策（宏观%B + 结构定位）────────────────────────────────────
    // smart_gates=false（默认）：纯影子——首仓派发时记录三层判定快照，不拦截，
    // 行为与 v2.9.x 完全一致；true：新增两层开始真实拦截（趋势/指标层沿用原开关）
    // ⚠ 下面两个自由参数的默认值由【回测实证】确定，不是推理值：
    //   数据：BTC/ETH/SOL/BNB 2025-01~2026-08 的 1m 全量，walk-forward 切4段验证
    //   净空比 2.5/3.0/4.0 均 4/4 段正收益（参数高原），而 1.5/2.0 在大跌段(S3)
    //     严重亏损（-0.37）→ 门槛的分水岭在 2.0 与 2.5 之间，取高原中心 3.0
    //   %B 阈值 0.60 为 4/4 段，0.80 仅 2/4 段（拦得太松≈没拦，且偶发误拦有害）
    bool        smart_gates        = false;
    bool        use_htf_filter     = true;    // 宏观：日线%B过滤（smart_gates开启后参与）
    std::string htf_interval       = "1d";
    double      htf_pos_max        = 0.60;    // 自由参数①：%B高于此值拦新首仓（做多）
    bool        use_sr_gate        = true;    // 结构：支撑质量+净空检查
    int         sr_min_confluence  = 2;       // 够格区域的最低共振数
    double      sr_headroom_ratio  = 3.0;     // 自由参数②：净空÷止盈距离下限
    bool        use_sr_exit        = false;   // 止盈锚定阻力区（独立开关，默认关）
    bool        use_structural_stop = false;  // 结构性止损（独立开关，默认关，仅动态W模式）
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

    // 趋势状态机快照（use_trend_filter 时由外层定期拉取写入）。
    // 消抖：原始判定连续2次相同才切换状态（价格骑在EMA200边界时单次翻面不算数）
    bool   trend_bearish = false;
    bool   trend_raw_last = false;   // 上一次原始判定
    int    trend_raw_streak = 0;     // 原始判定连续相同次数
    std::chrono::steady_clock::time_point trend_time{};

    // ── v3.0 结构快照（应用层喂入）────────────────────────────────────────────
    // 宏观：日线%B
    bool   htf_ok    = false;
    double htf_pct_b = 0.5;
    std::chrono::steady_clock::time_point htf_time{};
    // 结构：相对现价的区域摘要（应用层用 decision::digest_zones 算好推进来）
    bool   sr_ok       = false;
    bool   sr_at_support = false;
    double sr_sup_hi   = 0;     // 下方最近够格支撑区上沿（0=无；空头净空计算用）
    double sr_res_lo   = 0;     // 上方最近够格阻力区下沿（0=无）
    double sr_stop_level = 0;   // 结构止损参考位（最深支撑下沿-0.25×ATR，0=无）
    std::chrono::steady_clock::time_point sr_time{};
    // 结构止损：持续跌破计数（插针防护——连续N个tick才触发）
    int    struct_stop_ticks = 0;
    // 止盈锚（激活追踪那一刻锁定，防止区域重算把锚抽走）
    double tp_anchor = 0;

    // 在途首仓预占的保证金：首仓已派发但还没成交入账的窗口里，总保证金上限
    // 检查要把它计入，否则多品种在同一 tick 窗口齐过闸会集体超限
    double inflight_margin = 0;

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
    int                    layers       = 0;   // 本轮用了几层（周期统计：层数分布）
    std::string            reason;   // "追踪止盈" / "硬止损" / "手动平仓" ...
    std::chrono::system_clock::time_point close_time;
};

// ─── CCG 引擎 ─────────────────────────────────────────────────────────────────
class CcgEngine {
public:
    using LogCb   = std::function<void(const std::string&)>;
    using TradeCb = std::function<void(const TradeRecord&)>;

    // 实盘构造：客户端 + 线程池（内部包成 EngineHost）
    CcgEngine(std::shared_ptr<ITradingClient> client,
              std::shared_ptr<ThreadPool>     pool);
    // 回测构造：注入虚拟时钟与内联执行器
    CcgEngine(std::shared_ptr<ITradingClient> client, EngineHost host);

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

    // ── 与交易所对账（v2.4）────────────────────────────────────────────────────
    // 交易所实际持仓的精简视图（由应用层从 TradingClient::fetch_positions 转换）
    struct ExchangePos {
        std::string symbol;
        int         direction   = 0;   // 1=多 -1=空
        double      qty         = 0;   // 绝对值
        double      entry_price = 0;   // 交易所侧开仓均价（孤儿仓认领时用）
    };
    // 连接成功/重启恢复后调用一次，把本地跟踪的仓位和交易所实际持仓核对：
    //   本地有仓、交易所没有   → 外部已平仓：清空本地仓位并停止该bot（等人工确认，不自动重开）
    //   本地qty > 交易所qty    → 外部部分平仓：本地数量收敛到交易所值（均价保留）
    //   本地qty < 交易所qty    → 交易所多出（外部手动加仓）：仅告警不动本地状态
    // 同一品种有多个持仓bot时无法归属，跳过并告警。返回每条不一致的可读描述（空=完全一致）
    std::vector<std::string> reconcile_positions(const std::vector<ExchangePos>& exchange);

    // 写入指标信号快照（UI 异步拉取 BOLL/RSI 后回调，仅用于 entry_mode==Indicator 的首单判定）
    void update_indicator(const std::string& bot_id, double boll_lb, double boll_ub, double rsi);

    // 写入趋势状态机快照（use_trend_filter 的 bot 由外层每几分钟拉取一次高周期趋势后回调）
    void update_trend(const std::string& bot_id, bool bearish);

    // ── v3.0 结构数据写入（应用层喂入，同指标/趋势的快照模式）────────────────
    void update_htf(const std::string& bot_id, double pct_b);
    void update_sr_structure(const std::string& bot_id, bool at_support,
                             double sup_hi, double res_lo, double stop_level);

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

    std::shared_ptr<ITradingClient> client_;
    std::shared_ptr<ThreadPool>     pool_;
    EngineHost                      host_;
    mutable std::recursive_mutex   mtx_;
    std::map<std::string, CcgBot>  bots_;
    LogCb                          log_cb_;
    TradeCb                        trade_cb_;
    std::atomic<int>               id_seq_{0};
    std::atomic<double>            max_total_margin_{0.0};
};

} // namespace ccbot
