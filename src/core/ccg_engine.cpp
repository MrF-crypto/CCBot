#include "core/ccg_engine.h"
#include "core/decision.h"
#include "core/dynamic_params.h"
// 引擎只依赖 ITradingClient 接口（在 ccg_engine.h 里），不再直接依赖具体的
// TradingClient/curl——这样回测目标可以只编译引擎+模拟客户端，无需网络库
#include "core/thread_pool.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace ccbot {

// 指标数据有效期：超过这个时长没有新的 BOLL/RSI 写入就视为过期。
// 正常情况下 UI/headless 每 3s 拉一次，180s 的余量足够容忍短暂网络抖动，
// 又能保证断网/K线接口故障时不会拿几小时前的旧轨道值继续开仓补仓
static constexpr auto kIndStale = std::chrono::seconds(180);

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

// ── 加仓比例序列（最多 10 层）──────────────────────────────────────────────────
static std::vector<double> base_mult(CcgConfig::StratType t) {
    using ST = CcgConfig::StratType;
    switch (t) {
    case ST::Flat:       return {1,1,1,1,1,1,1,1,1,1};
    case ST::Martingale: return {1,1,2,2,4,4,8,8,16,16};
    case ST::MartPlus:   return {1,1,2,3,5,8,13,21,34,55};
    case ST::Triple:     return {1,3,9,27,81,243,729,2187,6561,19683};
    case ST::Square:     return {1,4,9,16,25,36,49,64,81,100};
    case ST::Fibonacci:  return {1,1,2,3,5,8,13,21,34,55};
    case ST::Lucas:      return {2,1,3,4,7,11,18,29,47,76};
    case ST::Linear:     return {1,2,3,4,5,6,7,8,9,10};
    }
    return {1,1,1,1,1,1,1,1,1,1};
}

std::vector<double> CcgEngine::entry_usdt(const CcgConfig& cfg) {
    auto m = base_mult(cfg.strat_type);
    int n = std::min(cfg.max_entries, (int)m.size());
    m.resize(n);
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
    // 层数夹逼到曲线表实际支持的范围：>10 会静默截断预算分配，且第11层起
    // should_enter 永真造成每 tick 空转派发
    cfg.max_entries = std::max(1, std::min(cfg.max_entries, 10));

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
    new_cfg.max_entries = std::max(1, std::min(new_cfg.max_entries, 10));
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
    cfg.stop_loss_pct  = new_cfg.stop_loss_pct;
    cfg.entry_mode     = new_cfg.entry_mode;
    cfg.kline_interval = new_cfg.kline_interval;
    cfg.boll_period    = new_cfg.boll_period;
    cfg.boll_mult      = new_cfg.boll_mult;
    cfg.use_rsi_filter = new_cfg.use_rsi_filter;
    cfg.rsi_period     = new_cfg.rsi_period;
    cfg.rsi_threshold  = new_cfg.rsi_threshold;
    cfg.rsi_confirm_mode = new_cfg.rsi_confirm_mode;
    cfg.rsi_oversold_th  = new_cfg.rsi_oversold_th;
    cfg.dynamic_band_mode = new_cfg.dynamic_band_mode;
    cfg.min_profit_floor  = new_cfg.min_profit_floor;
    cfg.use_trend_filter  = new_cfg.use_trend_filter;
    cfg.trend_interval    = new_cfg.trend_interval;
    cfg.trend_ema_period  = new_cfg.trend_ema_period;
    cfg.sr_radar          = new_cfg.sr_radar;
    cfg.sr_interval       = new_cfg.sr_interval;
    cfg.smart_gates         = new_cfg.smart_gates;
    cfg.use_htf_filter      = new_cfg.use_htf_filter;
    cfg.htf_interval        = new_cfg.htf_interval;
    cfg.htf_pos_max         = new_cfg.htf_pos_max;
    cfg.use_sr_gate         = new_cfg.use_sr_gate;
    cfg.sr_min_confluence   = new_cfg.sr_min_confluence;
    cfg.sr_headroom_ratio   = new_cfg.sr_headroom_ratio;
    cfg.use_sr_exit         = new_cfg.use_sr_exit;
    cfg.use_structural_stop = new_cfg.use_structural_stop;
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

void CcgEngine::update_sr_structure(const std::string& bot_id, bool at_support,
                                    double sup_hi, double res_lo, double stop_level) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    auto& bot = it->second;
    bot.sr_ok         = true;
    bot.sr_at_support = at_support;
    bot.sr_sup_hi     = sup_hi;
    bot.sr_res_lo     = res_lo;
    bot.sr_stop_level = stop_level;
    bot.sr_time       = host_.now_steady();
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

double CcgEngine::total_margin_used() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    double sum = 0;
    for (const auto& [id, b] : bots_) {
        if (b.total_cost > 0 && b.cfg.leverage > 0) sum += b.total_cost / b.cfg.leverage;
        sum += b.inflight_margin;   // 在途首仓预占，防止多品种同窗口齐过闸集体超限
    }
    return sum;
}

std::vector<std::string> CcgEngine::reconcile_positions(const std::vector<ExchangePos>& exchange) {
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

        const int want_dir = (bot.cfg.direction == CcgConfig::Direction::Long) ? 1 : -1;
        const ExchangePos* ex = nullptr;
        for (const auto& p : exchange)
            if (p.symbol == bot.cfg.symbol && p.direction == want_dir) { ex = &p; break; }

        const double local = bot.total_qty;
        const double tol   = std::max(local * 1e-4, 1e-9);

        if (!ex || ex->qty <= tol) {
            // 交易所已无此仓位：外部（手动/强平）已平仓——本地状态作废，停下来等人工确认
            issues.push_back(bot.cfg.symbol + " 本地记录持仓 " + std::to_string(local) +
                             " 但交易所已无仓位（外部平仓?），已清空本地状态并停止该bot");
            bot.entries.clear();
            bot.total_qty = bot.total_cost = bot.avg_price = 0;
            bot.interval_hit = bot.tp_reached = false;
            bot.last_entry_price = 0;
            bot.state = CcgBot::State::Stopped;
            bot.last_action = "对账:交易所无仓位，已停止";
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
            CcgEntry e;
            e.level     = 0;
            e.price     = ex.entry_price;
            e.qty       = ex.qty;
            e.cost_usdt = ex.qty * ex.entry_price;
            e.order_id  = "adopted";
            e.time      = host_.now_wall();
            adopter->entries.push_back(e);
            adopter->total_qty  = ex.qty;
            adopter->avg_price  = ex.entry_price;
            adopter->total_cost = ex.qty * ex.entry_price;
            adopter->last_entry_price = ex.entry_price;
            adopter->dca_extreme = ex.entry_price;
            adopter->tp_extreme  = ex.entry_price;
            adopter->interval_hit = adopter->tp_reached = false;
            adopter->last_action  = "对账:认领孤儿仓位";
            issues.push_back(ex.symbol + " 交易所存在本地未跟踪的仓位（qty=" +
                             std::to_string(ex.qty) + " 均价=" + std::to_string(ex.entry_price) +
                             "），已认领到同方向bot恢复管理（可能是上次崩溃期间成交的）");
        } else {
            issues.push_back(ex.symbol + " 交易所存在无人管理的孤儿仓位（qty=" +
                             std::to_string(ex.qty) + "），且没有可认领的同方向bot，"
                             "请人工处理——该仓位目前没有任何止盈止损保护！");
        }
    }

    for (const auto& msg : issues) log("⚠ 对账: " + msg);
    if (issues.empty()) log("对账完成：本地仓位与交易所一致");
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

void CcgEngine::remove_all() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    bots_.clear();
}

std::vector<CcgBot> CcgEngine::get_bots() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<CcgBot> out;
    out.reserve(bots_.size());
    for (const auto& [id, b] : bots_) out.push_back(b);
    return out;
}

// ── 本 tick 生效的策略参数（在 tick 持锁中调用）───────────────────────────────
CcgEngine::EffParams CcgEngine::eff_params(const CcgBot& bot) const {
    EffParams p;
    p.interval_pct = bot.cfg.interval_pct;
    p.trail_entry  = bot.cfg.trail_entry;
    p.trail_tp     = bot.cfg.trail_tp;
    p.dyn   = false;
    p.fresh = true;
    if (!bot.cfg.dynamic_band_mode) return p;

    double W = dynparams::band_width_pct(bot.ind_boll_lb, bot.ind_boll_ub);
    if (!bot.ind_ok || W <= 0) {
        // 动态模式已开但还没有任何可用指标数据：参数保持配置值，标记数据过期
        p.dyn   = true;
        p.fresh = false;
        return p;
    }
    p.dyn          = true;
    p.fresh        = (host_.now_steady() - bot.ind_time) < kIndStale;
    p.interval_pct = dynparams::interval_pct(W);
    p.trail_entry  = dynparams::trail_entry_pct(W);
    p.trail_tp     = dynparams::trail_tp_pct(W);
    return p;
}

// 趋势空头态：补仓间隔放大（静态/动态模式都适用），在 eff_params 之上叠加
static double apply_trend_interval(const CcgBot& bot, double interval_pct,
                                   std::chrono::steady_clock::time_point now) {
    return trend_active_bearish(bot, now) ? interval_pct * kBearIntervalMult : interval_pct;
}

// ── 追踪变量更新（在 tick 持锁中调用）────────────────────────────────────────
void CcgEngine::update_tracking(CcgBot& bot, double price) {
    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);

    if (bot.entries.empty()) return;  // 尚未建仓，不用追踪

    const EffParams eff = eff_params(bot);
    const double eff_interval = apply_trend_interval(bot, eff.interval_pct, host_.now_steady());

    // ── DCA 间隔追踪 ──────────────────────────────────────────────────────────
    double interval_th = bot.last_entry_price *
        (is_long ? (1.0 - eff_interval / 100.0)
                 : (1.0 + eff_interval / 100.0));

    if (!bot.interval_hit) {
        bool triggered = is_long ? (price <= interval_th) : (price >= interval_th);
        if (eff.dyn) {
            // 动态W模式：补仓锚定布林带——除了跌够动态间隔，价格还必须在带外
            // （多：≤下轨；空：≥上轨），即"当前统计意义上的超卖/超买位"才武装补仓。
            // 指标数据过期时冻结武装（fresh=false），宁可错过不可乱买
            bool band_cond = eff.fresh &&
                (is_long ? (price <= bot.ind_boll_lb) : (price >= bot.ind_boll_ub));
            triggered = triggered && band_cond;
        }
        if (triggered) {
            bot.interval_hit = true;
            bot.dca_extreme  = price;
        }
    } else {
        // 到达间隔后追踪极值（多：跟踪最低点；空：跟踪最高点）
        bot.dca_extreme = is_long ? std::min(bot.dca_extreme, price)
                                  : std::max(bot.dca_extreme, price);
    }

    // ── 止盈追踪 ──────────────────────────────────────────────────────────────
    if (bot.avg_price > 0) {
        bool tp_hit;
        if (eff.dyn) {
            // 动态W模式：止盈锚定上轨（多）/下轨（空）+ 保底利润双条件。
            // 保底条款必须有：下跌趋势里上轨可能低于均价（高位库存拖的），
            // 只看"触上轨"会亏着平仓；保底保证每轮至少覆盖手续费+微利
            double floor_th = bot.avg_price *
                (is_long ? (1.0 + bot.cfg.min_profit_floor / 100.0)
                         : (1.0 - bot.cfg.min_profit_floor / 100.0));
            // v3.0 止盈锚定：够格阻力比上轨更近且仍在保底线之上时，在阻力前落袋
            // （不指望价格穿墙）。激活是一次性的，锚点天然锁定在激活时刻
            double target = is_long ? bot.ind_boll_ub : bot.ind_boll_lb;
            bool sr_fresh = bot.sr_ok &&
                (host_.now_steady() - bot.sr_time) < std::chrono::minutes(30);
            if (bot.cfg.use_sr_exit && sr_fresh) {
                if (is_long && bot.sr_res_lo > floor_th && bot.sr_res_lo < target)
                    target = bot.sr_res_lo;
                if (!is_long && bot.sr_sup_hi > 0 && bot.sr_sup_hi < floor_th &&
                    bot.sr_sup_hi > target)
                    target = bot.sr_sup_hi;
            }
            bool band_cond = eff.fresh &&
                (is_long ? (price >= target) : (price <= target));
            tp_hit = band_cond &&
                (is_long ? (price >= floor_th) : (price <= floor_th));
            if (tp_hit && !bot.tp_reached) bot.tp_anchor = target;
        } else {
            double tp_th = bot.avg_price *
                (is_long ? (1.0 + bot.cfg.tp_pct / 100.0)
                         : (1.0 - bot.cfg.tp_pct / 100.0));
            tp_hit = is_long ? (price >= tp_th) : (price <= tp_th);
        }
        if (!bot.tp_reached && tp_hit) {
            bot.tp_reached = true;
            bot.tp_extreme = price;
            if (eff.dyn) {
                // 影子对照：未启用阻力锚时，记录"若启用会更早在哪激活"供两周后对比
                std::string extra;
                if (!bot.cfg.use_sr_exit && bot.sr_ok &&
                    bot.sr_res_lo > bot.avg_price && bot.sr_res_lo < bot.ind_boll_ub)
                    extra = "（参考：若启用阻力锚将更早在 " +
                            std::to_string(bot.sr_res_lo) + " 激活）";
                log(bot.cfg.symbol + " 止盈追踪激活 @" + std::to_string(price) + extra);
            }
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
    const double trail_entry = eff_params(bot).trail_entry;
    double bounce_th = bot.dca_extreme *
        (is_long ? (1.0 + trail_entry / 100.0)
                 : (1.0 - trail_entry / 100.0));
    return is_long ? (price >= bounce_th) : (price <= bounce_th);
}

bool CcgEngine::should_close(const CcgBot& bot, double price) const {
    if (bot.entries.empty()) return false;
    if (!bot.tp_reached)     return false;

    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
    const EffParams eff = eff_params(bot);
    double trail_th = bot.tp_extreme *
        (is_long ? (1.0 - eff.trail_tp / 100.0)
                 : (1.0 + eff.trail_tp / 100.0));
    // 动态模式的"保底利润"必须贯穿到平仓端：极值刚过激活线就回落时，纯追踪
    // 阈值可能低于保底线（把赢单拖成亏单）——平仓线不得劣于保底线
    if (eff.dyn && bot.avg_price > 0) {
        double floor_th = bot.avg_price *
            (is_long ? (1.0 + bot.cfg.min_profit_floor / 100.0)
                     : (1.0 - bot.cfg.min_profit_floor / 100.0));
        trail_th = is_long ? std::max(trail_th, floor_th)
                           : std::min(trail_th, floor_th);
    }
    return is_long ? (price <= trail_th) : (price >= trail_th);
}

bool CcgEngine::should_stop_loss(const CcgBot& bot, double price) const {
    if (bot.entries.empty())       return false;
    if (bot.cfg.stop_loss_pct <= 0) return false;
    if (bot.avg_price <= 0)        return false;

    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
    double sl_th = bot.avg_price *
        (is_long ? (1.0 - bot.cfg.stop_loss_pct / 100.0)
                 : (1.0 + bot.cfg.stop_loss_pct / 100.0));
    return is_long ? (price <= sl_th) : (price >= sl_th);
}

// ── 主 tick（由 UI 定时器每 3 秒调用）────────────────────────────────────────
void CcgEngine::tick(const std::string& symbol, double price) {
    if (price <= 0) return;

    std::vector<std::string>                  do_entry;
    std::vector<std::pair<std::string,std::string>> do_close;  // {id, reason}
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto now = host_.now_wall();

        for (auto& [id, bot] : bots_) {
            if (bot.cfg.symbol != symbol) continue;
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

            // v3.0 结构性止损（仅多头+动态W模式）：价格持续跌破参考位（最深支撑下沿
            // -0.25×ATR）20个tick≈1分钟才触发——插针防护。影子模式只记录一次不平仓
            bool struct_stop_fire = false;
            if (!bot.entries.empty() && bot.cfg.direction == CcgConfig::Direction::Long &&
                bot.cfg.dynamic_band_mode && bot.sr_stop_level > 0 && bot.sr_ok &&
                (host_.now_steady() - bot.sr_time) < std::chrono::minutes(30)) {
                if (price < bot.sr_stop_level) {
                    ++bot.struct_stop_ticks;
                    if (bot.struct_stop_ticks == 20 && !bot.cfg.use_structural_stop)
                        log("[决策] " + bot.cfg.symbol + " 已持续跌破结构止损位 " +
                            std::to_string(bot.sr_stop_level) + "（影子：未启用结构止损，仅记录）");
                    if (bot.struct_stop_ticks >= 20 && bot.cfg.use_structural_stop)
                        struct_stop_fire = true;
                } else {
                    bot.struct_stop_ticks = 0;
                }
            }

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

                // v3.0 三层决策（宏观%B + 结构定位）：smart_gates=false 时纯影子——
                // 只在首仓真正派发时把判定快照写进日志；true 时新增两层真实拦截
                std::string decision_snap;
                if (can_enter) {
                    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
                    auto snow = host_.now_steady();

                    decision::Inputs din;
                    din.is_long     = is_long;
                    // 启用拦截时数据缺失同样不放行：宁可错过不可乱开
                    din.strict      = bot.cfg.smart_gates;
                    din.use_htf     = bot.cfg.use_htf_filter;
                    din.htf_ok      = bot.htf_ok && (snow - bot.htf_time) < std::chrono::minutes(30);
                    din.htf_pct_b   = bot.htf_pct_b;
                    din.htf_pos_max = bot.cfg.htf_pos_max;
                    din.use_sr      = bot.cfg.use_sr_gate;
                    din.sr_ok       = bot.sr_ok && (snow - bot.sr_time) < std::chrono::minutes(30);
                    din.at_support  = bot.sr_at_support;
                    // 预期止盈距离：动态模式=到上轨的距离（那就是利润目标）；静态=止盈%
                    double tp_dist;
                    if (bot.cfg.dynamic_band_mode && bot.ind_ok &&
                        (is_long ? bot.ind_boll_ub > price : bot.ind_boll_lb < price) &&
                        (is_long ? bot.ind_boll_ub - price : price - bot.ind_boll_lb) > 0) {
                        tp_dist = is_long ? (bot.ind_boll_ub - price) : (price - bot.ind_boll_lb);
                    } else {
                        tp_dist = price * std::max(bot.cfg.dynamic_band_mode ? 1.0
                                                                             : bot.cfg.tp_pct, 0.1) / 100.0;
                    }
                    // 净空：多头看上方阻力，空头镜像看下方支撑
                    double barrier = is_long ? bot.sr_res_lo
                                             : (bot.sr_sup_hi > 0 ? bot.sr_sup_hi : 0);
                    if (is_long) {
                        din.headroom = decision::headroom_ratio(price, barrier, tp_dist);
                    } else {
                        din.headroom = (barrier > 0 && barrier < price && tp_dist > 0)
                                       ? (price - barrier) / tp_dist : 1e9;
                    }
                    if (din.headroom < 0) din.headroom = 1e9;   // 数据非法按无限净空处理
                    din.headroom_min = bot.cfg.sr_headroom_ratio;

                    auto verdict  = decision::evaluate(din);
                    decision_snap = decision::summarize(din, verdict);

                    if (!verdict.pass() && bot.cfg.smart_gates) {
                        can_enter = false;
                        // 数据未就绪与条件不满足分开提示——前者是"等一等"，
                        // 后者是"这里不该买"，用户看日志时需要能区分
                        const char* why = verdict.data_block ? "三层数据未就绪，暂不开仓"
                                                             : "三层决策拦截";
                        if (bot.last_action != why) {
                            bot.last_action = why;
                            log(bot.cfg.symbol + " " + why + ": " + decision_snap +
                                (verdict.data_block
                                 ? "（数据到齐后自动放行；新上市品种需等日线21根+4h线60根历史）"
                                 : ""));
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
                    // v3.0 决策快照随首仓派发落日志（影子模式的数据积累点）
                    if (!decision_snap.empty())
                        log("[决策] " + bot.cfg.symbol + " 首仓派发 @" +
                            std::to_string(price) + " | " + decision_snap +
                            (bot.cfg.smart_gates ? "" : " (影子)"));
                }
            } else if (should_stop_loss(bot, price)) {
                // 硬止损优先于追踪止盈
                do_close.push_back({id, "硬止损"});
                bot.pending = true;
            } else if (struct_stop_fire) {
                do_close.push_back({id, "结构止损"});
                bot.pending = true;
            } else if (should_close(bot, price)) {
                do_close.push_back({id, "追踪止盈"});
                bot.pending = true;
            } else if (should_enter(bot, price)) {
                do_entry.push_back(id);
                bot.pending = true;
            }
        }
    }

    for (const auto& id         : do_entry) submit_entry(id);
    for (const auto& [id, reason]: do_close) submit_close(id, reason);
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

            // 首仓时设置杠杆
            if (level == 0) client_->set_leverage(cfg.symbol, cfg.leverage);

            const std::string side = (cfg.direction == CcgConfig::Direction::Long)
                                     ? "BUY" : "SELL";
            double qty = client_->round_qty(cfg.symbol, usdt / price);
            if (qty <= 0) {
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
            log("submit_entry 异常: " + std::string(e.what()));
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it != bots_.end()) {
                it->second.pending = false;
                it->second.inflight_margin = 0;
            }
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
            double closed_qty = 0;   // 实际平掉的数量（可能是部分成交）
            if (total_qty > 0) {
                const std::string side = (cfg.direction == CcgConfig::Direction::Long)
                                         ? "SELL" : "BUY";
                double qty = client_->round_qty(cfg.symbol, total_qty);
                auto r = client_->place_market_order(cfg.symbol, side, qty, true);
                if (!r.ok && r.error.find("[-2022]") != std::string::npos) {
                    // reduceOnly被拒 = 交易所侧没有可平的仓位（用户在交易所手动平过/
                    // 强平过）。本地留着这个幽灵仓位会陷入无限重试（追踪止盈/硬止损
                    // 每tick再触发-2022），且用户手动平仓也平不掉——按启动对账同款
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
                    bot.total_qty  = total_qty - closed_qty;
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
            log("submit_close 异常: " + std::string(e.what()));
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it != bots_.end()) it->second.pending = false;
        }
    });
}

} // namespace ccbot
