#pragma once
#include "core/itrading_client.h"
#include "core/engine_host.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <chrono>
#include <memory>
#include <atomic>
#include <array>
#include <set>

namespace ccbot {

// 加仓层数上限。平推/递增在大层数下有实际意义（把跌幅摊到更细的格子里）；
// 指数型曲线在深层会让首仓预算趋近于0，不建议超过 10 层
inline constexpr int kMaxLayers = 50;

// 指标数据的有效期。超过这个时长视为过期：冻结新补仓/止盈激活/首仓信号判定。
// 放头文件是为了让 GUI 的状态展示与引擎的判定用【同一个值】——两边各写一个
// 180 的话，界面会说"数据新鲜"而引擎其实已经冻结了
inline constexpr auto kIndStale = std::chrono::seconds(180);

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

    // ⚠ 默认值 = 回测实证最优组合（10个主流币 2021-01~2026-08 全量1m，连续回放）：
    //   递增曲线 / 7层 / 保底利润3.5% / 指标首单 RSI25-30反转确认 / 动态W /
    //   趋势过滤 / 高位拦截(%B 0.60)
    //   实证依据见 docs/BACKTEST.md。这些只影响【新建】bot，已有 bot 从配置文件恢复
    std::string symbol;
    StratType   strat_type   = StratType::Linear;   // 递增：均价压得动且深层资金不爆炸
    Direction   direction    = Direction::Long;
    double      budget_usdt  = 3000.0;   // 预算资金（每个方向）
    // 10层：8品种 × 8个滚动窗口 = 64个纯样本外测试段（预热4月/训练12月/测试6月）
    //   10层  +75192  盈利窗口 61/64
    //    8层  +70620  盈利窗口 56/64
    // 两个维度同向，故取 10。此前默认 8 来自更早的六年连续口径测试。
    //
    // ⚠ 层数不是稳健维度（三次测试排序三次翻转），且与保底利润强耦合——
    //   7层配2.0% 是四年测试里最差的组合(0.483)，必须成对使用。
    //   v4.0.7 前 GUI 新建 bot 恰好预填 7、保底恰好 2.0%，正是这个最差组合；
    //   本次把引擎、GUI 预填、两处读取兜底四个地方统一到 10。
    //
    // 改的只是【新建】bot 的默认值：已存在的 bots.json 里有显式值，读回来照旧
    int         max_entries  = 10;
    int         leverage     = 3;
    // ⚠ 默认 6 而不是 8：walk-forward 实测（8品种 × 8 个滚动窗口 = 64 个纯样本外
    //   测试段）固定 6% 全面优于 5%/4%，3% 是净负的。v4.6.0 之前这个值被
    //   dyn_fixed_interval 覆盖着，实际跑的就是 6
    double      interval_pct = 6.0;      // 补仓间隔 %（相对上一笔成交价）
    double      trail_entry  = 1.0;      // 追踪建仓 %（自 dca_extreme 反弹此比例才补）
    double      tp_pct       = 5.0;      // 止盈激活 %（相对均价）
    double      trail_tp     = 2.0;      // 追踪止盈回撤 %（自 tp_extreme 回落此比例平仓）
    // ── 出场价记忆：止盈后不在原地把仓位买回来（v4.0.8）──────────────────────
    // 其余所有高位闸门都是【无记忆的相对指标】，会随时间衰减到失效：
    //   日涨幅  1 根日线后归零
    //   7日涨幅 7 根日线后归零
    //   日线%B  约 18 根日线后回落到 0.60 以下（均线爬上来了）
    // 币爆拉一波、止盈出场、然后横在高位——上面三条最终【全部放行】，机器人
    // 会在山顶重新开首仓。它们只认价格序列的形状，不认价位本身。
    //
    // 这一条是唯一的绝对参照：记住上次止盈的出场价，要求现价比它低够了才准重开。
    // 做空镜像（要求现价比出场价【高】够了）。
    //
    // 只记【追踪止盈】。硬止损/手动平仓都不记：
    //   止损出场说明判断错了，此时锁死重入等于把亏损凝固；
    //   手动平仓是用户的主动决定，不该反过来约束用户下一步
    double      reentry_drawdown_pct = 0;    // 0=关；现价须 ≤ 出场价×(1−此值%)
    // 过期：一个再也回不去的价位会把 bot 永久锁死（埋伏型策略尤其致命——
    // 蹲了几个月就为这一波，结果永远等不到回撤）。0=永不过期
    int         reentry_memory_days  = 30;
    bool        auto_restart  = true;
    int         cooldown_secs = 300;     // 回测用值；60秒在高位区会立刻回补
    // ── 交易所侧灾难止损（默认关）────────────────────────────────────────────
    // 均价下方 x% 处在【交易所】挂一张 STOP_MARKET + closePosition 单。
    // v4.6.0 移除本地硬止损之后，这是本策略【唯一】的止损，也是唯一的进程外保护：
    // 追踪止盈活在本进程里，程序崩了、断电了、窗口被误关了就什么都不剩；
    // 这一张单子挂在币安服务器上，进程死了它还在。
    //
    // 每次补仓成交后按新均价重挂（均价会随补仓下移，止损位跟着下移）。
    // 取值要**远离正常止盈区间**——它只防瀑布，不参与常规交易。网格策略天然要
    // 吃深度回撤，设太紧会在正常的补仓过程中被打掉，把浮亏变成实亏。
    // 建议：满层理论跌幅再留一段余量（比如满层跌 20% 的配置设 30~35）。
    // 用【独立开关】而不是"比例=0 即关闭"：这个功能会把浮亏变成实亏，是否启用
    // 属于策略取向而非参数调优，必须是一个明确的是/否，不能藏在一个数字里。
    // 分开之后也能关掉它而不丢失已经配好的比例。
    bool        use_disaster_stop = false;
    double      disaster_stop_pct = 30.0;   // 仅在 use_disaster_stop=true 时生效

    // RSI 确认方式：Snapshot=当前这一刻 RSI 到没到阈值就行；
    // CrossFromOversold=必须先探底跌破 rsi_oversold_th，之后再回穿 rsi_threshold 才算数
    // （更严格的"动能反转"确认，避免在强趋势下跌中过早进场）
    enum class RsiConfirmMode { Snapshot, CrossFromOversold };

    // ── 关于首仓信号的两条实测结论（避免重复投入）────────────────────────────
    //
    // ① 首仓质量【不可预测】。BTC 六年 1540 次下轨信号，其中 66.5% 不用补仓
    //    就能达到 +2% 保底（"好首仓"），33.5% 会先跌 4% 触发补仓。
    //    在信号发出那一刻可观测的全部特征里，没有一个能把 66.5% 显著推高：
    //      RSI 30~45 +1.6pp / 价在EMA200上 +1.4pp / 带宽偏大 +0.6pp
    //      RSI<30 −2.7pp / 24h跌幅>3% −2.3pp / 跌破深度>0.5% −2.2pp
    //    全部在噪音量级，而且方向一致地说明：跌得越深、越急、越超卖，后续反而越差
    //    （与日线口径的发现吻合）。这条信号已经榨干，不必再找增强特征。
    //
    // ② 由此得出补仓的真正作用：不是"判断错了的补救"，而是处理那必然存在的
    //    1/3 坏首仓。间隔取 4~6% 正因为它能让坏首仓补 1~2 层就摊出来；
    //    0.9%（动态W默认）会让同一批坏首仓直接补满 8 层套牢。
    //
    // 指标信号首单（entry_mode == Indicator 时才生效）
    EntryMode   entry_mode     = EntryMode::Indicator;   // 实证：立即开仓跳过了微观扳机层
    std::string kline_interval = "1h";
    int         boll_period    = 20;
    double      boll_mult      = 2.0;
    bool        use_rsi_filter = true;
    int         rsi_period     = 14;      // 实证：周期7太敏感、21太迟钝
    double      rsi_threshold  = 30.0;   // 多：RSI≥此值；空：RSI≤(100-此值)
    // 实证最强的一条对照：同阈值下瞬时快照 -798U vs 反转确认 +203U
    RsiConfirmMode rsi_confirm_mode = RsiConfirmMode::CrossFromOversold;
    double         rsi_oversold_th  = 25.0;  // 25/30 位于"探底甜点×窄确认带"的交汇处

    // 趋势状态机（v2.5+）：高周期（默认4h EMA200 + 中轨斜率）判定空头态时
    //   1) 暂停开新首仓（网格最怕的单边下跌里不接飞刀）
    //   2) 已有仓位的补仓间隔自动放大 1.5 倍（子弹省着打）
    // 趋势数据缺失/过期时不拦（fail-open）：这是增强过滤，不该因为断数据把交易卡死
    bool        use_trend_filter = true;
    std::string trend_interval   = "4h";
    int         trend_ema_period = 200;

    // ── 首仓的宏观许可层 ──────────────────────────────────────────────────
    // 只剩一条判据：日线%B 过高不追（做多）。关掉 = 完全不参与判定。
    // v4.6.0 移除了 24h / 近7日两条涨幅闸门——无实证依据且默认一直关着，
    // 结论与踩过的坑见 docs/NEGATIVE_RESULTS.md
    //
    // ⚠ v4.0.16 移除了原来的【结构层】（支撑拦截 / 净空拦截 / SR区域检测 /
    //   止盈锚定阻力区 / 结构性止损，约 610 行）。它需要每品种每 15 分钟拉
    //   400 根 K 线做区域检测，占用共用线程池，而实际取向上用不到。
    //   历史实证结论仍在 docs/BACKTEST.md，代码在 git 历史里可查。
    //
    // %B 阈值 0.60 的实证依据：BTC/ETH/SOL/BNB 2025-01~2026-08 的 1m 全量、
    // walk-forward 切 4 段，0.60 为 4/4 段正收益，0.80 仅 2/4 段（拦得太松≈没拦）。
    // 数据缺失【不放行】（strict）——闸门既然开了就该真守住
    bool        use_htf_filter     = true;    // 宏观：日线%B过滤
    std::string htf_interval       = "1d";
    double      htf_pos_max        = 0.60;    // 自由参数①：%B高于此值拦新首仓（做多）
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

    // 指标信号快照（entry_mode==Indicator 时，由 UI 每个 tick 异步拉取后写入）。
    // v4.6.0 移除动态W之后这些值【只服务首仓信号】——补仓和止盈不再看布林带
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
    // 宏观：日线%B ＋ 同源的滚动涨幅
    bool   htf_ok    = false;
    double htf_pct_b = 0.5;
    std::chrono::steady_clock::time_point htf_time{};
    // 在途首仓预占的保证金：首仓已派发但还没成交入账的窗口里，总保证金上限
    // 检查要把它计入，否则多品种在同一 tick 窗口齐过闸会集体超限
    double inflight_margin = 0;

    // 交易所侧灾难止损单。order_id 落盘，重启后先撤旧单再按当前均价重挂，
    // 避免遗留一张触发价对不上新均价的孤儿单
    std::string disaster_stop_id;
    double      disaster_stop_price = 0;
    // 同一个 bot 的挂单同步正在进行中。sync_disaster_stop 会被并发派发（每次成交
    // 后一次、重启 resync 又一次），而它中间要放锁去发 HTTP：两个线程读到同一个
    // 旧单号，都撤、都挂，第二张会被币安拒（同方向 closePosition 只允许一张），
    // 失败分支随即把 disaster_stop_id 清空——于是交易所上明明有单，程序却认为
    // 这个仓位没有进程外保护，并反复告警、反复重挂再被拒。
    // 不落盘：它只是一次同步的进行中标记，重启后本就该重新同步
    bool        ds_syncing = false;

    // 统计
    double realized_pnl = 0;
    int    cycle_count  = 0;

    // ── 满层时长累计（品种健康度）────────────────────────────────────────────
    // 回测里满层时间占比是【贯穿全部实验的枢纽变量】，但此前只存在于回测报告，
    // 实盘界面上看不到。60 品种实测（6% 档）：
    //   满层  0~10%  →  22/22 盈利，净利中位 +19432
    //   满层 10~25%  →  16/17 盈利
    //   满层 25~50%  →   8/11 盈利
    //   满层 75%+    →   0/6  盈利，净利中位 −16854
    // 而所有有效改进（宽间隔、趋势过滤、选币）都是通过压低它起作用的，
    // 所有失败尝试（梯度间隔、动态W、补仓闸门）都把它推高了。
    //
    // 让它在运行时可见，就有了【提前退出的依据】——而不是等亏了才发现。
    // 两个累计量都随 bot 落盘，重启后继续累加；口径是"自该 bot 创建以来"。
    int64_t full_layer_secs = 0;   // 处于满层状态的累计秒数
    int64_t alive_secs      = 0;   // 有效计时的累计秒数（分母；跳过 Stopped）
    // 上一次计时的时刻，用于算增量。不落盘：重启后从当前时刻重新起算，
    // 否则会把关机时间也算成"满层"或"存活"
    std::chrono::steady_clock::time_point last_health_tick{};

    // 满层时间占比 %（分母为 0 时返回 0）
    double full_layer_pct() const {
        return alive_secs > 0 ? 100.0 * (double)full_layer_secs / (double)alive_secs : 0.0;
    }

    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point cooldown_until;
    // 出场价记忆（只在【追踪止盈】全量平仓时写入）。0=无记忆。
    // 用 wall clock 而不是 steady：它要跨进程重启存进 bots.json，
    // steady_clock 的原点每次启动都变，存下来毫无意义
    double last_tp_price = 0;
    std::chrono::system_clock::time_point last_tp_time{};
    // last_action 同时是【日志去重键】：只有它变了才打新日志。所以它必须是稳定的
    // 短字符串，不能塞进带实时数字的详情——否则每个 tick 都不相等，日志会 3 秒刷一条
    std::string last_action;
    // 宏观许可层的完整快照（"%B=0.34✓ | 24h涨6.20%✗过热"）。每次判定都更新，
    // 不参与去重，纯供界面展示——"信号已满足却不开仓"时，答案就在这里
    std::string last_decision;
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
    std::vector<CcgBot> get_bots() const;

    void set_log_cb(LogCb cb);
    void set_trade_cb(TradeCb cb);

    // 账户级总保证金上限（所有 bot 加起来），0=不限。超过时暂缓开新的首仓，
    // 已有仓位的加仓/止盈止损不受影响——防止同时配置太多品种时风险失控
    // 重启恢复仓位后调用：重建所有交易所侧灾难止损单
    void   resync_disaster_stops();
    void   set_max_total_margin(double usdt);
    double max_total_margin() const;
    double total_margin_used() const;   // 当前所有 bot 已用保证金合计

    // ── 账户级并发持仓上限（0=不限）───────────────────────────────────────────
    // 与总保证金上限是【两道不同的闸】，全市场扫描下缺一不可：
    //   保证金上限管的是"总共投出去多少钱"，撞到了才停；
    //   并发上限管的是"同时压在几个品种上"。
    // 全市场跑超卖策略时，大跌那天可能几十个品种同时满足信号——只有保证金上限
    // 的话，钱会被最先触发的那几个吃光，而且分散度完全失控（几十个仓位同时
    // 深套，与"分散"的初衷正好相反）。
    //
    // 计数包含【在途】的首仓：不然同一 tick 窗口里多个 bot 会一起过闸，
    // 与 inflight_margin 防的是同一类竞争
    void set_max_open_positions(int n);
    int  max_open_positions() const;
    int  open_position_count() const;   // 已持仓 + 在途首仓的 bot 数

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
    // 对账模式。区别只在【要不要防在途竞态】，判定规则本身完全相同。
    enum class ReconcileMode {
        // 连接成功/重启恢复后调用。此刻没有任何在途任务，落盘状态就是全部真相，
        // 所以不做任何跳过——尤其不能按"刚成交"跳过：崩溃重启时 entries 里的
        // 时间戳可能就在几秒前，而那正是最需要对账的场景
        Startup,
        // 运行中周期调用。必须比启动模式保守：
        //   · 跳过 pending 的 bot —— 下单/平仓已在交易所生效、本地还没入账，
        //     此刻比对必然误判（正在止盈的会被当成"外部平仓"清空并停掉）
        //   · 跳过刚成交的 bot —— 持仓快照可能拍摄于成交【之前】，同样误判
        Periodic
    };
    // 把本地跟踪的仓位和交易所实际持仓核对：
    //   本地有仓、交易所没有   → 外部已平仓：清空本地仓位；auto_restart 开则进冷却
    //                            等下一轮（与止盈后同一条路径），否则停止等人工确认
    //   本地qty > 交易所qty    → 外部部分平仓：本地数量收敛到交易所值（均价保留）
    //   本地qty < 交易所qty    → 交易所多出（外部手动加仓）：仅告警不动本地状态
    // 同一品种有多个持仓bot时无法归属，跳过并告警。返回每条不一致的可读描述（空=完全一致）
    // managed_elsewhere：由【本程序的另一套引擎】（SAR）管理的品种。
    // 这些品种的交易所仓位不是"无人管理的孤儿仓"——它们有主，只是主不在这里。
    // 不传的话，SAR 开的每一笔仓都会被这里报成孤儿仓并发 webhook，每分钟一次。
    // 只影响"反向核查"那一段；本引擎自己 bot 的比对完全不受它影响
    std::vector<std::string> reconcile_positions(
        const std::vector<ExchangePos>& exchange,
        ReconcileMode mode = ReconcileMode::Startup,
        const std::set<std::string>& managed_elsewhere = {});

    // 仅测试用：直接摆布 pending 标志。
    // 存在的理由是"订单已在交易所生效、本地还没入账"这个【瞬间】无法在测试里
    // 自然复现——测试用的 host_.submit 是同步执行的，派发完 pending 立刻就被清掉了。
    // 而周期对账最凶险的误判恰恰发生在那个瞬间（正在止盈的会被当成外部平仓），
    // 所以必须能把它构造出来。生产代码永不调用
    void set_pending_for_test(const std::string& bot_id, bool v) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it != bots_.end()) it->second.pending = v;
    }

    // 写入指标信号快照（UI 异步拉取 BOLL/RSI 后回调，仅用于 entry_mode==Indicator 的首单判定）
    void update_indicator(const std::string& bot_id, double boll_lb, double boll_ub, double rsi);
    // 写入趋势状态机快照（use_trend_filter 的 bot 由外层每几分钟拉取一次高周期趋势后回调）
    void update_trend(const std::string& bot_id, bool bearish);

    // 宏观许可的日线 %B（应用层喂入，同指标/趋势的快照模式）
    void update_htf(const std::string& bot_id, double pct_b);

    // ── 工具 ──────────────────────────────────────────────────────────────────
    static std::vector<double> entry_usdt(const CcgConfig& cfg);  // 各层 USDT 分配
    static std::string         strat_name(CcgConfig::StratType t);
    static std::string         dir_name  (CcgConfig::Direction d);

private:
    void update_tracking  (CcgBot& bot, double price);
    bool should_enter     (const CcgBot& bot, double price) const;
    bool should_close     (const CcgBot& bot, double price) const;
    // 交易所侧灾难止损单的挂/改/撤（在线程池里跑，内部不持 mtx_ 做 HTTP）
    void sync_disaster_stop  (const std::string& bot_id);
    void cancel_disaster_stop(const std::string& bot_id);
    void submit_entry   (const std::string& bot_id);
    void submit_close   (const std::string& bot_id, const std::string& reason);
    // 异步下单/平仓任务抛出后的统一收尾：复位 pending 与在途保证金。
    // 不复位那个 bot 会永久冻结——所有开仓/补仓/止盈路径都以 !pending 为前提
    void clear_pending_after_throw(const std::string& bot_id, const std::string& what);
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
    std::atomic<int>               max_open_positions_{0};
};

} // namespace ccbot
