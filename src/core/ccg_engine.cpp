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

// ── 多周期梯子 ───────────────────────────────────────────────────────────────
// 四档固定 1h / 4h / 12h / 1d。各档新鲜度阈值不同：高周期的带算得晚一点无所谓，
// 低周期的带过几分钟就偏旧了
static constexpr std::chrono::seconds kMtfStale[4] = {
    std::chrono::seconds(300),    // 1h
    std::chrono::seconds(900),    // 4h
    std::chrono::seconds(1800),   // 12h
    std::chrono::seconds(3600),   // 1d
};
static const char* kMtfName[4] = { "1h", "4h", "12h", "1d" };

// 各档层数分配。手动填 "3,2,2,1" 优先；空则按 3:2:2:1 权重铺到 max_entries。
// 层数不足 4 时从最深的档往前砍——深档是"罕见事件才解锁"的保险层，
// 层数紧张时优先保证浅层有子弹
std::array<int,4> CcgEngine::mtf_tier_alloc(const CcgConfig& cfg) {
    const int n = std::max(1, std::min(cfg.max_entries, kMaxLayers));
    std::array<int,4> out{0,0,0,0};

    // ① 手动指定
    if (!cfg.mtf_tier_layers.empty()) {
        int idx = 0, cur = 0; bool has = false;
        for (char ch : cfg.mtf_tier_layers) {
            if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); has = true; }
            else if (has) { if (idx < 4) out[idx++] = cur; cur = 0; has = false; }
        }
        if (has && idx < 4) out[idx++] = cur;
        int sum = out[0] + out[1] + out[2] + out[3];
        if (sum > 0) {
            // 与 max_entries 对不齐时以 max_entries 为准，从最深档裁剪/补足
            for (int t = 3; t >= 0 && sum > n; --t) {
                int cut = std::min(out[t], sum - n);
                out[t] -= cut; sum -= cut;
            }
            if (sum < n) out[0] += (n - sum);
            return out;
        }
    }

    // ② 按 3:2:2:1 权重铺满
    static constexpr int wgt[4] = {3, 2, 2, 1};
    const int wsum = 8;
    int used = 0;
    for (int t = 0; t < 4; ++t) { out[t] = n * wgt[t] / wsum; used += out[t]; }
    for (int t = 0; t < 4 && used < n; ++t) { ++out[t]; ++used; }   // 余数补给浅档

    // ③ 层数不足以铺满四档时，从最深的档往前砍
    for (int t = 3; t >= 1; --t) {
        if (out[0] + out[1] + out[2] + out[3] <= n && out[t] > 0) break;
    }
    for (int t = 3; t >= 0; --t) {
        if (out[t] == 0) continue;
        int total = out[0] + out[1] + out[2] + out[3];
        if (total <= n) break;
        int cut = std::min(out[t], total - n);
        out[t] -= cut;
    }
    return out;
}

// 第 slot 个槽位（0-based）归属哪一档
static int mtf_tier_of_slot(const std::array<int,4>& alloc, int slot) {
    int acc = 0;
    for (int t = 0; t < 4; ++t) {
        acc += alloc[t];
        if (slot < acc) return t;
    }
    return 3;   // 超出分配（配置与层数对不齐）时归最深档，宁严勿松
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
    cfg.stop_loss_pct  = new_cfg.stop_loss_pct;
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
    cfg.dynamic_band_mode = new_cfg.dynamic_band_mode;
    cfg.dyn_interval_mult = new_cfg.dyn_interval_mult;
    cfg.dyn_fixed_interval = new_cfg.dyn_fixed_interval;
    cfg.mtf_ladder         = new_cfg.mtf_ladder;
    cfg.mtf_tier_layers    = new_cfg.mtf_tier_layers;
    cfg.mtf_k              = new_cfg.mtf_k;
    cfg.mtf_min_gap_pct    = new_cfg.mtf_min_gap_pct;
    cfg.min_profit_floor  = new_cfg.min_profit_floor;
    cfg.tp_floor_only     = new_cfg.tp_floor_only;
    cfg.tp_fixed_profit   = new_cfg.tp_fixed_profit;
    cfg.fixed_trail_tp    = new_cfg.fixed_trail_tp;
    cfg.first_entry_bounce_pct = new_cfg.first_entry_bounce_pct;
    cfg.use_trend_filter  = new_cfg.use_trend_filter;
    cfg.trend_interval    = new_cfg.trend_interval;
    cfg.trend_ema_period  = new_cfg.trend_ema_period;
    cfg.sr_radar          = new_cfg.sr_radar;
    cfg.sr_interval       = new_cfg.sr_interval;
    cfg.use_htf_filter      = new_cfg.use_htf_filter;
    cfg.htf_interval        = new_cfg.htf_interval;
    cfg.htf_pos_max         = new_cfg.htf_pos_max;
    cfg.use_sr_support      = new_cfg.use_sr_support;
    cfg.use_sr_headroom     = new_cfg.use_sr_headroom;
    cfg.sr_min_confluence   = new_cfg.sr_min_confluence;
    cfg.sr_res_min_conf     = new_cfg.sr_res_min_conf;
    cfg.sr_independent_conf = new_cfg.sr_independent_conf;
    cfg.sr_lower_half_only  = new_cfg.sr_lower_half_only;
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

void CcgEngine::set_market_bearish(bool bearish) {
    market_bearish_.store(bearish);
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

void CcgEngine::update_mtf_band(const std::string& bot_id, int tier, double lb, double ub) {
    if (tier < 0 || tier > 3 || lb <= 0 || ub <= lb) return;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(bot_id);
    if (it == bots_.end()) return;
    auto& b = it->second.mtf_band[tier];
    b.lb = lb; b.ub = ub; b.t = host_.now_steady();
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
    p.interval_pct = bot.cfg.dyn_fixed_interval > 0
                     ? bot.cfg.dyn_fixed_interval
                     : dynparams::interval_pct(W, bot.cfg.dyn_interval_mult);
    p.trail_entry  = dynparams::trail_entry_pct(W);
    p.trail_tp     = dynparams::trail_tp_pct(W);
    if (bot.cfg.fixed_trail_tp > 0) p.trail_tp = bot.cfg.fixed_trail_tp;
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
        if (bot.cfg.mtf_ladder) {
            // ── 多周期梯子：接管间距推导（止盈那半完全不动）──────────────────
            // 第 i 槽必须先跌破【它所属档位】的布林下轨，且相对上一笔成交价
            // 再跌够最小间距。两个条件都满足才武装追踪建仓。
            const auto alloc = mtf_tier_alloc(bot.cfg);
            const int  tier  = mtf_tier_of_slot(alloc, (int)bot.entries.size());
            const auto& tb   = bot.mtf_band[tier];
            const auto  age  = host_.now_steady() - tb.t;

            // 该档带数据不新鲜就冻结——宁可错过不可乱买（与动态W同原则）
            bool ok = (tb.lb > 0 && tb.ub > tb.lb && age < kMtfStale[tier]);

            // ① 跌破该档下轨（实时价判定，与动态W一致；追踪建仓那步本身防抖）
            if (ok) ok = is_long ? (price <= tb.lb) : (price >= tb.ub);

            // ② 最小间距 = max(k × 该档带宽, 兜底地板)。
            //    瀑布是高波动事件，带子撑开时地板跟着撑开，正好挡住"四档同时
            //    触发、整个梯子打在崩盘顶部"——这是纯固定百分比挡不住的
            if (ok) {
                const double mb = (tb.ub + tb.lb) * 0.5;
                const double bandw = (mb > 0) ? (tb.ub - tb.lb) / mb * 100.0 : 0.0;
                double gap = std::max(bot.cfg.mtf_k * bandw,
                                      bot.cfg.mtf_min_gap_pct);
                // 趋势过滤的"空头态间隔×1.5"必须同样作用在梯子的间距上。
                // 此前这里直接用 gap 覆盖了 triggered，而 ×1.5 是烘焙在 interval_th
                // 里的——等于开了多周期梯子，趋势过滤的补仓那一半就【静默失效】了。
                // 趋势过滤是个独立勾选项，用户会以为它还在工作；两个功能各自都对，
                // 组合起来其中一个悄悄不算数，是最难发现的那类缺陷
                gap = apply_trend_interval(bot, gap, host_.now_steady());
                const double gap_th = bot.last_entry_price *
                    (is_long ? (1.0 - gap / 100.0) : (1.0 + gap / 100.0));
                ok = is_long ? (price <= gap_th) : (price >= gap_th);
            }
            triggered = ok;
        } else if (eff.dyn) {
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
            // tp_floor_only：不等上轨，盈利达标即可激活（"够本就跑"）
            bool band_cond = eff.fresh &&
                (bot.cfg.tp_floor_only ||
                 (is_long ? (price >= target) : (price <= target)));
            // 路径B：够到固定利润线就收割，不等上轨（与路径A 是【或】关系）
            bool fixed_cond = false;
            if (bot.cfg.tp_fixed_profit > 0 && bot.avg_price > 0 && eff.fresh) {
                double fx = bot.avg_price *
                    (is_long ? (1.0 + bot.cfg.tp_fixed_profit / 100.0)
                             : (1.0 - bot.cfg.tp_fixed_profit / 100.0));
                fixed_cond = is_long ? (price >= fx) : (price <= fx);
            }
            tp_hit = (band_cond &&
                      (is_long ? (price >= floor_th) : (price <= floor_th)))
                     || fixed_cond;
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
                log(bot.cfg.symbol + " 止盈追踪激活 @" + std::to_string(price));
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
    double trail_entry = eff_params(bot).trail_entry;

    // 多周期梯子：反弹比例必须跟着【当前槽位所属档位】的带宽走。
    // 否则深层档位会失效——1d 档的带宽约是 1h 的 5 倍，用 1h 推出来的反弹比例
    // 去等一个日线级别的反转，随便一个小反弹就把仓位打进去了，"追踪建仓"形同虚设
    if (bot.cfg.mtf_ladder) {
        const auto alloc = mtf_tier_alloc(bot.cfg);
        const int  tier  = mtf_tier_of_slot(alloc, (int)bot.entries.size());
        const auto& tb   = bot.mtf_band[tier];
        const double mb  = (tb.ub + tb.lb) * 0.5;
        if (mb > 0 && tb.ub > tb.lb) {
            const double bandw = (tb.ub - tb.lb) / mb * 100.0;
            // 用梯子专用的夹逼上限。共用 trail_entry_pct 的话，4h/12h/1d 三档会
            // 全部撞在 0.4% 的上限上变成同一个值，本函数上面那段注释想避免的
            // "小反弹就把深层打进去"照样发生——见 mtf_trail_entry_pct 的说明
            trail_entry = dynparams::mtf_trail_entry_pct(bandw);
        }
    }

    double bounce_th = bot.dca_extreme *
        (is_long ? (1.0 + trail_entry / 100.0)
                 : (1.0 - trail_entry / 100.0));
    return is_long ? (price >= bounce_th) : (price <= bounce_th);
}

// 补仓侧闸门：只作用于"深层"补仓（占资金大头且必然在暴跌中触发的那几层）。
// 数据缺失一律放行——补仓闸门是减仓保护而非入场许可，断数据时卡住补仓会让
// 已有仓位失去摊薄能力，风险方向反而更糟
bool CcgEngine::dca_gate_blocked(const CcgBot& bot,
                                 std::chrono::steady_clock::time_point now) const {
    const int from = bot.cfg.dca_gate_from_layer;
    if (from <= 0) return false;
    // entries.size() 是已有层数，下一笔是第 size()+1 层
    if ((int)bot.entries.size() + 1 < from) return false;

    if (bot.cfg.dca_gate_trend && trend_active_bearish(bot, now)) return true;
    if (bot.cfg.dca_gate_htf_min > 0) {
        bool htf_fresh = bot.htf_ok && (now - bot.htf_time) < std::chrono::minutes(30);
        if (htf_fresh && bot.htf_pct_b < bot.cfg.dca_gate_htf_min) return true;
    }
    return false;
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
                bot.band_broken = false; bot.band_extreme = 0;   // 首仓追踪基准同样清零
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
            // -0.25×ATR）20个tick≈1分钟才触发——插针防护。未启用时不再打影子日志
            bool struct_stop_fire = false;
            if (!bot.entries.empty() && bot.cfg.direction == CcgConfig::Direction::Long &&
                bot.cfg.dynamic_band_mode && bot.sr_stop_level > 0 && bot.sr_ok &&
                (host_.now_steady() - bot.sr_time) < std::chrono::minutes(30)) {
                if (price < bot.sr_stop_level) {
                    ++bot.struct_stop_ticks;
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
                    // 首仓追踪建仓：破轨后记极值，自极值反弹够比例才开——等企稳不接刀。
                    // ⚠ 反弹条件是【叠加】在超卖区之上的，不是替换：价格必须仍在中轨
                    // 下方才算数。否则破轨一次就永久放行，价格涨到上轨附近照样开仓，
                    // 等于把下轨过滤器整个关掉（曾经的实现缺陷，实测会让开仓数翻2.6倍）
                    if (bot.cfg.first_entry_bounce_pct > 0) {
                        const double mid = (bot.ind_boll_lb + bot.ind_boll_ub) * 0.5;
                        // 价格已回到中轨另一侧仍未触发 → 本轮超卖结束，解除武装重新等
                        if (is_long ? (price > mid) : (price < mid)) {
                            bot.band_broken = false;
                            bot.band_extreme = 0;
                        }
                        if (priceCond) {
                            bot.band_broken = true;
                            bot.band_extreme = (bot.band_extreme <= 0)
                                ? price
                                : (is_long ? std::min(bot.band_extreme, price)
                                           : std::max(bot.band_extreme, price));
                        }
                        if (bot.band_broken && bot.band_extreme > 0) {
                            double bth = bot.band_extreme *
                                (is_long ? (1.0 + bot.cfg.first_entry_bounce_pct / 100.0)
                                         : (1.0 - bot.cfg.first_entry_bounce_pct / 100.0));
                            // 反弹达标 且 仍在中轨下方（超卖区内）才开
                            priceCond = (is_long ? (price >= bth && price <= mid)
                                                 : (price <= bth && price >= mid));
                        } else {
                            priceCond = false;
                        }
                    }
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

                // 周期熊市总开关：BTC级别判定为熊市时全场暂停开新首仓。
                // 只拦【新首仓】——已有仓位的补仓/止盈/止损一律不受影响，否则
                // 等于在熊市里既不让摊薄也不让离场，是最糟的组合
                if (can_enter && bot.cfg.use_cycle_bear_switch && market_bearish_.load()) {
                    can_enter = false;
                    if (bot.last_action != "周期熊市，全场暂停开首仓") {
                        bot.last_action = "周期熊市，全场暂停开首仓";
                        log(bot.cfg.symbol + " 周期熊市态，暂停开新首仓（BTC日线级别，转牛后自动放行）");
                    }
                }

                // v3.0 三层决策（宏观%B + 结构定位）。
                // 三个判据平级独立，任一开启才走判定；全关则完全不参与
                // （不判定、不记录，也不空跑去刷日志）
                const bool gates_on = bot.cfg.use_htf_filter ||
                                      bot.cfg.use_sr_support || bot.cfg.use_sr_headroom;
                std::string decision_snap;
                if (can_enter && gates_on) {
                    const bool is_long = (bot.cfg.direction == CcgConfig::Direction::Long);
                    auto snow = host_.now_steady();

                    decision::Inputs din;
                    din.is_long     = is_long;
                    // 数据缺失不放行：宁可错过不可乱开
                    din.strict      = true;
                    din.use_htf     = bot.cfg.use_htf_filter;
                    din.htf_ok      = bot.htf_ok && (snow - bot.htf_time) < std::chrono::minutes(30);
                    din.htf_pct_b   = bot.htf_pct_b;
                    din.htf_pos_max = bot.cfg.htf_pos_max;
                    din.use_sr_support  = bot.cfg.use_sr_support;
                    din.use_sr_headroom = bot.cfg.use_sr_headroom;
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
                    // 真实止盈距离：动态W模式还要满足"价格≥均价×(1+保底利润)"，
                    // 首仓时均价即入场价，所以保底线就是 price×(1+floor)。两个条件
                    // 取【更远】的那个才是真正要走的路
                    if (bot.cfg.sr_headroom_true_tp && bot.cfg.dynamic_band_mode &&
                        bot.cfg.min_profit_floor > 0) {
                        tp_dist = std::max(tp_dist,
                                           price * bot.cfg.min_profit_floor / 100.0);
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
                    // 每次判定都刷新（不管放行还是拦截），界面据此显示实时判据。
                    // 与 last_action 分开：那个是去重键，必须保持稳定
                    bot.last_decision = decision_snap;

                    if (!verdict.pass()) {
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
                    // 决策快照随首仓派发落日志
                    if (!decision_snap.empty())
                        // 保留：一轮只打一次，是事后复盘"这单当初凭什么开"的唯一依据
                        log("[决策] " + bot.cfg.symbol + " 首仓派发 @" +
                            std::to_string(price) + " | " + decision_snap);
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
                } else if (dca_gate_blocked(bot, host_.now_steady())) {
                    std::string why = "第" + std::to_string(bot.entries.size() + 1) +
                                      "层补仓被闸门拦下（深层资金保护）";
                    if (bot.last_action != why) {
                        bot.last_action = why;
                        log(bot.cfg.symbol + " " + why);
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
            log("submit_close 异常: " + std::string(e.what()));
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(bot_id);
            if (it != bots_.end()) it->second.pending = false;
        }
    });
}

} // namespace ccbot
