#include "core/ccg_engine.h"
#include "core/decision.h"
#include "core/dynamic_params.h"
// 引擎只依赖 ITradingClient 接口（在 ccg_engine.h 里），不再直接依赖具体的
// TradingClient/curl——单元测试因此可以只编译引擎+FakeClient，无需网络库
#include "core/thread_pool.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace ccbot {

// 指标数据有效期：超过这个时长没有新的 BOLL/RSI 写入就视为过期。
// 正常情况下 UI/headless 每 3s 拉一次，180s 的余量足够容忍短暂网络抖动，
// 又能保证断网/K线接口故障时不会拿几小时前的旧轨道值继续开仓补仓

// 趋势数据有效期：外层每 ~5 分钟拉一次 4h 级别趋势，30 分钟没更新视为过期。
// 过期时趋势过滤自动失效（fail-open）——它是增强项，不该因断数据卡死交易
static constexpr auto kTrendStale = std::chrono::minutes(30);

// 空头态时补仓间隔的放大倍数（子弹省着打）
static constexpr double kBearIntervalMult = 1.5;

// now 由调用方从 host_ 取（回测注入虚拟时钟，实盘=真实时钟）
static bool trend_active_bearish(const CcgBot& bot, std::chrono::steady_clock::time_point now) {
    return bot.cfg.use_trend_filter && bot.trend_bearish &&
           (now - bot.trend_time) < kTrendStale;
}

// ── 加仓比例序列（最多 kMaxLayers 层）────────────────────────────────────────
// 按需生成而不是查固定表：层数上限从 10 提到 50 后，写死的表会静默截断预算分配。
// ⚠ 指数型曲线（倍投/三倍/斐波/卢卡斯）在深层数下权重爆炸，首仓分到的预算会
// 趋近于 0 导致下单量不足——大层数只对平推/递增有实际意义
static std::vector<double> base_mult(CcgConfig::StratType t, int n) {
    using ST = CcgConfig::StratType;
    std::vector<double> m;
    m.reserve(n);
    switch (t) {
    case ST::Flat:
        m.assign(n, 1.0);
        break;
    case ST::Linear:
        for (int i = 1; i <= n; ++i) m.push_back(i);
        break;
    case ST::Martingale:                      // 1,1,2,2,4,4,8,8...
        for (int i = 0; i < n; ++i) m.push_back(std::pow(2.0, i / 2));
        break;
    case ST::Triple:
        for (int i = 0; i < n; ++i) m.push_back(std::pow(3.0, i));
        break;
    case ST::Square:
        for (int i = 1; i <= n; ++i) m.push_back((double)i * i);
        break;
    case ST::MartPlus:                        // 类斐波那契 1,1,2,3,5,8...
    case ST::Fibonacci: {
        double a = 1, b = 1;
        for (int i = 0; i < n; ++i) { m.push_back(a); double c = a + b; a = b; b = c; }
        break;
    }
    case ST::Lucas: {                         // 2,1,3,4,7,11...
        double a = 2, b = 1;
        for (int i = 0; i < n; ++i) { m.push_back(a); double c = a + b; a = b; b = c; }
        break;
    }
    }
    if (m.empty()) m.assign(n, 1.0);
    return m;
}

std::vector<double> CcgEngine::entry_usdt(const CcgConfig& cfg) {
    int n = std::max(1, std::min(cfg.max_entries, kMaxLayers));
    auto m = base_mult(cfg.strat_type, n);
    double total = 0;
    for (auto v : m) total += v;
    if (total <= 0) total = 1;
    std::vector<double> out;
    for (auto v : m) out.push_back(cfg.budget_usdt * v / total);
    return out;
}

std::string CcgEngine::strat_name(CcgConfig::StratType t) {
    using ST = CcgConfig::StratType;
    switch (t) {
    case ST::Flat:       return "平推";
    case ST::Martingale: return "倍投";
    case ST::MartPlus:   return "倍投Plus";
    case ST::Triple:     return "三倍";
    case ST::Square:     return "平方";
    case ST::Fibonacci:  return "斐波那契";
    case ST::Lucas:      return "卢卡斯";
    case ST::Linear:     return "递增";
    }
    return "未知";
}

std::string CcgEngine::dir_name(CcgConfig::Direction d) {
    using D = CcgConfig::Direction;
    if (d == D::Long)  return "多";
    if (d == D::Short) return "空";
    return "双向";
}

// ── 构造 ───────────────────────────────────────────────────────────────────────
CcgEngine::CcgEngine(std::shared_ptr<ITradingClient> client,
                     std::shared_ptr<ThreadPool>     pool)
    : client_(std::move(client)), pool_(std::move(pool)) {
    auto p = pool_;
    host_.submit = [p](std::function<void()> fn) { p->submit(std::move(fn)); };
}

// 回测构造：注入虚拟时钟与内联执行器（默认 submit=同步执行，保证确定性）
CcgEngine::CcgEngine(std::shared_ptr<ITradingClient> client, EngineHost host)
    : client_(std::move(client)), host_(std::move(host)) {
    if (!host_.submit) host_.submit = [](std::function<void()> fn) { fn(); };
}

void CcgEngine::set_log_cb(LogCb cb) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    log_cb_ = std::move(cb);
}

void CcgEngine::set_trade_cb(TradeCb cb) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    trade_cb_ = std::move(cb);
}

void CcgEngine::log(const std::string& msg) {
    LogCb cb;
    { std::lock_guard<std::recursive_mutex> lk(mtx_); cb = log_cb_; }
    if (cb) cb("[CCG] " + msg);
}

// ── Bot 生命周期 ──────────────────────────────────────────────────────────────
std::string CcgEngine::add_bot(const CcgConfig& raw_cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    CcgConfig cfg = raw_cfg;
    // 层数夹逼到支持范围：超出上限会静默截断预算分配，且超出层 should_enter
    // 永真造成每 tick 空转派发
    cfg.max_entries = std::max(1, std::min(cfg.max_entries, kMaxLayers));

    // Both 在引擎内部所有 is_long 判断里都会走空头分支——"双向"必须由上层拆成
    // 两个独立 bot，直接传 Both 进来得到的是一个伪装成双向的纯空单，拒绝
    if (cfg.direction == CcgConfig::Direction::Both) {
        log(cfg.symbol + " 拒绝添加：direction=Both 必须拆成多/空两个独立bot");
        return "";
    }

    for (const auto& [id, b] : bots_) {
        if (b.cfg.symbol != cfg.symbol || b.state == CcgBot::State::Stopped) continue;
        // 防止同品种同方向重复添加（已停止的可以重新添加）
        if (b.cfg.direction == cfg.direction) {
            log(cfg.symbol + " 已有运行中/冷却中的相同方向Bot，跳过重复添加");
            return "";
        }
        // 单向持仓模式下，同品种反向 bot 的 BUY/SELL 会在交易所净额互相抵消，
        // 两个 bot 的本地跟踪同时失真——只有双向持仓(hedge)模式才允许共存
        if (client_ && !client_->is_dual_mode()) {
            log(cfg.symbol + " 拒绝添加：单向持仓模式下不能同品种同时做多和做空"
                "（会互相抵消仓位），请在币安切换双向持仓模式或停掉另一方向");
            return "";
        }
    }

    std::string suffix;
    switch (cfg.direction) {
    case CcgConfig::Direction::Long:  suffix = "_L"; break;
    case CcgConfig::Direction::Short: suffix = "_S"; break;
    default:                           suffix = "_B"; break;
    }
    std::string id = cfg.symbol + suffix + "_" + std::to_string(id_seq_++);

    CcgBot bot;
    bot.bot_id     = id;
    bot.cfg        = cfg;
    bot.state      = CcgBot::State::Running;
    bot.start_time = host_.now_wall();
    bots_[id]      = std::move(bot);
    return id;
}

std::string CcgEngine::restore_bot(CcgBot snapshot) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    for (const auto& [id, b] : bots_) {
        if (b.cfg.symbol    == snapshot.cfg.symbol &&
            b.cfg.direction == snapshot.cfg.direction &&
            b.state         != CcgBot::State::Stopped) {
            return "";
        }
    }

    if (snapshot.bot_id.empty())
        snapshot.bot_id = snapshot.cfg.symbol + "_restored_" + std::to_string(id_seq_++);
    snapshot.pending    = false;
    snapshot.start_time = host_.now_wall();

    std::string id = snapshot.bot_id;
    bots_[id] = std::move(snapshot);
    return id;
}

bool CcgEngine::update_bot_cfg(const std::string& id, const CcgConfig& raw_cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return false;
    CcgConfig new_cfg = raw_cfg;
    new_cfg.max_entries = std::max(1, std::min(new_cfg.max_entries, kMaxLayers));
    auto& cfg = it->second.cfg;
    // symbol/direction 是 bot 的身份标识，不允许通过编辑改变
    cfg.strat_type    = new_cfg.strat_type;
    cfg.budget_usdt    = new_cfg.budget_usdt;
    cfg.leverage       = new_cfg.leverage;
    cfg.max_entries    = new_cfg.max_entries;
    cfg.interval_pct   = new_cfg.interval_pct;
    cfg.trail_entry    = new_cfg.trail_entry;
    cfg.tp_pct         = new_cfg.tp_pct;
    cfg.trail_tp       = new_cfg.trail_tp;
    cfg.auto_restart   = new_cfg.auto_restart;
    cfg.cooldown_secs  = new_cfg.cooldown_secs;
    cfg.use_disaster_stop = new_cfg.use_disaster_stop;
    cfg.disaster_stop_pct = new_cfg.disaster_stop_pct;
    cfg.entry_mode     = new_cfg.entry_mode;
    cfg.kline_interval = new_cfg.kline_interval;
    cfg.boll_period    = new_cfg.boll_period;
    cfg.boll_mult      = new_cfg.boll_mult;
    cfg.use_rsi_filter = new_cfg.use_rsi_filter;
    cfg.rsi_period     = new_cfg.rsi_period;
    cfg.rsi_threshold  = new_cfg.rsi_threshold;
    cfg.rsi_confirm_mode = new_cfg.rsi_confirm_mode;
    cfg.rsi_oversold_th  = new_cfg.rsi_oversold_th;
    cfg.use_trend_filter  = new_cfg.use_trend_filter;
    cfg.trend_interval    = new_cfg.trend_interval;
    cfg.trend_ema_period  = new_cfg.trend_ema_period;
    cfg.use_htf_filter      = new_cfg.use_htf_filter;
    cfg.htf_interval        = new_cfg.htf_interval;
    cfg.htf_pos_max         = new_cfg.htf_pos_max;
    return true;
}

void CcgEngine::update_htf(const std::string& bot_id, double pct_b) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    it->second.htf_ok    = (pct_b >= -0.5);   // decision::pct_b 非法时返回 -1
    it->second.htf_pct_b = pct_b;
    it->second.htf_time  = host_.now_steady();
}

// 下单/平仓的异步任务抛出后统一收尾：复位 pending 与在途保证金。
// 不复位的话那个 bot 会永久卡住——所有开仓/补仓/止盈路径都以 !pending 为前提
void CcgEngine::clear_pending_after_throw(const std::string& bot_id, const std::string& what) {
    log(what);
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    it->second.pending = false;
    it->second.inflight_margin = 0;
}

void CcgEngine::update_trend(const std::string& bot_id, bool bearish) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    auto& bot = it->second;
    bot.trend_time = host_.now_steady();

    // 消抖：价格骑在 EMA200/斜率阈值边界时，每5分钟的重判会来回翻面。
    // 原始判定连续2次（约10分钟）相同才真正切换状态——趋势级别的信号不差这点延迟
    if (bearish == bot.trend_raw_last) {
        if (bot.trend_raw_streak < 1000) ++bot.trend_raw_streak;
    } else {
        bot.trend_raw_last   = bearish;
        bot.trend_raw_streak = 1;
    }
    if (bot.trend_raw_streak < 2 || bearish == bot.trend_bearish) return;

    bot.trend_bearish = bearish;
    log(bot.cfg.symbol + " 趋势状态切换: " +
        (bearish ? "空头态（暂停新首仓，补仓间隔×1.5）" : "多头/震荡态（恢复正常）") +
        "（连续2次确认）");
}

void CcgEngine::set_max_total_margin(double usdt) {
    max_total_margin_.store(std::max(0.0, usdt));
}

double CcgEngine::max_total_margin() const {
    return max_total_margin_.load();
}

void CcgEngine::set_max_open_positions(int n) {
    max_open_positions_.store(std::max(0, n));
}

int CcgEngine::max_open_positions() const {
    return max_open_positions_.load();
}

int CcgEngine::open_position_count() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    int n = 0;
    for (const auto& [id, b] : bots_) {
        // 已有持仓，或首仓正在途中（inflight_margin>0 且还没成交）都要算进去。
        // 只数 total_qty>0 的话，同一 tick 窗口里派发出去的那几笔在成交入账前
        // 是"隐形"的，多个品种会一起过闸——与 inflight_margin 防的是同一类竞争
        if (b.total_qty > 0 || b.inflight_margin > 0) ++n;
    }
    return n;
}

double CcgEngine::total_margin_used() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    double sum = 0;
    for (const auto& [id, b] : bots_) {
        if (b.total_cost > 0 && b.cfg.leverage > 0) sum += b.total_cost / b.cfg.leverage;
        sum += b.inflight_margin;   // 在途首仓预占，防止多品种同窗口齐过闸集体超限
    }
    return sum;
}

// 周期对账里"刚成交"的静置期。持仓快照来自 REST，可能拍摄于成交之前——
// GUI 每 3 秒拉一次，加上网络延迟，30 秒有足够余量。
// 代价只是外部平仓要多等最多 30 秒才被发现，而此前是"不重启就永远发现不了"
static constexpr int kReconcileSettleSecs = 30;

std::vector<std::string> CcgEngine::reconcile_positions(
        const std::vector<ExchangePos>& exchange,
        ReconcileMode mode,
        const std::set<std::string>& managed_elsewhere) {
    std::vector<std::string> issues;
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    // 统计每个（品种×方向）有几个持仓中的 bot（多个则无法把交易所持仓归属到具体 bot）。
    // 按方向区分：双向持仓模式下同品种多空双bot是合法配置，不该被跳过
    auto key_of = [](const std::string& sym, CcgConfig::Direction d) {
        return sym + ((d == CcgConfig::Direction::Long) ? "|L" : "|S");
    };
    std::map<std::string, int> holders;
    for (const auto& [id, b] : bots_)
        if (b.total_qty > 0) holders[key_of(b.cfg.symbol, b.cfg.direction)]++;

    for (auto& [id, bot] : bots_) {
        if (bot.total_qty <= 0) continue;
        if (holders[key_of(bot.cfg.symbol, bot.cfg.direction)] > 1) {
            issues.push_back(bot.cfg.symbol + " 有多个同向持仓bot，无法自动对账，请人工核对");
            continue;
        }
        // ① 在途的 bot 一律跳过，【不分模式】。
        //
        // pending 的含义就是"状态不确定"：订单已经派发、可能已在交易所成交，
        // 而本地还没入账。此刻拿本地数量和交易所快照比对必然对不上，而且方向
        // 恰好最坏——正在止盈平仓的会被判成"外部平仓"，状态被清空、bot 被停掉。
        //
        // 更糟的是它能制造一个非法状态：清零并置为冷却之后，那笔在途成交的回调
        // 才写回仓位 → 冷却态却持有仓位。而冷却期满时引擎会无条件清理"上一轮
        // 残留"，那笔仓位会被静默抹掉，交易所上却真实存在。
        // 并发压力测试（seed=1/7）正是抓到这条。
        //
        // 启动模式同样跳过：那时本来就没有 pending 的 bot，跳过是 no-op；
        // 而万一真有（例如运行中手动触发对账），跳过才是对的
        if (bot.pending) continue;

        // ② 刚成交的只在周期模式跳过：持仓快照可能拍摄于这笔成交【之前】。
        //    启动模式不能这么做——崩溃重启时 entries 的时间戳可能就在几秒前，
        //    按"刚成交"跳过会让最需要对账的那个场景整个失效
        if (mode == ReconcileMode::Periodic && !bot.entries.empty()) {
            const auto age = host_.now_wall() - bot.entries.back().time;
            if (age < std::chrono::seconds(kReconcileSettleSecs)) continue;
        }

        const int want_dir = (bot.cfg.direction == CcgConfig::Direction::Long) ? 1 : -1;
        const ExchangePos* ex = nullptr;
        for (const auto& p : exchange)
            if (p.symbol == bot.cfg.symbol && p.direction == want_dir) { ex = &p; break; }

        const double local = bot.total_qty;
        const double tol   = std::max(local * 1e-4, 1e-9);

        if (!ex || ex->qty <= tol) {
            // 交易所已无此仓位：外部（手动/强平）已平仓——本地状态作废。
            //
            // ⚠ 这笔平仓的盈亏【进不了交易明细】：本地不知道成交价，也无从推断
            // 是止盈走的还是被强平的。统计里会缺这一笔，这是外部干预的固有代价
            //
            // 仓位已空，撤掉交易所侧的灾难止损单——与止盈平仓同一处理。
            // closePosition 单在无仓可平时币安会自动失效，但不撤会留在挂单列表里，
            // 下一轮开仓时同方向再挂一张会被拒
            if (!bot.disaster_stop_id.empty()) {
                const std::string bid = id;
                host_.submit([this, bid]() { cancel_disaster_stop(bid); });
            }

            bot.entries.clear();
            bot.total_qty = bot.total_cost = bot.avg_price = 0;
            bot.interval_hit = bot.tp_reached = false;
            bot.ind_dipped = false;    // 若要重新等信号，探底状态清零重新累积
            bot.last_entry_price = 0;

            // 后续行为跟随 auto_restart，与止盈平仓走同一条路径：
            // 用户开了自动循环，语义就是"这一轮结束了就开下一轮"，而手动平仓
            // 正是"这一轮结束了"。走冷却而不是直接 Running，是为了留出缓冲——
            // 也让指标首单闸门在下一轮正常生效（冷却期满会落入 entries.empty()
            // 的正常首仓判定，不会无视信号立刻市价买入）。
            //
            // Stopped 保持不动：那是用户意愿或程序自我保护，不能被这里悄悄复活
            if (bot.state == CcgBot::State::Stopped) {
                bot.last_action = "对账:交易所无仓位，本地已清空";
                issues.push_back(bot.cfg.symbol + " 本地记录持仓 " + std::to_string(local) +
                                 " 但交易所已无仓位（外部平仓?），已清空本地状态"
                                 "（该bot本就处于停止态，保持不变）");
            } else if (bot.cfg.auto_restart) {
                bot.cooldown_until = host_.now_wall() +
                                     std::chrono::seconds(bot.cfg.cooldown_secs);
                bot.state = CcgBot::State::Cooldown;
                bot.last_action = "对账:外部已平仓，冷却后重新开始";
                issues.push_back(bot.cfg.symbol + " 本地记录持仓 " + std::to_string(local) +
                                 " 但交易所已无仓位（外部平仓?），已清空本地状态，"
                                 "冷却 " + std::to_string(bot.cfg.cooldown_secs) +
                                 "s 后按策略重新开始（该笔平仓盈亏不计入统计）");
            } else {
                bot.state = CcgBot::State::Stopped;
                bot.last_action = "对账:交易所无仓位，已停止";
                issues.push_back(bot.cfg.symbol + " 本地记录持仓 " + std::to_string(local) +
                                 " 但交易所已无仓位（外部平仓?），已清空本地状态并停止该bot"
                                 "（未开启自动循环）");
            }
        } else if (ex->qty < local - tol) {
            // 交易所比本地少：外部部分平仓——数量收敛到交易所值，均价保留
            issues.push_back(bot.cfg.symbol + " 本地持仓 " + std::to_string(local) +
                             " > 交易所 " + std::to_string(ex->qty) +
                             "（外部部分平仓?），本地数量已收敛到交易所值");
            bot.total_qty  = ex->qty;
            bot.total_cost = bot.avg_price * ex->qty;
            bot.last_action = "对账:数量已收敛";
        } else if (ex->qty > local + tol) {
            // 交易所比本地多：外部手动加过仓——不动本地（多出部分不归引擎管），仅提醒
            issues.push_back(bot.cfg.symbol + " 交易所持仓 " + std::to_string(ex->qty) +
                             " > 本地跟踪 " + std::to_string(local) +
                             "（外部手动加仓?），多出部分不受本程序管理，请知悉");
        }
    }

    // 反向核查：交易所有仓、本地没有任何 bot 认账的"孤儿仓位"——典型场景是
    // 成交后、落盘前进程被杀（断电/OOM）。如果存在同品种同方向、当前空仓的 bot，
    // 就把仓位认领回来（按交易所侧的均价和数量恢复跟踪），否则只能告警等人工处理。
    // 不认领的话不但仓位无人止盈止损，Immediate 模式的 bot 还会再开一份→双倍敞口
    for (const auto& ex : exchange) {
        if (ex.qty <= 0) continue;
        // 另一套引擎（SAR）管着的品种：有主，不是孤儿。它的止盈止损由那边负责，
        // 在这里报警只会每分钟刷一条假告警，把真正的孤儿仓淹掉
        if (managed_elsewhere.count(ex.symbol)) continue;
        auto dir = (ex.direction > 0) ? CcgConfig::Direction::Long : CcgConfig::Direction::Short;

        bool tracked = false;
        CcgBot* adopter = nullptr;
        for (auto& [id, b] : bots_) {
            if (b.cfg.symbol != ex.symbol || b.cfg.direction != dir) continue;
            if (b.total_qty > 0) { tracked = true; break; }
            if (!b.pending && !adopter) adopter = &b;
        }
        if (tracked) continue;

        if (adopter && ex.entry_price > 0) {
            adopter->entries.clear();

            // ⚠ 必须按成本【反推层数】，不能一律记成单层。
            //
            // 补仓闸门看的是 entries.size() >= max_entries。若把整个仓位塞进一笔
            // entry，一个【已经满仓】的 bot 认领后会以为自己还在第 1 层，于是
            // 还能再补第 2~8 层——按预算 18000/递增8层算，认领 18000 之上再补
            // 17500，总名义约等于预算的两倍。
            // 那会直接打破"名义仓位 ≤ 权益 ⇒ 强平价 ≤ 0"这个不变式，
            // 也就是整套资金安全性的地基。
            //
            // 账户级总保证金上限确实也会挡，但它默认是 0（=不限），
            // 所以默认配置下没有任何东西拦这件事。
            const double adopted_cost = ex.qty * ex.entry_price;
            const auto   sizes        = entry_usdt(adopter->cfg);
            const int    max_lv       = std::max(1, adopter->cfg.max_entries);
            int    n_levels = 1;
            double cum      = 0;
            for (int i = 0; i < (int)sizes.size() && i < max_lv; ++i) {
                cum += sizes[i];
                n_levels = i + 1;
                // 容差 2%：交易所均价与本地记账总有零头差异，不该因此少算一层
                if (cum >= adopted_cost * 0.98) break;
            }

            // 各层价格无从得知（只有交易所给的均价），全部按均价合成——
            // entries[].price 不参与任何决策，只用于展示与落盘，所以这样安全。
            // 数量按梯子权重切分，保证 总量 与 均价 与交易所完全一致
            double w_sum = 0;
            for (int i = 0; i < n_levels; ++i) w_sum += sizes[i];
            double placed = 0;
            for (int i = 0; i < n_levels; ++i) {
                CcgEntry e;
                e.level = i;
                e.price = ex.entry_price;
                // 最后一层吃掉舍入残差，确保各层之和精确等于 ex.qty
                e.qty = (i == n_levels - 1) ? (ex.qty - placed)
                                            : (w_sum > 0 ? ex.qty * sizes[i] / w_sum : 0);
                placed += e.qty;
                e.cost_usdt = e.qty * ex.entry_price;
                e.order_id  = "adopted";
                e.time      = host_.now_wall();
                adopter->entries.push_back(e);
            }
            adopter->total_qty  = ex.qty;
            adopter->avg_price  = ex.entry_price;
            adopter->total_cost = ex.qty * ex.entry_price;
            adopter->last_entry_price = ex.entry_price;
            adopter->dca_extreme = ex.entry_price;
            adopter->tp_extreme  = ex.entry_price;
            adopter->interval_hit = adopter->tp_reached = false;
            // ⚠ 必须退出冷却态：持仓与冷却是【互斥】的。
            //
            // 冷却态的语义是"上一轮已平完，在等下一轮"，所以 tick 里冷却期满时会
            // 无条件执行 entries.clear() + total_qty=0 来清理上一轮的残留状态。
            // 如果认领之后还留在冷却态，那笔刚认回来的仓位会在冷却期满时被【静默
            // 清零】，然后同一个 tick 继续往下走、发现"空仓"又开一笔新首仓——
            // 交易所实际持有 认领的 + 新开的，本地只记新开的，也就是【双倍仓位】。
            // 比单纯丢记录更糟，而且前面刚打过一条"已认领"的日志让人放心。
            //
            // Stopped 不动：那是用户意愿或程序自我保护，而 Stopped 不会走冷却清理
            // 那条路径，所以仓位是安全的、只是不自动交易——本地记录仍然正确。
            if (adopter->state == CcgBot::State::Cooldown)
                adopter->state = CcgBot::State::Running;
            adopter->last_action  = "对账:认领孤儿仓位";
            issues.push_back(ex.symbol + " 交易所存在本地未跟踪的仓位（qty=" +
                             std::to_string(ex.qty) + " 均价=" + std::to_string(ex.entry_price) +
                             "），已认领到同方向bot恢复管理，按成本反推为第 " +
                             std::to_string(n_levels) + "/" +
                             std::to_string(adopter->cfg.max_entries) +
                             " 层（可能是上次崩溃期间成交的）");
        } else {
            issues.push_back(ex.symbol + " 交易所存在无人管理的孤儿仓位（qty=" +
                             std::to_string(ex.qty) + "），且没有可认领的同方向bot，"
                             "请人工处理——该仓位目前没有任何止盈止损保护！");
        }
    }

    for (const auto& msg : issues) log("⚠ 对账: " + msg);
    // "一致"只在启动时报一次——那是有信息量的（确认恢复出来的状态可信）。
    // 周期对账每分钟一次，一致是常态，打出来只会把交易日志淹掉：
    // 一天 1440 条噪音，而真正要看见的那条不一致反而被埋在里面
    if (issues.empty() && mode == ReconcileMode::Startup)
        log("对账完成：本地仓位与交易所一致");
    return issues;
}

void CcgEngine::update_indicator(const std::string& bot_id, double boll_lb, double boll_ub, double rsi) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    auto& bot = it->second;
    bot.ind_boll_lb = boll_lb;
    bot.ind_boll_ub = boll_ub;
    bot.ind_rsi     = rsi;
    bot.ind_ok      = true;
    bot.ind_time    = host_.now_steady();

    if (bot.cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold) {
        const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
        bool oversold_now = is_long ? (rsi <= bot.cfg.rsi_oversold_th)
                                     : (rsi >= (100.0 - bot.cfg.rsi_oversold_th));
        if (oversold_now) bot.ind_dipped = true;
    }
}

void CcgEngine::stop_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end()) it->second.state = CcgBot::State::Stopped;
}

void CcgEngine::resume_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end() && it->second.state == CcgBot::State::Stopped)
        it->second.state = CcgBot::State::Running;
}

void CcgEngine::close_bot(const std::string& id) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(id);
        if (it == bots_.end()) return;
        auto& bot = it->second;
        if (bot.pending || bot.entries.empty()) return;  // 无持仓或正在处理中，跳过
        bot.pending = true;
    }
    submit_close(id, "手动平仓");
}

void CcgEngine::remove_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    // 订单在途时删除会造成"交易所已成交、本地记录已删"的孤儿仓位（永远无人止盈止损）
    if (it->second.pending) {
        log(it->second.cfg.symbol + " 正在执行订单，暂时无法删除，请稍后再试");
        return;
    }
    bots_.erase(it);
}

void CcgEngine::stop_all() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (auto& [id, b] : bots_) b.state = CcgBot::State::Stopped;
}

std::vector<CcgBot> CcgEngine::get_bots() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<CcgBot> out;
    out.reserve(bots_.size());
    for (const auto& [id, b] : bots_) out.push_back(b);
    return out;
}

// 趋势空头态：补仓间隔放大。这是唯一还会改写配置间隔的东西——
// v4.6.0 移除动态W之后，间隔/追踪建仓/止盈追踪三个参数直接就是配置值，
// 不再需要多一层间接
static double apply_trend_interval(const CcgBot& bot, double interval_pct,
                                   std::chrono::steady_clock::time_point now) {
    return trend_active_bearish(bot, now) ? interval_pct * kBearIntervalMult : interval_pct;
}

// ── 追踪变量更新（在 tick 持锁中调用）────────────────────────────────────────
void CcgEngine::update_tracking(CcgBot& bot, double price) {
    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);

    if (bot.entries.empty()) return;

    // ── DCA 间隔追踪 ──────────────────────────────────────────────────────────
    // 标准网格：相对上一笔成交价跌够固定间隔就武装追踪建仓。
    // 趋势过滤的"空头态间隔×1.5"叠在这上面（见 apply_trend_interval）
    const double eff_interval =
        apply_trend_interval(bot, bot.cfg.interval_pct, host_.now_steady());
    const double interval_th = bot.last_entry_price *
        (is_long ? (1.0 - eff_interval / 100.0)
                 : (1.0 + eff_interval / 100.0));

    if (!bot.interval_hit) {
        if (is_long ? (price <= interval_th) : (price >= interval_th)) {
            bot.interval_hit = true;
            bot.dca_extreme  = price;
        }
    } else {
        // 到达间隔后追踪极值（多：跟踪最低点；空：跟踪最高点）
        bot.dca_extreme = is_long ? std::min(bot.dca_extreme, price)
                                  : std::max(bot.dca_extreme, price);
    }

    // ── 止盈追踪 ──────────────────────────────────────────────────────────────
    // 均价 ± tp_pct 触发追踪，之后按 trail_tp 的回撤出场（见 should_close）
    if (bot.avg_price > 0) {
        const double tp_th = bot.avg_price *
            (is_long ? (1.0 + bot.cfg.tp_pct / 100.0)
                     : (1.0 - bot.cfg.tp_pct / 100.0));
        const bool tp_hit = is_long ? (price >= tp_th) : (price <= tp_th);
        if (!bot.tp_reached && tp_hit) {
            bot.tp_reached = true;
            bot.tp_extreme = price;
        }
        if (bot.tp_reached) {
            bot.tp_extreme = is_long ? std::max(bot.tp_extreme, price)
                                     : std::min(bot.tp_extreme, price);
        }
    }
}

bool CcgEngine::should_enter(const CcgBot& bot, double price) const {
    if ((int)bot.entries.size() >= bot.cfg.max_entries) return false;
    if (!bot.interval_hit) return false;
    if (bot.tp_reached)    return false;  // 达到止盈时不加仓

    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
    const double trail_entry = bot.cfg.trail_entry;

    double bounce_th = bot.dca_extreme *
        (is_long ? (1.0 + trail_entry / 100.0)
                 : (1.0 - trail_entry / 100.0));
    return is_long ? (price >= bounce_th) : (price <= bounce_th);
}

bool CcgEngine::should_close(const CcgBot& bot, double price) const {
    if (bot.entries.empty()) return false;
    if (!bot.tp_reached)     return false;

    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
    const double trail_th = bot.tp_extreme *
        (is_long ? (1.0 - bot.cfg.trail_tp / 100.0)
                 : (1.0 + bot.cfg.trail_tp / 100.0));
    return is_long ? (price <= trail_th) : (price >= trail_th);
}

// ── 主 tick（由 UI 定时器每 3 秒调用）────────────────────────────────────────
void CcgEngine::tick(const std::string& symbol, double price) {
    // ⚠ 必须显式判 isfinite：NaN 与任何数比较都是 false，所以 `price <= 0` 这道
    // 守卫【拦不住 NaN】。放进去之后 bot.current_price 变成 NaN，Immediate 模式
    // 的空仓 bot 会照常派发首仓，submit_entry 里 usdt/NaN=NaN，而 `qty <= 0`
    // 同样拦不住 NaN——最终会带着一个 NaN 数量去交易所下单。
    // （压力测试 B11：单个 tick 就能复现）
    if (!std::isfinite(price) || price <= 0) return;

    std::vector<std::string>                  do_entry;
    std::vector<std::pair<std::string,std::string>> do_close;  // {id, reason}
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto now = host_.now_wall();

        for (auto& [id, bot] : bots_) {
            if (bot.cfg.symbol != symbol) continue;

            // ── 满层健康度计时 ────────────────────────────────────────────────
            // 放在 pending 检查【之前】：在途下单期间仓位照样存在，该计时。
            // 每个 bot 只属于一个 symbol，所以这里不会重复累加。
            {
                const auto ns = host_.now_steady();
                if (bot.last_health_tick.time_since_epoch().count() != 0 &&
                    bot.state != CcgBot::State::Stopped) {
                    const auto dt = std::chrono::duration_cast<std::chrono::seconds>(
                                        ns - bot.last_health_tick).count();
                    // 上限 60 秒：程序休眠、断连恢复、行情长时间中断都会产生一个
                    // 巨大的间隔，把它算进去会凭空制造"满层很久"或"存活很久"。
                    // 正常 tick 是 3 秒一次，60 秒已经很宽松
                    if (dt > 0 && dt <= 60) {
                        bot.alive_secs += dt;
                        if ((int)bot.entries.size() >= bot.cfg.max_entries)
                            bot.full_layer_secs += dt;
                    }
                }
                bot.last_health_tick = ns;
            }

            if (bot.pending) continue;

            bot.current_price = price;

            if (bot.state == CcgBot::State::Cooldown) {
                if (now < bot.cooldown_until) continue;
                // 冷却结束：清零本轮状态回到 Running，不再直接开首仓，而是落入下方
                // entries.empty() 的正常首仓闸门——指标信号、账户总保证金上限都会被
                // 正确检查（原先直接 do_entry 会绕过这两道闸，指标模式的 bot 每轮
                // 止盈后会无视信号立刻市价重新买入）
                bot.entries.clear();
                bot.avg_price = bot.total_qty = bot.total_cost = 0;
                bot.interval_hit = bot.tp_reached = false;
                bot.ind_dipped  = false;   // 新一轮等待信号，探底状态清零重新累积
                bot.last_entry_price = price;
                bot.dca_extreme = price;
                bot.tp_extreme  = price;
                bot.last_action = "冷却结束，等待开仓条件";
                bot.state = CcgBot::State::Running;
            }
            if (bot.state != CcgBot::State::Running) continue;

            // 初次建仓：初始化追踪基准
            if (bot.last_entry_price == 0) {
                bot.last_entry_price = price;
                bot.dca_extreme      = price;
                bot.tp_extreme       = price;
            }

            update_tracking(bot, price);

            if (bot.entries.empty()) {
                // 首仓：Immediate 模式一满足 Running 就立刻开；Indicator 模式要等
                // BOLL+RSI 信号（UI 每 tick 异步拉取写入 bot.ind_*）才开
                bool can_enter = true;
                if (bot.cfg.entry_mode == CcgConfig::EntryMode::Indicator) {
                    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
                    bool priceCond = is_long ? (price <= bot.ind_boll_lb) : (price >= bot.ind_boll_ub);
                    bool rsiCond = true;
                    if (bot.cfg.use_rsi_filter) {
                        bool snapshotHit = is_long ? (bot.ind_rsi >= bot.cfg.rsi_threshold)
                                                    : (bot.ind_rsi <= (100.0 - bot.cfg.rsi_threshold));
                        rsiCond = (bot.cfg.rsi_confirm_mode == CcgConfig::RsiConfirmMode::CrossFromOversold)
                                ? (bot.ind_dipped && snapshotHit)   // 必须先探底跌破过，再回穿阈值
                                : snapshotHit;                       // 瞬时快照：这一刻到阈值就行
                    }
                    // 指标数据必须在有效期内：冷却重启/程序重启后的头几秒里，
                    // ind_* 还是上一轮甚至几小时前的旧值，拿旧轨道判断会误开仓
                    bool fresh = (host_.now_steady() - bot.ind_time) < kIndStale;
                    can_enter = bot.ind_ok && fresh && priceCond && rsiCond;
                }
                // 趋势状态机：空头态暂停开新首仓（对 Immediate/Indicator 模式都生效）
                if (can_enter && trend_active_bearish(bot, host_.now_steady())) {
                    can_enter = false;
                    if (bot.last_action != "空头趋势，暂停开首仓") {
                        bot.last_action = "空头趋势，暂停开首仓";
                        log(bot.cfg.symbol + " 处于高周期空头态，暂停开新首仓（趋势恢复后自动放行）");
                    }
                }

                // 首仓的宏观许可层。
                // 宏观许可（日线%B）。关掉就完全不参与判定，也不空跑去刷日志。
                // v4.6.0 移除了 24h/近7日涨幅两条闸门——README 自己记着"无实证依据"，
                // 默认也一直是关的，只剩 %B 这一条有 walk-forward 支持
                const bool gates_on = bot.cfg.use_htf_filter;
                std::string decision_snap;
                if (can_enter && gates_on) {
                    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
                    auto snow = host_.now_steady();

                    decision::Inputs din;
                    din.is_long     = is_long;
                    // 数据缺失不放行：宁可错过不可乱开
                    din.strict      = true;
                    din.use_htf     = bot.cfg.use_htf_filter;
                    const bool htf_fresh_30m =
                        (snow - bot.htf_time) < std::chrono::minutes(30);
                    din.htf_ok      = bot.htf_ok && htf_fresh_30m;
                    din.htf_pct_b   = bot.htf_pct_b;
                    din.htf_pos_max = bot.cfg.htf_pos_max;
                    auto verdict  = decision::evaluate(din);
                    decision_snap = decision::summarize(din, verdict);
                    // 每次判定都刷新（不管放行还是拦截），界面据此显示实时判据。
                    // 与 last_action 分开：那个是去重键，必须保持稳定
                    bot.last_decision = decision_snap;

                    if (!verdict.pass()) {
                        can_enter = false;
                        // 数据未就绪与条件不满足分开提示——前者是"等一等"，
                        // 后者是"这里不该买"，用户看日志时需要能区分
                        const char* why = verdict.data_block ? "宏观数据未就绪，暂不开仓"
                                                             : "宏观拦截";
                        if (bot.last_action != why) {
                            bot.last_action = why;
                            std::string hint;
                            if (verdict.data_block) {
                                // 提示必须指向【真正缺的那条线】。三个判据的数据来源
                                // 完全不同，笼统说一句"新上市品种需等历史"会把人引偏——
                                // 24h 涨幅缺失跟品种上市多久毫无关系
                                hint = "（";
                                if (verdict.day_chg_missing)
                                    hint += "24h涨幅来自 @ticker 推送流，一直不到通常是"
                                            "WebSocket 订阅失败（查日志里的\"行情WS服务端消息\"）"
                                            "或网络不通；把该阈值填 0 可先关掉这条闸门。";
                                if (verdict.htf_missing)
                                    hint += "日线指标来自 REST，新上市品种需等日线21根历史。";
                                hint += "数据到齐后自动放行）";
                            }
                            log(bot.cfg.symbol + " " + why + ": " + decision_snap + hint);
                        }
                    }
                }
                // 账户级并发持仓上限：只挡"开新首仓"。放在保证金上限【之前】判——
                // 它更便宜（数个数 vs 遍历求和），而且在全市场扫描场景下它才是
                // 先撞到的那一道
                if (can_enter) {
                    const int cap_n = max_open_positions_.load();
                    if (cap_n > 0 && open_position_count() >= cap_n) {
                        can_enter = false;
                        std::string why = "已达并发持仓上限(" + std::to_string(cap_n) + ")，暂缓开首仓";
                        if (bot.last_action != why) {
                            bot.last_action = why;
                            log(bot.cfg.symbol + " " + why);
                        }
                    }
                }
                // 账户级总保证金上限：只挡"开新首仓"，已有仓位的加仓/止盈止损不受影响
                auto sizes = entry_usdt(bot.cfg);
                double first_margin = (!sizes.empty() && bot.cfg.leverage > 0)
                    ? sizes[0] / bot.cfg.leverage : 0;
                double cap = max_total_margin_.load();
                if (can_enter && cap > 0) {
                    if (total_margin_used() + first_margin > cap) {
                        can_enter = false;
                        if (bot.last_action != "达到总保证金上限，暂缓开首仓") {
                            bot.last_action = "达到总保证金上限，暂缓开首仓";
                            log(bot.cfg.symbol + " 达到账户总保证金上限（$" +
                                std::to_string((int)cap) + "），暂缓开首仓");
                        }
                    }
                }
                if (can_enter) {
                    // 预占在途保证金：成交入账前的窗口里，其他品种的闸门检查
                    // 必须能看到这笔即将占用的额度（submit_entry 完成时清零）
                    bot.inflight_margin = first_margin;
                    do_entry.push_back(id);
                    bot.pending = true;
                    // 决策快照随首仓派发落日志
                    if (!decision_snap.empty())
                        // 保留：一轮只打一次，是事后复盘"这单当初凭什么开"的唯一依据
                        log("[决策] " + bot.cfg.symbol + " 首仓派发 @" +
                            std::to_string(price) + " | " + decision_snap);
                }
            } else if (should_close(bot, price)) {
                do_close.push_back({id, "追踪止盈"});
                bot.pending = true;
            } else if (should_enter(bot, price)) {
                // 账户级总保证金上限：补仓同样受约束。
                // 此前这道闸只挡首仓，已建仓的 bot 可以一路补到把账户吃光——
                // 统一账户下更危险，保证金池是全账户共享的，一个品种深度补仓
                // 会把其他品种一起拖进强平
                double cap_d = max_total_margin_.load();
                bool   cap_blocked = false;
                if (cap_d > 0) {
                    auto   sizes_d = entry_usdt(bot.cfg);
                    size_t lvl_d   = bot.entries.size();
                    double next_margin = (lvl_d < sizes_d.size() && bot.cfg.leverage > 0)
                                         ? sizes_d[lvl_d] / bot.cfg.leverage : 0;
                    cap_blocked = (total_margin_used() + next_margin > cap_d);
                }
                if (cap_blocked) {
                    std::string why = "第" + std::to_string(bot.entries.size() + 1) +
                                      "层补仓达到账户总保证金上限，暂缓";
                    if (bot.last_action != why) {
                        bot.last_action = why;
                        log(bot.cfg.symbol + " " + why + "（$" +
                            std::to_string((int)cap_d) + "）");
                    }
                } else {
                    do_entry.push_back(id);
                    bot.pending = true;
                }
            }
        }
    }

    for (const auto& id         : do_entry) submit_entry(id);
    for (const auto& [id, reason]: do_close) submit_close(id, reason);
}

// ── 交易所侧灾难止损单 ────────────────────────────────────────────────────────
// 挂在币安服务器上，进程死了它还在。每次仓位变化后按新均价重挂。
// 全程在线程池线程里跑；HTTP 调用期间【不持锁】，只在读参数和写回结果时短暂持锁。
void CcgEngine::sync_disaster_stop(const std::string& bot_id) {
    std::string sym, side, old_id;
    double target = 0, old_price = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        auto& b = it->second;
        if (!b.cfg.use_disaster_stop || b.cfg.disaster_stop_pct <= 0) return;
        if (b.total_qty <= 0 || b.avg_price <= 0) return;
        // 同一 bot 的同步串行化：已有一次在途就直接跳过。跳过是安全的——真正需要
        // 改单的时候（均价变了），下一次成交还会再派发一次；而并发跑两次的后果是
        // 第二张挂单被币安拒、把第一张的单号误清掉（见 ds_syncing 的说明）
        if (b.ds_syncing) return;
        b.ds_syncing = true;

        const bool is_long = (b.cfg.direction == CcgConfig::Direction::Long);
        target = b.avg_price * (is_long ? (1.0 - b.cfg.disaster_stop_pct / 100.0)
                                        : (1.0 + b.cfg.disaster_stop_pct / 100.0));
        sym    = b.cfg.symbol;
        side   = is_long ? "BUY" : "SELL";
        old_id = b.disaster_stop_id;
        old_price = b.disaster_stop_price;
    }

    // 下面有 5 个提前返回点，标记必须每条路径都清掉——漏一条这个 bot 的灾难止损
    // 就永久不再同步了（比并发挂两张更糟：静默失去保护）。交给析构函数，不靠人记
    struct SyncFlagGuard {
        CcgEngine* self; const std::string& id;
        ~SyncFlagGuard() {
            std::lock_guard<std::recursive_mutex> lk(self->mtx_);
            auto it = self->bots_.find(id);
            if (it != self->bots_.end()) it->second.ds_syncing = false;
        }
    } flag_guard{this, bot_id};

    if (target <= 0) return;

    // 触发价没有实质变化就不动它——每次补仓都撤了重挂会平白消耗限流额度，
    // 而且撤单和挂单之间有个没有保护的空窗
    if (!old_id.empty() && old_price > 0 &&
        std::fabs(target - old_price) / old_price < 0.001) return;

    // 先挂新的再撤旧的？不行——closePosition 单同一方向只能存在一张，
    // 币安会拒掉第二张。只能先撤后挂，空窗期无法避免，所以尽量少动（上面的阈值）
    if (!old_id.empty()) client_->cancel_disaster_stop(sym, old_id);

    std::string new_id = client_->place_disaster_stop(sym, target, side);

    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    if (new_id.empty()) {
        // 挂单失败是**必须让人看见**的：仓位此刻没有任何进程外保护
        it->second.disaster_stop_id.clear();
        it->second.disaster_stop_price = 0;
        log("⚠ " + sym + " 交易所侧灾难止损单挂单失败——该仓位当前没有进程外保护，"
            "下次仓位变化时会自动重试");
        return;
    }
    it->second.disaster_stop_id    = new_id;
    it->second.disaster_stop_price = target;
    log(sym + " 交易所侧灾难止损单已挂 @$" + std::to_string((int)target) +
        "（均价 -" + std::to_string(it->second.cfg.disaster_stop_pct) + "%，进程死了也在）");
}

// 重启后重建交易所侧保护：把记着的触发价清零，强制走一遍"撤旧单+按当前均价重挂"。
// 不能只依赖落盘的 order_id——程序不在的这段时间里那张单可能已经触发或被手动撤掉，
// 本地记录并不代表交易所上还有
void CcgEngine::resync_disaster_stops() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& [id, b] : bots_) {
            if (!b.cfg.use_disaster_stop || b.cfg.disaster_stop_pct <= 0) continue;
            if (b.total_qty <= 0 || b.avg_price <= 0) continue;
            b.disaster_stop_price = 0;   // 清零 = 强制重挂
            ids.push_back(id);
        }
    }
    for (const auto& id : ids)
        host_.submit([this, id]() { sync_disaster_stop(id); });
}

void CcgEngine::cancel_disaster_stop(const std::string& bot_id) {
    std::string sym, id;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        if (it->second.disaster_stop_id.empty()) return;
        sym = it->second.cfg.symbol;
        id  = it->second.disaster_stop_id;
        // 先在本地清掉：即便撤单请求失败，仓位也已经平了，交易所侧的
        // closePosition 单会因为无仓可平而自动失效，不该继续记在账上
        it->second.disaster_stop_id.clear();
        it->second.disaster_stop_price = 0;
    }
    client_->cancel_disaster_stop(sym, id);
}

// ── 异步入场（在线程池中执行 HTTP 下单）──────────────────────────────────────
void CcgEngine::submit_entry(const std::string& bot_id) {
    host_.submit([this, bot_id]() {
        try {
            // 读取状态（短暂持锁）
            CcgConfig     cfg;
            double        price = 0;
            int           level = 0;
            double        usdt  = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it == bots_.end()) return;
                const auto& bot = it->second;
                cfg   = bot.cfg;
                price = bot.current_price;
                level = (int)bot.entries.size();
                auto sizes = entry_usdt(cfg);
                if (level >= (int)sizes.size()) {
                    it->second.pending = false;
                    it->second.inflight_margin = 0;
                    return;
                }
                usdt = sizes[level];
            }

            // 首仓时设置杠杆。返回值不能丢：失败的话仓位会按【交易所上原有的杠杆】
            // 开出去，而本地的保证金核算用的是 cfg.leverage（total_margin_used 里
            // total_cost/leverage）。两者不一致时账户级保证金上限守的是一个假数字——
            // 若交易所实际杠杆比配置的低，真实占用的保证金高于本地估算，那道闸就
            // 形同虚设。对"保证金提前规划好"的用法来说这是直接踩在痛点上。
            // 仍然放行而不是拦下：多数失败是网络抖动，而敞口（qty×价格）并不受
            // 杠杆影响，错过建仓的代价更大。但必须让人看见
            if (level == 0 && !client_->set_leverage(cfg.symbol, cfg.leverage)) {
                log("⚠ " + cfg.symbol + " 杠杆设置失败（目标 " +
                    std::to_string(cfg.leverage) + "x）——本次将按交易所上原有杠杆开仓，"
                    "本地保证金核算可能与实际不符，请到交易所核对杠杆设置");
            }

            const std::string side = (cfg.direction == CcgConfig::Direction::Long)
                                     ? "BUY" : "SELL";
            // price 由 tick() 保证有限且 >0 才会派发到这里，但一旦为0，usdt/price
            // 得到的是 inf，而下面的 qty<=0 检查【拦不住 inf 和 NaN】——会带着一个
            // 无穷大/非数的数量去下单。纵深防御：tick() 是第一道，这里是第二道
            if (!std::isfinite(price) || price <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.inflight_margin = 0;
                    it->second.last_action = "无有效价格，跳过本次开仓";
                }
                log(cfg.symbol + " 第" + std::to_string(level+1) + "仓：价格无效，跳过");
                return;
            }
            double qty = client_->round_qty(cfg.symbol, usdt / price);
            if (!std::isfinite(qty) || qty <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.inflight_margin = 0;
                    it->second.last_action = "数量不足，跳过";
                }
                log(cfg.symbol + " 第" + std::to_string(level+1) + "仓数量不足");
                return;
            }

            auto r = client_->place_market_order(cfg.symbol, side, qty, false);

            // 更新状态
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it == bots_.end()) return;
            auto& bot = it->second;
            bot.pending = false;
            bot.inflight_margin = 0;

            // 零成交防幽灵仓：市价单可能被接受但零成交（EXPIRED，无流动性），
            // 此时 r.ok=true 但 executedQty=0——绝不能把下单前的目标数量当成交入账，
            // 否则本地会记录一笔交易所根本不存在的仓位，后续止盈止损全部失真
            if (r.ok && r.executed_qty <= 0) {
                bot.last_action = "下单零成交，下个tick重试";
                log(cfg.symbol + " 第" + std::to_string(level+1) +
                    "仓订单已提交但零成交（流动性不足?），不入账，下个tick重试");
                return;
            }

            if (r.ok) {
                // 优先用交易所返回的实际成交均价/数量；下单前的快照价只做兜底
                // （否则 qty 按 lot size 取整后，用下单前的目标 usdt 算均价会systematic 偏差）
                double fill_price = (r.avg_price    > 0) ? r.avg_price    : price;
                double fill_qty   = r.executed_qty;

                CcgEntry e;
                e.level     = level;
                e.price     = fill_price;
                e.qty       = fill_qty;
                e.cost_usdt = fill_qty * fill_price;
                e.order_id  = r.order_id;
                e.time      = host_.now_wall();
                bot.entries.push_back(e);

                bot.total_qty  += fill_qty;
                bot.total_cost += fill_qty * fill_price;
                bot.avg_price   = bot.total_cost / bot.total_qty;
                bot.last_entry_price = fill_price;
                bot.dca_extreme      = fill_price;
                bot.tp_extreme       = fill_price;
                bot.interval_hit     = false;
                bot.tp_reached       = false;

                std::ostringstream ss;
                ss << cfg.symbol << " 第" << (level+1) << "/" << cfg.max_entries << "仓"
                   << (cfg.direction==CcgConfig::Direction::Long ? "多" : "空")
                   << " qty=" << fill_qty << " @$" << std::fixed << std::setprecision(4) << fill_price
                   << " 均价=$" << std::setprecision(4) << bot.avg_price;
                bot.last_action = "第" + std::to_string(level+1) + "仓@" +
                                  std::to_string((int)fill_price);
                log(ss.str());
                // 仓位变了（均价下移），交易所侧的灾难止损单要跟着改。
                // 派到线程池另跑，不阻塞本次下单回调
                if (cfg.use_disaster_stop && cfg.disaster_stop_pct > 0)
                    host_.submit([this, bot_id]() { sync_disaster_stop(bot_id); });
            } else if (r.uncertain) {
                // 网络中断连查单都失败：订单可能已成交但本地没记录。绝不能下个tick
                // 盲目重试（可能双倍仓位）——停掉该bot，等联网后由对账功能恢复真相
                bot.state = CcgBot::State::Stopped;
                bot.last_action = "⚠ 下单状态不明，已停止待人工核对";
                log("⚠ " + cfg.symbol + " 第" + std::to_string(level+1) +
                    "仓下单状态不明（" + r.error + "），已停止该bot。请恢复网络后重启程序触发对账，或手动核对交易所仓位");
            } else {
                log(cfg.symbol + " 第" + std::to_string(level+1) + "仓失败: " + r.error);
            }
        } catch (const std::exception& e) {
            clear_pending_after_throw(bot_id, "submit_entry 异常: " + std::string(e.what()));
        } catch (...) {
            // 非 std::exception 的东西逃出去，pending 会永久停留在 true——
            // 那个 bot 从此既不下单也不平仓，且没有任何日志。
            // 实践中 libcurl/simdjson 都抛 std::exception 派生类，概率很低，
            // 但后果是"永久静默冻结"，值得一条兜底
            clear_pending_after_throw(bot_id, "submit_entry 未知异常（非 std::exception）");
        }
    });
}

// ── 异步平仓 ──────────────────────────────────────────────────────────────────
void CcgEngine::submit_close(const std::string& bot_id, const std::string& reason) {
    host_.submit([this, bot_id, reason]() {
        try {
            CcgConfig cfg;
            double    total_qty = 0, avg_price = 0, close_price = 0;
            int       layers = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it == bots_.end()) return;
                const auto& bot = it->second;
                cfg         = bot.cfg;
                total_qty   = bot.total_qty;
                avg_price   = bot.avg_price;
                close_price = bot.current_price;
                layers      = (int)bot.entries.size();
            }

            bool   closed_ok      = false;
            bool   external_gone  = false;   // 交易所确认无此仓位（外部已平仓）
            bool   unclosable     = false;   // 残量低于最小下单量，市价单发不出去
            double closed_qty = 0;   // 实际平掉的数量（可能是部分成交）
            if (total_qty > 0) {
                const std::string side = (cfg.direction == CcgConfig::Direction::Long)
                                         ? "SELL" : "BUY";
                double qty = client_->round_qty(cfg.symbol, total_qty);
                // 残量低于交易所最小下单量：取整后 qty=0，这张单必被拒。
                // 此前没有这道检查，于是每个 tick 都会发一张数量为 0 的平仓单——
                // 被拒→下个tick再发，无限空转，还白白消耗限流额度，仓位永远不结清。
                // 注意这【不是】已有的"灰尘结算"能覆盖的情况：那段逻辑只在平仓
                // 【成功】之后才跑，而这里单子根本发不出去。
                // 触发路径：开仓部分成交到低于最小下单量（压力测试 B12 可确定性复现）
                if (qty <= 0) {
                    unclosable = true;
                }
                auto r = unclosable ? OrderOutcome{}
                                    : client_->place_market_order(cfg.symbol, side, qty, true);
                if (!r.ok && r.error.find("[-2022]") != std::string::npos) {
                    // reduceOnly被拒 = 交易所侧没有可平的仓位（用户在交易所手动平过/
                    // 强平过）。本地留着这个幽灵仓位会陷入无限重试（追踪止盈每 tick
                    // 再触发 -2022），且用户手动平仓也平不掉——按启动对账同款
                    // 策略：清空本地状态并停止该bot，等人工确认后手动"继续"
                    external_gone = true;
                }
                if (r.ok) {
                    // 只认交易所回报的实际成交量：0 = 没成交（如市价单无流动性EXPIRED），
                    // 视为失败留到下个tick重试，绝不能假设"下了单=平掉了"
                    closed_qty = r.executed_qty;
                    closed_ok  = closed_qty > 0;
                    if (!closed_ok) log(cfg.symbol + " 平仓单已提交但零成交，下个tick重试");
                    if (r.avg_price > 0) close_price = r.avg_price;  // 用真实成交均价算PnL
                } else {
                    // uncertain（网络中断查单也失败）也走这里：reduceOnly 平仓天然幂等——
                    // 若实际已平，下个tick重试会被交易所拒绝，对账兜底；重试是安全的
                    log(cfg.symbol + " 平仓失败: " + r.error +
                        (r.uncertain ? "（状态不明，reduceOnly重试安全，下个tick再试）" : ""));
                }
            } else {
                closed_ok  = true;  // 无持仓，视为已平
                closed_qty = 0;
            }

            // trade_cb_ 必须在锁外调用（回调里可能做持久化/发通知，持锁调用会把
            // 整个引擎阻塞在回调时长上，回调若再碰引擎还会死锁）——锁内只填记录
            TradeRecord rec;
            bool        has_rec = false;
            TradeCb     cb_copy;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it == bots_.end()) return;
                auto& bot = it->second;
                bot.pending = false;
                cb_copy = trade_cb_;

                // 残量低于最小下单量：停机 + 告警，而不是伪造一笔平仓。
                // 为什么不当灰尘直接结清：最小下单量并不等于"金额可忽略"——
                // BTCUSDT 的最小下单量是 0.001，按 10 万美元算就是 100 美元。
                // 凭空记一笔没真正卖出的盈亏，是在账本上撒谎。
                // 交易所网页端的「一键平仓」不受最小下单量限制，用户能自己处理
                if (unclosable) {
                    bot.state = CcgBot::State::Stopped;
                    bot.last_action = "残量低于最小下单量，无法平仓，已停止";
                    log("⚠ " + cfg.symbol + " 剩余持仓 " + std::to_string(total_qty) +
                        " 低于交易所最小下单量，市价平仓单发不出去。已停止该bot"
                        "（否则会每个tick空转重发）——请在交易所网页端用「一键平仓」"
                        "处理这笔残仓，再点【继续】");
                    return;
                }

                // 交易所确认无此仓位：清空本地跟踪并停止（同启动对账的外部平仓处理）
                if (external_gone) {
                    bot.entries.clear();
                    bot.total_qty = bot.total_cost = bot.avg_price = 0;
                    bot.interval_hit = bot.tp_reached = false;
                    bot.ind_dipped = false;
                    bot.last_entry_price = 0;
                    bot.state = CcgBot::State::Stopped;
                    bot.last_action = "交易所无此仓位，已清空并停止";
                    log("⚠ " + cfg.symbol + " 平仓被拒[-2022]：交易所已无此仓位（外部手动"
                        "平仓/强平?）。本地状态已清空、bot已停止——确认无误后可点【继续】重新启用");
                    return;
                }

                // 灰尘结算：剩余量取整后低于最小下单量时永远无法平掉，若按"部分成交"
                // 处理会陷入每 tick 重试的死循环——把灰尘视为已平清，正常结算本轮
                double remaining = total_qty - closed_qty;
                bool   dust_only = closed_ok && total_qty > 0 &&
                                   remaining > 0 &&
                                   client_->round_qty(cfg.symbol, remaining) <= 0;

                // 部分成交：只平掉了一部分且剩余仍可交易，扣减本地数量保留跟踪，下个tick继续
                if (closed_ok && total_qty > 0 && !dust_only &&
                    closed_qty < total_qty * 0.999) {
                    const bool is_long = (cfg.direction == CcgConfig::Direction::Long);
                    double pnl = (is_long ? (close_price - avg_price)
                                           : (avg_price - close_price)) * closed_qty;
                    bot.realized_pnl += pnl;
                    // ⚠ 必须从【当前】持仓量扣减，不能写 total_qty(旧快照) - closed_qty。
                    // 发 HTTP 期间是不持锁的（否则整个引擎会卡在网络时长上），这段窗口里
                    // 对账可能已经把仓位清空（交易所侧确认外部已平仓 → entries 清空、
                    // total_qty 归零）。用旧快照做绝对赋值会把一个已经作废的数量重新写
                    // 回去，得到"没有任何加仓记录、却有持仓量"的撕裂状态——那个幽灵仓位
                    // 会占住保证金上限、反复发被拒的平仓单，盈亏还算在一个不存在的仓位上。
                    // 增量扣减则天然幂等：清空过就是 0，没清空就正常减。
                    // （压力测试 B13 可确定性复现；并发压测里约每 2 万轮出现一次）
                    if (bot.total_qty + 1e-12 < total_qty) {
                        log("⚠ " + cfg.symbol + " 平仓在途期间本地持仓被改动（对账?），"
                            "按改动后的数量结算");
                    }
                    bot.total_qty  = std::max(0.0, bot.total_qty - closed_qty);
                    bot.total_cost = bot.avg_price * bot.total_qty;
                    // tp_reached/entries 保持不变——下个tick should_close 仍成立，继续平剩余
                    bot.last_action = reason + "(部分成交," + std::to_string(closed_qty) + ")";
                    log(cfg.symbol + " " + reason + " 部分成交 " + std::to_string(closed_qty) +
                        "/" + std::to_string(total_qty) + "，剩余 " + std::to_string(bot.total_qty) +
                        " 下个tick继续平仓");
                    rec.symbol      = cfg.symbol;
                    rec.direction   = cfg.direction;
                    rec.entry_price = avg_price;
                    rec.exit_price  = close_price;
                    rec.qty         = closed_qty;
                    rec.pnl         = pnl;
                    rec.layers      = layers;
                    rec.reason      = reason + "(部分)";
                    rec.close_time  = host_.now_wall();
                    has_rec = true;
                } else if (closed_ok) {
                    const bool is_long = (cfg.direction == CcgConfig::Direction::Long);
                    // PnL 按实际平掉的数量算（灰尘忽略不计，量级可忽略）
                    double settle_qty = (closed_qty > 0) ? closed_qty : total_qty;
                    double pnl = total_qty > 0
                        ? (is_long ? (close_price - avg_price) : (avg_price - close_price)) * settle_qty
                        : 0.0;
                    bot.realized_pnl += pnl;
                    bot.cycle_count++;

                    if (total_qty > 0) {
                        rec.symbol      = cfg.symbol;
                        rec.direction   = cfg.direction;
                        rec.entry_price = avg_price;
                        rec.exit_price  = close_price;
                        rec.qty         = settle_qty;
                        rec.pnl         = pnl;
                        rec.layers      = layers;
                        rec.reason      = reason;
                        rec.close_time  = host_.now_wall();
                        has_rec = true;
                    }

                    std::ostringstream ss;
                    ss << cfg.symbol << " " << reason
                       << " 均=$" << std::fixed << std::setprecision(4) << avg_price
                       << " 收=$" << close_price
                       << " P&L=" << std::setprecision(2) << pnl << "U"
                       << " 累计=" << bot.realized_pnl << "U";
                    if (dust_only) ss << "（含忽略灰尘 " << std::to_string(total_qty - closed_qty) << "）";
                    log(ss.str());
                    // 仓位已清空：撤掉交易所侧的灾难止损单。
                    // closePosition 单在无仓可平时币安会自动失效，但不撤会留在挂单
                    // 列表里，下一轮开仓时同方向再挂一张会被拒
                    if (!bot.disaster_stop_id.empty())
                        host_.submit([this, bot_id]() { cancel_disaster_stop(bot_id); });

                    bot.entries.clear();
                    bot.total_qty = bot.total_cost = bot.avg_price = 0;
                    bot.interval_hit = bot.tp_reached = false;
                    bot.ind_dipped  = false;   // 平仓后如果还会再等信号，探底状态清零重新累积
                    bot.last_entry_price = 0;
                    bot.last_action = reason;

                    // 平仓在途期间用户点过【停止】的话，尊重用户意愿保持 Stopped——
                    // 不能被自动重启逻辑无声覆盖成 Cooldown（bot 违背意愿自动复活）
                    if (bot.state != CcgBot::State::Stopped) {
                        if (cfg.auto_restart) {
                            bot.cooldown_until = host_.now_wall() +
                                                 std::chrono::seconds(cfg.cooldown_secs);
                            bot.state = CcgBot::State::Cooldown;
                            log(cfg.symbol + " 冷却 " + std::to_string(cfg.cooldown_secs) + "s 后重启");
                        } else {
                            bot.state = CcgBot::State::Stopped;
                        }
                    }
                }
            }
            if (has_rec && cb_copy) cb_copy(rec);
        } catch (const std::exception& e) {
            clear_pending_after_throw(bot_id, "submit_close 异常: " + std::string(e.what()));
        } catch (...) {
            clear_pending_after_throw(bot_id, "submit_close 未知异常（非 std::exception）");
        }
    });
}

} // namespace ccbot
