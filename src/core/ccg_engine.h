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
#include <array>

namespace ccbot {

// 加仓层数上限。平推/递增在大层数下有实际意义（把跌幅摊到更细的格子里）；
// 指数型曲线在深层会让首仓预算趋近于0，不建议超过 10 层
inline constexpr int kMaxLayers = 50;

// 指标数据的有效期。超过这个时长视为过期：冻结新补仓/止盈激活/首仓信号判定。
// 放头文件是为了让 GUI 的状态展示与引擎的判定用【同一个值】——两边各写一个
// 180 的话，界面会说"数据新鲜"而引擎其实已经冻结了
inline constexpr auto kIndStale = std::chrono::seconds(180);

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

    // ⚠ 默认值 = 回测实证最优组合（10个主流币 2021-01~2026-08 全量1m，连续回放）：
    //   递增曲线 / 7层 / 保底利润3.5% / 指标首单 RSI25-30反转确认 / 动态W /
    //   趋势过滤 / SR雷达 / 三层拦截(%B0.60 + 净空3.0)
    //   实证依据见 docs/BACKTEST.md。这些只影响【新建】bot，已有 bot 从配置文件恢复
    std::string symbol;
    StratType   strat_type   = StratType::Linear;   // 递增：均价压得动且深层资金不爆炸
    Direction   direction    = Direction::Long;
    double      budget_usdt  = 3000.0;   // 预算资金（每个方向）
    // 8层：六年连续口径下与保底2.0%配套的最优组合。层数本身不是稳健维度
    // （三次测试排序三次翻转），但它与保底利润强耦合——7层配2.0%是四年测试里
    // 最差的组合(0.483)，必须成对使用
    int         max_entries  = 8;
    int         leverage     = 3;
    double      interval_pct = 8.0;      // 间隔比例 %（动态W模式下不生效）
    double      trail_entry  = 1.0;      // 追踪建仓 %（动态W模式下不生效）
    double      tp_pct       = 5.0;      // 整体止盈 %（动态W模式下不生效）
    double      trail_tp     = 2.0;      // 追踪止盈 %（动态W模式下不生效）
    bool        auto_restart  = true;
    int         cooldown_secs = 300;     // 回测用值；60秒在高位区会立刻回补
    double      stop_loss_pct = 0.0;   // 均价跌幅超过此值强制平仓（0=禁用）

    // ── 交易所侧灾难止损（默认关）────────────────────────────────────────────
    // 均价下方 x% 处在【交易所】挂一张 STOP_MARKET + closePosition 单。
    // 与 stop_loss_pct 的区别是决定性的：stop_loss_pct 活在本进程里，程序崩了、
    // 断电了、窗口被误关了就什么都不剩；这一张单子挂在币安服务器上，进程死了
    // 它还在。这是唯一的进程外保护。
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

    // 动态W模式（v2.3+）：把整个策略的锚点从"固定百分比"换成"布林带本身"——
    //   补仓锚定下轨：除了跌够动态间隔，价格还必须在带外（统计超卖位）才补
    //   止盈锚定上轨：价格触及上轨 且 盈利>=保底利润 才激活追踪止盈（不再用固定tp_pct）
    //   间距自适应：间隔/追踪参数由实时带宽W推导（见 dynamic_params.h），不再用配置里的固定值
    // 带子在下跌趋势中整体下移时梯子跟着走，均价贴着下轨，带内震荡就能完成多头周期。
    // 指标数据超时（>180s无更新）时冻结新补仓/止盈激活，宁可错过不可乱做。
    bool   dynamic_band_mode = true;
    // ⚠ v3.3.0 由 3.5% 改为 2.0%——旧值是 4段 walk-forward 定的，而分段会在段边界
    // 把持仓一笔勾销，恰好掩盖了"库存出不来"这个本策略的核心风险（同组参数
    // 4段汇总 +2529 vs 连续 -594）。改用六年连续口径重测后结论反转。
    // 实证（收益÷回撤，连续口径）：
    //   10品种 2021-2025四年   2.0%/8层 0.886  vs  3.5%/7层 0.674
    //   前10品种 2025-2026     2.0%/8层 3.149  vs  3.5%/7层 -0.163
    //   11-20档品种(外样本)     2.0% 为峰值(+784均值)，3.5% 为 -781
    //   细网格 1.25~2.75       单峰，峰值 2.0，2.5 起回撤跳崖(646→1308→2641)
    // 2.0% 不是更赚钱的参数（3.5% 在四年里多赚 900U），是回撤小得多的参数
    // （5052 vs 8006）。取 2.0% 的决定性理由：3.5% 制造的回撤相当于 80% 本金，
    // 而回测【不模拟强平】，现实中那 900U 大概率拿不到。
    // 分年度看是一份保险：牛市/横盘年 3.5% 每年多赚 10~60%，唯独 2022 全年阴跌
    // 里 3.5% 亏损翻倍（-3340 vs -1621）
    double min_profit_floor  = 2.0;

    // 补仓间隔倍数（1.0=原始设计 W/3，夹逼[0.3,1.5]）。放大它等于把梯子摊到
    // 更宽的价格带上——见 dynparams::interval_pct 的说明。
    // 实测（BTC六年，8层/保底2.0%/三层拦截）：满层时间占比与回撤确实随倍数
    // 单调下降（27.4%→2.0%，回撤 4557→1453），但利润塌得更快（2430→141），
    // 收益/回撤 0.53→0.10。原因是放宽间隔的本质是让补仓很少触发＝把仓位做小，
    // 而且不是等比缩小：敞口降到约1/4、利润却只剩1/17——网格赚的是小幅震荡的
    // 钱，间隔一宽这类周期整个漏掉。
    // 想降回撤应该【降预算】而不是放宽间隔：间隔×1.0+预算33% 得到相同回撤
    // (1519 vs 1453) 但利润是 5.7 倍 (810 vs 141)。默认保持 1.0
    double dyn_interval_mult = 1.0;

    // ── 多周期梯子（v3.7 实验，默认关）────────────────────────────────────────
    // 把补仓间距的来源从"一个参数"换成"市场结构本身"：梯子是 N 个有序槽位，
    // 槽位 i 归属某一档，填第 i 槽必须先跌破【该档周期】的布林下轨。
    // 四档固定为 1h / 4h / 12h / 1d——带宽大致按 √T 缩放，所以间距天然递增，
    // 越深的层需要越极端的证据才解锁（"跌破日线下轨"是统计上罕见的事件）。
    //
    // 动机：固定间隔在浅回调里也会一路补下去，导致弹药在到底之前就打光
    // （实测所有品种都打满层）。多周期档位让浅回调只消耗第一档。
    //
    // ⚠ 只接管【补仓间距的推导】，止盈那半（触上轨+保底利润）完全不动。
    bool        mtf_ladder = false;
    // 各档层数，如 "3,2,2,1"。空=按 3:2:2:1 权重自动分配到 max_entries；
    // 层数不足 4 时从最深的档往前砍（N=3 → 2/1/0/0，只用 1h 和 4h）
    std::string mtf_tier_layers = "";
    // 最小间距 = max(mtf_k × 该档带宽, mtf_min_gap_pct)。
    // 前者自适应：瀑布本身是高波动事件，带子撑开时地板跟着撑开，正好挡住
    // "四档同时触发、整个梯子打在崩盘顶部"这个失效模式；
    // 后者兜底：带数据过期或异常收窄时不至于失去地板
    double      mtf_k           = 0.5;
    double      mtf_min_gap_pct = 2.0;

    // 固定补仓间隔%（0=用 W/3 推导）。存在的唯一目的是回答一个从没被干净验证过
    // 的问题：动态W的"自适应"本身有没有创造价值？
    // 现在的 dynamic_band_mode 同时控制【结构锚定】（补仓须在下轨外、止盈须触
    // 上轨）和【参数推导】（间隔/追踪由W算），关掉它两件事一起没了，所以无法
    // 区分。本开关保留全部结构锚定，只把间隔换成固定值——若最优固定值能打平
    // 动态推导，说明 W/3 只是碰巧算出了一个接近最优的常数，自适应是装饰品
    double dyn_fixed_interval = 0.0;

    // 趋势状态机（v2.5+）：高周期（默认4h EMA200 + 中轨斜率）判定空头态时
    //   1) 暂停开新首仓（网格最怕的单边下跌里不接飞刀）
    //   2) 已有仓位的补仓间隔自动放大 1.5 倍（子弹省着打）
    // 趋势数据缺失/过期时不拦（fail-open）：这是增强过滤，不该因为断数据把交易卡死
    bool        use_trend_filter = true;
    std::string trend_interval   = "4h";
    int         trend_ema_period = 200;

    // SR雷达：自动检测支撑/阻力区域（摆动点聚类 + 攻防转换 + FVG + POC + 斐波）。
    // 它是三层决策【结构层】的数据源，不是一个独立功能——GUI 不再暴露开关，
    // 由 smart_gates / use_sr_exit / use_structural_stop 任一开启时自动带上。
    // 检测计算在应用层（GUI/headless），引擎只存配置
    bool        sr_radar    = true;
    std::string sr_interval = "4h";

    // ── v3.0 三层决策（宏观%B + 结构定位）────────────────────────────────────
    // v3.8 起三层拦截拆成三个【平级独立开关】，不再有 smart_gates 总开关：
    //   高位 use_htf_filter    —— 日线%B 过高不追（做多）
    //   支撑 use_sr_support    —— 价格须正踩在够格支撑区【内部】
    //   净空 use_sr_headroom   —— 头顶到最近够格阻力的空间 ÷ 止盈距离
    // 三个都关 = 三层决策完全不参与。拆开的价值在于可归因：此前它们绑在一起，
    // "三层收紧只比不拦好12%"这个结论无法判断是哪一条贡献的。
    // 预期支撑是最紧的一条（要求价格正在区域内部，而区域厚度通常仅 0.5×ATR，
    // 价格大部分时间在区域之间的空隙里），净空则很少拦。
    // 影子模式（判定但不拦、只写快照）已于 v3.6 移除。
    // ⚠ 下面两个自由参数的默认值由【回测实证】确定，不是推理值：
    //   数据：BTC/ETH/SOL/BNB 2025-01~2026-08 的 1m 全量，walk-forward 切4段验证
    //   净空比 2.5/3.0/4.0 均 4/4 段正收益（参数高原），而 1.5/2.0 在大跌段(S3)
    //     严重亏损（-0.37）→ 门槛的分水岭在 2.0 与 2.5 之间，取高原中心 3.0
    //   %B 阈值 0.60 为 4/4 段，0.80 仅 2/4 段（拦得太松≈没拦，且偶发误拦有害）
    // 数据缺失【不放行】（strict）——闸门既然开了就该真守住
    // ⚠ 这两个阈值是 4段 walk-forward 定的，而该口径后被证明会系统性美化本策略
    //   （见 BACKTEST.md），至今未在连续口径下重验
    bool        use_htf_filter     = true;    // 宏观：日线%B过滤
    std::string htf_interval       = "1d";
    double      htf_pos_max        = 0.60;    // 自由参数①：%B高于此值拦新首仓（做多）
    bool        use_sr_support     = true;    // 结构①：脚下踩住够格支撑区
    bool        use_sr_headroom    = true;    // 结构②：头顶净空足够止盈
    int         sr_min_confluence  = 2;       // 够格区域的最低共振数
    // 阻力侧单独的计票门槛（0=同 sr_min_confluence）。实验项，仅回测可达，GUI 不暴露。
    // 动机：同门槛下两侧不对称——支撑票数不够=拦（保守），阻力票数不够=这堵墙被
    // 忽略、净空显示∞=放行（激进）。但降到 1 票实测是灾难：净空3.0 下周期从 47 掉到
    // 7、收益 1519→-17；4段 walk-forward 最好的一档(净空2.0)也只有 0.252 vs 基线 0.548。
    // 原因是够格阻力一多，净空比处处偏小，3.0 门槛几乎筛掉了全部机会——
    // 不对称是真的，但"两侧都保守"并不等于更好
    int         sr_res_min_conf    = 0;
    // 共振按【独立证据族】计票（摆动与其算术衍生的斐波归为一族）——见
    // Zone::confluence_independent() 的完整论证。理论依据：共振提升置信度的前提
    // 是证据独立，相关证据计两票等于虚报置信度；斐波是摆动极值的纯算术函数，
    // 零新增信息。实测：收益 +19%、收益/回撤 +20%、周期数不降反升（伪共振区
    // 同时也在阻力侧充当假墙，剔除后净空计算更准）
    bool        sr_independent_conf = true;
    // 价格须落在区域下半部才算踩住支撑。实测有害（周期 -28%、收益/回撤 -26%）：
    // "砸穿 vs 踩住"的区分已由追踪建仓的反弹确认承担，此处再设一道是重复设防，
    // 损失的机会大于避免的坏单。保留为可选，默认关
    bool        sr_lower_half_only  = false;
    double      sr_headroom_ratio  = 3.0;     // 自由参数②：净空÷止盈距离下限
    // 净空的分母用"上轨距离与保底线取远"而非仅"到上轨距离"。实验项，仅回测可达。
    // 动机看似成立（止盈是双条件，只用上轨距离会低估要走的路），但实测更差：
    // 19个月连续 1519→1184、收益/回撤 2.26→1.76、周期 47→40；把净空门槛一起
    // 重扫(1.0~4.0)后最好一档也只有 0.452 vs 基线 0.548。
    // 论证错在哪：保底线是 avg×(1+floor)，而 avg 会被补仓一路压低，所以真正的
    // 止盈【价格】常常低于入场价×(1+floor)。用入场价×3.5% 当分母等于假设了一个
    // 没有补仓的世界，对一个 DCA 策略来说是系统性高估。相比之下"到上轨距离"是
    // 随行情实时更新的结构目标，反而是更好的估计量。
    // 唯一可取处：它是全部正收益组合里回撤最低的一条路径（净空2.5 时回撤
    // 1434 vs 基线 1973），极度厌恶回撤时可考虑
    bool        sr_headroom_true_tp = false;
    // ── 快进快出方案（v3.3 实验）─────────────────────────────────────────────
    // 现行动态W止盈是【触上轨 且 盈利≥保底】双条件，本质是"等一个完整的带内摆动"。
    // 另一种思路是"够本就跑"：不等上轨，盈利达标即激活追踪，靠高周转取胜。
    // tp_floor_only=true 时止盈激活【只看保底利润】，不要求触上轨（=替换上轨条件）
    bool        tp_floor_only  = false;
    // 固定利润止盈线%（0=关）。与上轨路径是【或】关系，不是替换：
    //   激活 = (触上轨 且 利润≥保底利润) 或 (利润≥本值)
    // 职责与 min_profit_floor 不同——保底利润是上轨路径的下限兜底（防下跌趋势里
    // 上轨低于均价时亏着平仓）；本值是独立的逃生阈值，通常设得更高。
    // 动机：仓位若始终摸不到上轨（带子跑得比价格快或带子扩张），哪怕已浮盈
    // 6~8% 也永不平仓，只能挂着变成浮亏——这正是"滞留库存"的成因之一。
    // 它同时是【方案A 与现行机制之间的连续旋钮】：设很小≈方案A（路径B几乎总是
    // 先触发），设很大≈现行机制（路径B几乎不触发）
    double      tp_fixed_profit = 0.0;
    // 固定追踪止盈回调%（0=用动态W推导的 0.15W）。快进快出方案要把回调压到很小，
    // 不能让它随带宽放大
    double      fixed_trail_tp = 0.0;
    // 首仓追踪建仓%（0=关，保持"在下轨内直接开"的现行为）。开启后首仓改为：
    // 跌破下轨 → 记录破轨后最低点 → 自最低点反弹此比例【且价格仍在中轨下方】
    // 才开首仓。把"接飞刀"变成"等企稳"。
    // ⚠ 反弹条件必须【叠加】在超卖区之上而不是替换掉下轨条件——早期实现漏了
    //   "仍在中轨下方"这一条，导致破轨一次就永久放行（价格涨到上轨附近照样开），
    //   开仓数虚增 2.6 倍，看起来像巨大改进，实为下轨过滤器被关掉。
    // 实测（保底2.0%/8层，收益÷回撤，关闭 → 最优值）：
    //   BTC 2021-2024   0.305 → 1.340 (0.5%)      BTC 2025-2026  0.946 → 1.176 (0.2%)
    //   10品种 2021-2024 0.886 → 3.074 (1.0%)     10品种 2025-2026 3.348 → 开了就变差
    // 即：BTC 单标的上两个年代都改善（0.2% 两段都居前），但多品种上最优值在
    // 1.0%/关闭之间翻转，近期窗口甚至是"关闭"明显最优。
    // 结论：全局默认关；【只跑BTC的bot】可设 0.2
    double      first_entry_bounce_pct = 0.0;

    // 周期熊市总开关：BTC 级别的"这轮牛市结束了，全场停手"。
    // 与 use_trend_filter（4h EMA200，战术级、按品种）不是一回事——那是躲回调，
    // 这是躲整轮熊市。实证动机：2022 全年阴跌里两种保底利润都深亏（-1621/-3340），
    // 纯多DCA在周期熊市没有能赢的参数组合，唯一出路是不进场。
    // 熊市判定由应用层从 BTC 日线算好后 set_market_bearish() 喂入
    bool        use_cycle_bear_switch = false;

    // ── 补仓侧闸门（v3.3 实验）────────────────────────────────────────────────
    // 三层决策目前只管首仓，而递增7层曲线下首仓仅占预算 1/28≈3.6%，第5~7层占
    // 64.3%——96.4% 的资金走的是"跌够间隔+反弹确认"这条无宏观检查的路径，
    // 且深层必然在暴跌中触发，正是最该检查的时刻检查最少。
    // dca_gate_from_layer：从第几层起施加闸门（1-indexed，0=关闭）
    // dca_gate_trend     ：高周期空头态暂停深层补仓
    // dca_gate_htf_min   ：日线%B低于此值暂停深层补仓（0=关）。方向与首仓相反——
    //   首仓要%B低（买跌），深层补仓怕的是%B长期贴住0（价格骑着下轨走=下跌趋势
    //   而非均值回归，此时补仓是在给趋势送钱）
    // 实测结论（市值前10，2025-01~2026-08 全量1m）：9 种组合【无一改善】，
    // 基线在收益/回撤/周期三项上全胜。关键是【被拦的变体回撤反而更大】——
    // 补仓不是回撤的成因，而是回撤的【修复机制】：拦掉深层补仓，均价降不下来，
    // 止盈线（均价×1.035 与上轨取大）就更远，仓位被套得更久更深。
    // 保留为实验开关，默认关，仅回测可达
    int         dca_gate_from_layer = 0;
    bool        dca_gate_trend      = false;
    double      dca_gate_htf_min    = 0.0;

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
    // 首仓追踪建仓状态（first_entry_bounce_pct>0 时用）：
    // band_broken=本轮是否已跌破过下轨；band_extreme=破轨后的最低价
    bool   band_broken  = false;
    double band_extreme = 0;
    // ── 多周期梯子的四档布林带（mtf_ladder 时用）────────────────────────────
    // 下标 0=1h 1=4h 2=12h 3=1d。各档新鲜度阈值不同：1d 的带 20 分钟前算的
    // 完全没问题，1h 的带 20 分钟就偏旧了
    struct TfBand {
        double lb = 0, ub = 0;
        std::chrono::steady_clock::time_point t{};
    };
    TfBand mtf_band[4];

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

    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point cooldown_until;
    // last_action 同时是【日志去重键】：只有它变了才打新日志。所以它必须是稳定的
    // 短字符串，不能塞进带实时数字的详情——否则每个 tick 都不相等，日志会 3 秒刷一条
    std::string last_action;
    // 三层决策的完整快照（"%B=0.34✓ | 支撑✗无 | 净空0.42✗不足"）。每次判定都更新，
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
    void        remove_all();
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
    // 连接成功/重启恢复后调用一次，把本地跟踪的仓位和交易所实际持仓核对：
    //   本地有仓、交易所没有   → 外部已平仓：清空本地仓位并停止该bot（等人工确认，不自动重开）
    //   本地qty > 交易所qty    → 外部部分平仓：本地数量收敛到交易所值（均价保留）
    //   本地qty < 交易所qty    → 交易所多出（外部手动加仓）：仅告警不动本地状态
    // 同一品种有多个持仓bot时无法归属，跳过并告警。返回每条不一致的可读描述（空=完全一致）
    std::vector<std::string> reconcile_positions(const std::vector<ExchangePos>& exchange);

    // 写入指标信号快照（UI 异步拉取 BOLL/RSI 后回调，仅用于 entry_mode==Indicator 的首单判定）
    void update_indicator(const std::string& bot_id, double boll_lb, double boll_ub, double rsi);
    // 多周期梯子的档位带值（tier: 0=1h 1=4h 2=12h 3=1d）
    void update_mtf_band(const std::string& bot_id, int tier, double lb, double ub);
    // 各档层数分配（解析 mtf_tier_layers，空则按 3:2:2:1 权重）。公开供 GUI 预览用
    static std::array<int,4> mtf_tier_alloc(const CcgConfig& cfg);

    // 写入趋势状态机快照（use_trend_filter 的 bot 由外层每几分钟拉取一次高周期趋势后回调）
    // 账户级周期熊市标志（BTC日线驱动，全场共用一个）。与 set_max_total_margin
    // 同级：不属于任何单个bot，由应用层统一喂入
    void set_market_bearish(bool bearish);
    bool market_bearish() const { return market_bearish_.load(); }

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
    bool dca_gate_blocked (const CcgBot& bot,
                           std::chrono::steady_clock::time_point now) const;
    bool should_close     (const CcgBot& bot, double price) const;
    bool should_stop_loss (const CcgBot& bot, double price) const;
    // 交易所侧灾难止损单的挂/改/撤（在线程池里跑，内部不持 mtx_ 做 HTTP）
    void sync_disaster_stop  (const std::string& bot_id);
    void cancel_disaster_stop(const std::string& bot_id);
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
    std::atomic<int>               max_open_positions_{0};
    std::atomic<bool>   market_bearish_{false};
};

} // namespace ccbot
