#pragma once
#include "core/trend_decision.h"
#include "core/itrading_client.h"
#include "core/engine_host.h"
#include "core/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// 趋势策略引擎：把决策变成订单的执行层。
//
// v5.0.0 起是本仓库唯一的策略引擎（曾与网格 DCA 并列，那套已整体移除）。
// 决策全在 trend_decision.h（纯函数、已穷举单测），这里只负责"把决策变成订单"
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

struct TrendConfig {
    // 仓位大小的两种算法：
    //   Notional  —— 固定名义价值（budget_usdt 就是名义）
    //   RiskBased —— 固定【单笔风险】：
    //                  数量 = 单笔愿亏金额 ÷ |开仓价 − 止损线|
    //
    // ⚠ 它用的是【真实止损线】，不是 ATR。界面上曾经叫"按ATR等风险"，那是个
    //   误导性的名字——三个策略的止损线来源各不相同（海龟=极值∓k×ATR、
    //   PSAR=SAR值、裸K=摆动极值），公式只要 |开仓价 − 止损线|，与 ATR 无关。
    //
    // 为什么要有它：同一份名义价值，在止损距离 1.6% 和 5.3% 的品种上，单次止损
    // 亏的钱差 3.3 倍。固定名义 = 风险全压在高波动那几个品种上，而"铺开品种
    // 分散风险"这件事就此失效——趋势策略胜率只有 30~40%，靠的正是分散摊平。
    // 这是海龟的"单位"概念。
    enum class SizeMode { Notional, RiskBased };

    std::string symbol;
    SizeMode    size_mode   = SizeMode::Notional;
    // RiskBased 时：单笔止损愿意亏多少钱。名义上限仍由 budget_usdt 兜住——
    // 止损距离极小时反推出的数量会很荒谬，必须有帽子
    double      risk_usdt   = 0;
    double      budget_usdt = 1000;   // Notional：名义价值；RiskBased：名义上限
    int         leverage    = 3;
    std::string interval    = "4h";   // 信号K线周期
    trend::Config rule;               // 策略选择与各自的参数

    // 数据新鲜度上限（秒）：超过这个时长的K线快照不参与【开新仓】判定。
    // 已持仓的止损线不受影响（沿用最后一条有效线），见 trend::step。
    //
    // 0 = 【按周期自动推算】，见 effective_max_age_sec()。默认就是自动。
    // 填非 0 则手动覆盖
    int         signal_max_age_sec = 0;

    // 实际生效的新鲜度上限。
    //
    // 判据是"我手里这张图【落后了几根K线】"，不是固定秒数——固定值对短周期
    // 完全不成立：旧的默认 900 秒按 4h 定的，配到 1m 上就是容忍 15 根K线之前的
    // 通道，那个数没有任何意义，而且填错不报错（静默走默认）。
    // 取 2.5 根并保底 60 秒
    int effective_max_age_sec() const {
        if (signal_max_age_sec > 0) return signal_max_age_sec;
        return std::max(60, (int)(bar_seconds(interval) * 5 / 2));
    }

    // K线周期的秒数。认不出来按 4h 处理（保守的慢节奏）
    static int bar_seconds(const std::string& interval) {
        static const struct { const char* name; int sec; } kTable[] = {
            {"1m", 60}, {"3m", 180}, {"5m", 300}, {"15m", 900}, {"30m", 1800},
            {"1h", 3600}, {"2h", 7200}, {"4h", 14400}, {"6h", 21600},
            {"8h", 28800}, {"12h", 43200}, {"1d", 86400},
        };
        for (const auto& e : kTable)
            if (interval == e.name) return e.sec;
        return 14400;
    }

    // ── 交易所侧灾难止损（默认关）────────────────────────────────────────────
    // 把棘轮止损线镜像成交易所上的一张 STOP_MARKET + closePosition 单。
    //
    // 为什么趋势策略比网格DCA 更需要它：DCA 的保护是「名义 ≤ 权益 ⇒ 不可强平」，那是个
    // 【不依赖任何订单存在】的数学不变量，进程死了仓位也扛得住；而趋势策略的全部保护
    // 就是那条活在进程里的止损线。程序崩了、断电了、窗口被误关了，DCA 的仓位在扛
    // 浮亏，趋势策略的仓位是【完全裸奔且没有任何底】。
    //
    // 对趋势策略也不存在 DCA 那边"会把浮亏变成实亏、所以属于策略取向"的纠结——
    // 止损本来就是这套策略的计划内动作，镜像到交易所只是让计划在进程死后仍然执行。
    bool   use_disaster_stop = false;

    // 挂单价 = 止损线【再外扩】这么多%（多头往下、空头往上）。
    //
    // ⚠ 缓冲不能是 0。交易所用连续的标记价触发，而本地是每 3 秒采样一次——挂在
    //   止损线【上】的话交易所几乎总会先触发，于是主出场路径从"本地 reduceOnly
    //   平仓"变成"交易所平掉、本地靠对账才发现"，而对账发现外部平仓会【停掉 bot】。
    //   等于把一个正常的止损出场变成一次需要人工介入的事件。
    //   留出缓冲之后：正常情况本地先平（并撤掉这张单），只有进程真的不在了
    //   价格才会继续走到这张单上。
    double disaster_stop_buffer_pct = 1.0;
};

struct TrendBot {
    enum class State { Running, Stopped };

    std::string bot_id;
    TrendConfig   cfg;
    trend::State  st;
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
    // ── K线快照（裸K 与 抛物线SAR 用；海龟只用上面的 ATR + 通道）──────────
    bool    bar_ok      = false;
    double  bar_open    = 0;    // 当前这根【未收盘】的开盘价（裸K·立即顺势用）
    double  prev_close  = 0;    // 刚收盘那根的收盘价（裸K·等收盘突破用）
    double  prev_high   = 0;    // 上一根的最高价
    double  prev_low    = 0;    // 上一根的最低价
    double  swing_low   = 0;    // bar 0 之前 N 根的最低价
    double  swing_high  = 0;
    // 抛物线SAR 的夹逼要前两根的高低点
    double  psar_prev_high = 0, psar_prev_low = 0;
    double  psar_prev2_high = 0, psar_prev2_low = 0;
    std::chrono::steady_clock::time_point sig_time{};
    int64_t bar_open_ms = 0;       // 最新信号快照所属K线的开盘时间
    // tick 里已经把上面那根算进冷却的K线。两者分开是因为 update_signal 与 tick
    // 在不同线程按不同节奏跑：只有 tick 能安全地递减冷却，而它必须知道
    // "这根K线我数过没有"——否则同一根K线内的每个 tick 都会扣一次
    int64_t last_counted_bar_ms = 0;

    // ── 交易所侧硬止损（"保命单"）的运行时状态 ──────────────────────────────
    // 语义是【开仓时挂一次、此后不动】，不再镜像移动止损。
    //
    // disaster_stop_price 在开仓成交那一刻就被冻结成目标触发价（初始移动止损线
    // 再外扩 buffer），之后的重试都用这个值——重试跨越二十多秒，期间止损线可能
    // 已经棘轮走了，跟着走就不叫"固定"了。它先于 disaster_stop_id 被写入，
    // 所以"有价无单号"就是【还没挂上】的状态，重启后据此继续重试
    std::string disaster_stop_id;
    double      disaster_stop_price = 0;
    // 同一 bot 的挂单串行化：没有它会并发派发多次，第二张被拒的同时把第一张的
    // 单号误清掉
    bool        ds_syncing = false;

    // 本仓位已尝试挂单的次数。阶梯见 TrendEngine::kDs* 常量
    int  ds_attempts = 0;
    // 已经告过"此仓位无交易所侧保护"的警，界面据此标红。挂上之后清掉
    bool ds_unprotected = false;
    // 下次允许重试的时刻（退避）。用单调钟，回放时由 EngineHost 注入
    std::chrono::steady_clock::time_point ds_next_try{};
    // 连续【因为挂不上硬止损而被迫平仓】的次数。熔断用。
    // ⚠ 它是唯一一个【不随仓位关闭复位】的字段：跨仓位累计才能看出
    //   "这是系统性问题"。只由一次成功挂单清零
    int  ds_fail_closes = 0;

    // "算不出止损线/数量不足，跳过开仓"是否已经报过。信号成立而数据没跟上时
    // 这条路径每个 tick（3 秒）都会走一遍，不去重就是每 3 秒一条同样的日志。
    // 成功开仓时复位，所以"又开始缺数据了"仍然会重新提示一次
    bool skip_logged = false;

    double realized_pnl = 0;
    int    trade_count  = 0;
    int    win_count    = 0;

    std::chrono::system_clock::time_point start_time{};
    std::string last_action;
    std::string last_decision;
};

// 成交记录。本版起是【唯一】的成交记录类型——网格DCA 的 TradeRecord 随它一起
// 移除了。当初刻意不复用那个是因为它带 layers（层数），那是 DCA 的概念，
// 趋势策略里没有层，混用会让统计口径悄悄串味
struct TrendTrade {
    std::string symbol;
    trend::Pos    side = trend::Pos::Flat;   // 本笔的方向
    double      entry_price = 0;
    double      exit_price  = 0;
    double      qty         = 0;
    double      pnl         = 0;
    std::string reason;
    bool        reversed    = false;     // 本次平仓后是否反手了
    std::chrono::system_clock::time_point close_time{};
};

class TrendEngine {
public:
    using LogCb   = std::function<void(const std::string&)>;
    using TradeCb = std::function<void(const TrendTrade&)>;

    TrendEngine(std::shared_ptr<ITradingClient> client,
              std::shared_ptr<ThreadPool>     pool);
    // 测试/回测构造：注入内联执行器与虚拟时钟
    TrendEngine(std::shared_ptr<ITradingClient> client, EngineHost host);

    std::string add_bot(const TrendConfig& cfg);   // 同品种已存在则返回空串
    // 从落盘快照恢复：cfg 用传入的最新配置，仓位/止损线/统计用快照里的值。
    // 与 add_bot 的区别是它【不清零持仓状态】——重启后本地跟踪必须对齐回
    // 重启前，否则引擎以为自己空仓，看到信号会再开一笔，变成双倍敞口
    std::string restore_bot(TrendBot snapshot);
    void stop_bot  (const std::string& bot_id);
    void resume_bot(const std::string& bot_id);
    void close_bot (const std::string& bot_id);  // 手动市价平仓
    void remove_bot(const std::string& bot_id);
    void stop_all();
    std::vector<TrendBot> get_bots() const;

    void set_log_cb(LogCb cb)     { log_cb_   = std::move(cb); }
    void set_trade_cb(TradeCb cb) { trade_cb_ = std::move(cb); }

    // ── 账户级闸门（0 = 不限）──────────────────────────────────────────────────
    // 与每个 bot 的 budget_usdt 是【两道不同的闸】：后者管"单个品种投多少"，
    // 这两道管"全部品种加起来"。
    //
    // 为什么趋势策略比网格更需要它：铺开多个品种是这套策略有效的前提（单个品种
    // 的胜率本来就低，靠分散摊平），于是 N 个品种 × 各自的 budget 很容易就超过
    // 账户权益。而每笔都带止损这件事只保证【单笔】亏损有界，不保证同时被打的
    // 十笔加起来有界——2026 年那种全市场同步下跌的日子里，所有品种会在同一个
    // 小时内一起触发止损。
    //
    // 判定用【保证金】口径（名义 ÷ 杠杆），与界面上"账户总保证金上限"同名同义。
    // 新仓的占用按 budget_usdt ÷ leverage 估——等风险模式下实际名义可能更小，
    // 所以这是保守估计：宁可早拦一点，不要漏拦
    void set_max_total_margin(double usdt);
    void set_max_open_positions(int n);

    // 应用层拉到K线后喂入信号快照（ATR + 唐奇安通道 + 当前K线开盘时间）
    void update_signal(const std::string& bot_id, double atr, double atr_pct,
                       bool dc_ok, double dc_up, double dc_dn, int64_t bar_open_ms);
    // K线快照（裸K 与 抛物线SAR 用）。与 update_signal 分开：海龟要的是
    // ATR+通道，这两个要的是原始高低点，合成一个大函数会让调用方被迫为
    // 用不到的参数填占位值。
    //
    // ⚠ 这个函数【自己设 sig_ok】。裸K 与 PSAR 都不需要 ATR，所以不能指望
    //   update_signal 来设——v5.1.0 之前 GUI 必须对裸K成对调用两次，
    //   漏一次的表现是"配置正常、日志正常、一单不开"，我写压力测试时踩过
    struct BarSnap {
        double open = 0;        // 当前这根（未收盘）的开盘价
        double prev_close = 0;  // 刚收盘那根的收盘价
        double prev_high = 0, prev_low = 0;        // 上一根
        double prev2_high = 0, prev2_low = 0;      // 上上根（PSAR 夹逼用）
        double swing_low = 0, swing_high = 0;      // bar 0 之前 N 根的极值
        int64_t bar_open_ms = 0;
    };
    void update_bars(const std::string& bot_id, const BarSnap& s);

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
    // 判定规则（比 DCA 版保守，因为趋势策略没有"层"的概念可供收敛）：
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

public:
    // ── 硬止损重试阶梯（公开是为了让测试能按这些数构造用例，不是给外部调的）──
    //
    // 为什么分三段而不是等间隔重试：失败原因的分布不是均匀的。头几次多半是
    // 一次网络抖动，快重试就能过；到第四次还不行就说明不是抖动，密集重试只是
    // 白烧限流额度，该退避。
    //
    // 总耗时 ≈ 0.5×3 + 2×7 + 3×3 ≈ 25 秒。这 25 秒里仓位【没有进程外保护】，
    // 但本地移动止损照常工作——所以这是"进程活着时的降级"，不是裸奔。
    // 时间预算的两头：太短则同一个原因连败十三次、白重试；太长则真出事时
    // 裸的时间过久
    static constexpr int kDsFastTries   = 3;    // 第 1..3 次：0.5 秒间隔
    static constexpr int kDsAlertAt     = 3;    // 第 3 次失败：先告一次警（还在重试）
    static constexpr int kDsBackoffEnd  = 10;   // 第 4..10 次：2 秒间隔
    static constexpr int kDsMaxTries    = 13;   // 第 11..13 次：3 秒间隔，之后放弃
    static constexpr int kDsFastMs      = 500;
    static constexpr int kDsBackoffMs   = 2000;
    static constexpr int kDsFinalMs     = 3000;
    // 连续几次"因挂不上而平仓"就停掉这个 bot。
    // 没有这道熔断的话，持续性故障（账户受限、品种不支持 closePosition、参数有
    // 系统性错误）会变成 开仓→挂不上→平仓→等信号→开仓→… 的循环，
    // 每一轮付两次手续费，而且单边行情里信号可能每根K线都来
    static constexpr int kDsCircuitBreak = 2;
    // 账户级熔断：累计几次"因挂不上而平仓"就【全局】停止开新仓。
    //
    // ⚠ 为什么 per-bot 的熔断不够：挂不上硬止损的原因基本都是系统性的
    //   （账户受限、网络到不了交易所、条件单端点不对、精度规则变了），
    //   而系统性故障对每个品种一视同仁。只有 per-bot 熔断的话，N 个 bot 会
    //   各自烧满 kDsCircuitBreak 轮"开仓→挂不上→平仓"才停下——9 个 bot 就是
    //   18 轮、36 笔市价单的手续费，而第一轮结束时其实就已经能断定了。
    //   实测日志（2026-09-29，代理把币安域名劫持到 198.18/15 的 fake-IP）里，
    //   硬止损收到的是一张 HTML 错误页，每个 bot 都在重复同一套 13 次重试
    static constexpr int kDsAccountBreak = 2;
    // 兜底平仓后至少冷却几根K线再考虑开新仓。
    //
    // ⚠ 没有它的话，裸K·盘中即时 会在【同一根K线】里立刻重开：实测日志里
    //   22:16:28 兜底平仓、22:16:31 就又开了一笔，间隔 3 秒。兜底平仓不是策略
    //   出场，它的含义是"开仓前提不成立"——那个前提在 3 秒后不会变成立
    static constexpr int kDsAbandonCooldownBars = 1;

    // 下一次重试该等多久（毫秒）。attempts = 已经失败过的次数
    static int ds_backoff_ms(int attempts) {
        if (attempts < kDsFastTries)  return kDsFastMs;
        if (attempts < kDsBackoffEnd) return kDsBackoffMs;
        return kDsFinalMs;
    }

private:
    // ── 交易所侧硬止损的挂/撤（在线程池里跑，内部不持 mtx_ 做 HTTP）──────────
    // 语义是【开仓成交后挂一次、此后不动】。v5.0.1 之前它镜像移动止损，
    // 线每移动超 0.5% 就撤旧挂新——那样每次重挂都有一个"撤了还没挂上"的空窗，
    // 而这张单的职责恰恰是"进程死了兜住"，空窗期正是最不该有的
    void try_place_hard_stop(const std::string& bot_id);
    void cancel_disaster_stop(const std::string& bot_id);
    // 挂满 kDsMaxTries 仍失败的兜底：平掉这一仓，并累计熔断计数
    void abandon_and_close(const std::string& bot_id, const std::string& why);
    // 仓位归零时复位硬止损的运行时状态。
    // ⚠ 不碰 ds_fail_closes —— 它要跨仓位累计才看得出系统性故障
    static void clear_ds_runtime(TrendBot& b);

    // 平仓（可选紧接着反手）。两笔单在同一任务里顺序执行，全程持 pending
    void submit_close(const std::string& bot_id, const std::string& reason,
                      trend::Pos reverse_to);
    void submit_open (const std::string& bot_id, trend::Pos dir, bool from_reverse);
    void clear_pending_after_throw(const std::string& bot_id, const std::string& what);
    void log(const std::string& msg) const;

    // 计算下单数量。Notional 用固定名义；RiskBased 用【真实止损距离】反推
    // 并受 budget 封顶。
    // ⚠ 用 stop_price 而不是 k×ATR：裸K线模式的止损是摆动低点，和 ATR 无关。
    //   拿 ATR 去算那个模式的仓位，算出来的"单次愿亏"是假的
    // stop_price<=0 或与 price 重合时无法计算，返回 0（调用方跳过下单）
    double plan_qty(const TrendConfig& cfg, double price, double stop_price) const;
    // 账户级闸门：空串=放行，否则是拦截原因。⚠ 调用方须已持 mtx_
    std::string open_gate_block(const TrendBot& self) const;

    std::shared_ptr<ITradingClient> client_;
    std::shared_ptr<ThreadPool>     pool_;
    EngineHost                      host_;
    mutable std::recursive_mutex    mtx_;
    std::map<std::string, TrendBot>   bots_;
    LogCb                           log_cb_;
    TradeCb                         trade_cb_;
    int                             seq_ = 0;
    double                          max_total_margin_   = 0;   // 0=不限
    int                             max_open_positions_ = 0;   // 0=不限
    // 账户级硬止损熔断：累计的兜底平仓次数，以及是否已经全局停止开新仓。
    // 一次成功挂单会把计数清零——那说明故障已经过去了
    int                             ds_abandons_total_  = 0;
    bool                            ds_account_broken_  = false;
    // 已经因为账户级闸门报过一次的 bot。闸门在每个 tick 都会命中，不去重的话
    // 就是每 3 秒一条同样的日志——本项目踩过这个（150 行日志里 120 行是噪音）。
    // 拦截原因始终写进 last_decision，界面上一直看得到，不依赖日志
    std::set<std::string>           cap_logged_;
};

} // namespace ccbot
