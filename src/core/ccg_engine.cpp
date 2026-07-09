#include "core/ccg_engine.h"
#include "net/trading_client.h"
#include "core/thread_pool.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace ccbot {

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
CcgEngine::CcgEngine(std::shared_ptr<TradingClient> client,
                     std::shared_ptr<ThreadPool>    pool)
    : client_(std::move(client)), pool_(std::move(pool)) {}

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
std::string CcgEngine::add_bot(const CcgConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);

    // 防止同品种同方向重复添加（已停止的可以重新添加）
    for (const auto& [id, b] : bots_) {
        if (b.cfg.symbol    == cfg.symbol &&
            b.cfg.direction == cfg.direction &&
            b.state         != CcgBot::State::Stopped) {
            log(cfg.symbol + " 已有运行中/冷却中的相同方向Bot，跳过重复添加");
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
    bot.start_time = std::chrono::system_clock::now();
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
    snapshot.start_time = std::chrono::system_clock::now();

    std::string id = snapshot.bot_id;
    bots_[id] = std::move(snapshot);
    return id;
}

bool CcgEngine::update_bot_cfg(const std::string& id, const CcgConfig& new_cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return false;
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
    return true;
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
    }
    return sum;
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
    bots_.erase(id);
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

// ── 追踪变量更新（在 tick 持锁中调用）────────────────────────────────────────
void CcgEngine::update_tracking(CcgBot& bot, double price) {
    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);

    if (bot.entries.empty()) return;  // 尚未建仓，不用追踪

    // ── DCA 间隔追踪 ──────────────────────────────────────────────────────────
    double interval_th = bot.last_entry_price *
        (is_long ? (1.0 - bot.cfg.interval_pct / 100.0)
                 : (1.0 + bot.cfg.interval_pct / 100.0));

    if (!bot.interval_hit) {
        bool triggered = is_long ? (price <= interval_th) : (price >= interval_th);
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
        double tp_th = bot.avg_price *
            (is_long ? (1.0 + bot.cfg.tp_pct / 100.0)
                     : (1.0 - bot.cfg.tp_pct / 100.0));

        bool tp_hit = is_long ? (price >= tp_th) : (price <= tp_th);
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
    double bounce_th = bot.dca_extreme *
        (is_long ? (1.0 + bot.cfg.trail_entry / 100.0)
                 : (1.0 - bot.cfg.trail_entry / 100.0));
    return is_long ? (price >= bounce_th) : (price <= bounce_th);
}

bool CcgEngine::should_close(const CcgBot& bot, double price) const {
    if (bot.entries.empty()) return false;
    if (!bot.tp_reached)     return false;

    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
    double trail_th = bot.tp_extreme *
        (is_long ? (1.0 - bot.cfg.trail_tp / 100.0)
                 : (1.0 + bot.cfg.trail_tp / 100.0));
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
        auto now = std::chrono::system_clock::now();

        for (auto& [id, bot] : bots_) {
            if (bot.cfg.symbol != symbol) continue;
            if (bot.pending) continue;

            bot.current_price = price;

            if (bot.state == CcgBot::State::Cooldown) {
                if (now >= bot.cooldown_until) {
                    // 冷却结束，重置状态重新开始
                    bot.entries.clear();
                    bot.avg_price = bot.total_qty = bot.total_cost = 0;
                    bot.interval_hit = bot.tp_reached = false;
                    bot.ind_dipped  = false;   // 新一轮等待信号，探底状态清零重新累积
                    bot.last_entry_price = price;
                    bot.dca_extreme = price;
                    bot.tp_extreme  = price;
                    bot.last_action = "重启中";
                    bot.state = CcgBot::State::Running;
                    do_entry.push_back(id);
                    bot.pending = true;
                }
                continue;
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
                    can_enter = bot.ind_ok && priceCond && rsiCond;
                }
                // 账户级总保证金上限：只挡"开新首仓"，已有仓位的加仓/止盈止损不受影响
                double cap = max_total_margin_.load();
                if (can_enter && cap > 0) {
                    auto sizes = entry_usdt(bot.cfg);
                    double first_margin = (!sizes.empty() && bot.cfg.leverage > 0)
                        ? sizes[0] / bot.cfg.leverage : 0;
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
                    do_entry.push_back(id);
                    bot.pending = true;
                }
            } else if (should_stop_loss(bot, price)) {
                // 硬止损优先于追踪止盈
                do_close.push_back({id, "硬止损"});
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
    pool_->submit([this, bot_id]() {
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
                    it->second.last_action = "数量不足，跳过";
                }
                log(cfg.symbol + " 第" + std::to_string(level+1) + "仓数量不足");
                return;
            }

            auto r = client_->place_market(cfg.symbol, side, qty, false);

            // 更新状态
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it == bots_.end()) return;
            auto& bot = it->second;
            bot.pending = false;

            if (r.ok) {
                // 优先用交易所返回的实际成交均价/数量；下单前的快照价只做兜底
                // （否则 qty 按 lot size 取整后，用下单前的目标 usdt 算均价会systematic 偏差）
                double fill_price = (r.avg_price    > 0) ? r.avg_price    : price;
                double fill_qty   = (r.executed_qty > 0) ? r.executed_qty : qty;

                CcgEntry e;
                e.level     = level;
                e.price     = fill_price;
                e.qty       = fill_qty;
                e.cost_usdt = fill_qty * fill_price;
                e.order_id  = r.order_id;
                e.time      = std::chrono::system_clock::now();
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
            } else {
                log(cfg.symbol + " 第" + std::to_string(level+1) + "仓失败: " + r.error);
            }
        } catch (const std::exception& e) {
            log("submit_entry 异常: " + std::string(e.what()));
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it != bots_.end()) it->second.pending = false;
        }
    });
}

// ── 异步平仓 ──────────────────────────────────────────────────────────────────
void CcgEngine::submit_close(const std::string& bot_id, const std::string& reason) {
    pool_->submit([this, bot_id, reason]() {
        try {
            CcgConfig cfg;
            double    total_qty = 0, avg_price = 0, close_price = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(bot_id);
                if (it == bots_.end()) return;
                const auto& bot = it->second;
                cfg         = bot.cfg;
                total_qty   = bot.total_qty;
                avg_price   = bot.avg_price;
                close_price = bot.current_price;
            }

            bool closed_ok = false;
            if (total_qty > 0) {
                const std::string side = (cfg.direction == CcgConfig::Direction::Long)
                                         ? "SELL" : "BUY";
                double qty = client_->round_qty(cfg.symbol, total_qty);
                auto r = client_->place_market(cfg.symbol, side, qty, true);
                closed_ok = r.ok;
                if (r.ok && r.avg_price > 0) close_price = r.avg_price;  // 用真实成交均价算PnL
                if (!r.ok)
                    log(cfg.symbol + " 平仓失败: " + r.error);
            } else {
                closed_ok = true;  // 无持仓，视为已平
            }

            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it == bots_.end()) return;
            auto& bot = it->second;
            bot.pending = false;

            if (closed_ok) {
                const bool is_long = (cfg.direction == CcgConfig::Direction::Long);
                double pnl = total_qty > 0
                    ? (is_long ? (close_price - avg_price) : (avg_price - close_price)) * total_qty
                    : 0.0;
                bot.realized_pnl += pnl;
                bot.cycle_count++;

                if (total_qty > 0 && trade_cb_) {
                    TradeRecord tr;
                    tr.symbol      = cfg.symbol;
                    tr.direction   = cfg.direction;
                    tr.entry_price = avg_price;
                    tr.exit_price  = close_price;
                    tr.qty         = total_qty;
                    tr.pnl         = pnl;
                    tr.reason      = reason;
                    tr.close_time  = std::chrono::system_clock::now();
                    trade_cb_(tr);
                }

                std::ostringstream ss;
                ss << cfg.symbol << " " << reason
                   << " 均=$" << std::fixed << std::setprecision(4) << avg_price
                   << " 收=$" << close_price
                   << " P&L=" << std::setprecision(2) << pnl << "U"
                   << " 累计=" << bot.realized_pnl << "U";
                log(ss.str());

                bot.entries.clear();
                bot.total_qty = bot.total_cost = bot.avg_price = 0;
                bot.interval_hit = bot.tp_reached = false;
                bot.ind_dipped  = false;   // 平仓后如果还会再等信号，探底状态清零重新累积
                bot.last_entry_price = 0;
                bot.last_action = reason;

                if (cfg.auto_restart) {
                    bot.cooldown_until = std::chrono::system_clock::now() +
                                         std::chrono::seconds(cfg.cooldown_secs);
                    bot.state = CcgBot::State::Cooldown;
                    log(cfg.symbol + " 冷却 " + std::to_string(cfg.cooldown_secs) + "s 后重启");
                } else {
                    bot.state = CcgBot::State::Stopped;
                }
            }
        } catch (const std::exception& e) {
            log("submit_close 异常: " + std::string(e.what()));
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it != bots_.end()) it->second.pending = false;
        }
    });
}

} // namespace ccbot
