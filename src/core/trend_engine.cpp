#include "core/trend_engine.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace ccbot {

namespace {
std::string fmt(double v, int prec = 4) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}
const char* side_of(trend::Pos p, bool closing) {
    // 开仓：多=BUY 空=SELL；平仓方向相反
    const bool buy = closing ? (p == trend::Pos::Short) : (p == trend::Pos::Long);
    return buy ? "BUY" : "SELL";
}
} // namespace

TrendEngine::TrendEngine(std::shared_ptr<ITradingClient> client,
                     std::shared_ptr<ThreadPool> pool)
    : client_(std::move(client)), pool_(std::move(pool)) {
    auto p = pool_;
    host_.submit = [p](std::function<void()> fn) {
        if (p) p->submit(std::move(fn)); else fn();
    };
}

TrendEngine::TrendEngine(std::shared_ptr<ITradingClient> client, EngineHost host)
    : client_(std::move(client)), host_(std::move(host)) {
    if (!host_.submit) host_.submit = [](std::function<void()> fn) { fn(); };
}

void TrendEngine::log(const std::string& msg) const {
    if (log_cb_) log_cb_(msg);
}

std::string TrendEngine::add_bot(const TrendConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (const auto& kv : bots_)
        if (kv.second.cfg.symbol == cfg.symbol &&
            kv.second.state != TrendBot::State::Stopped)
            return {};

    TrendBot b;
    // bot_id 前缀保留 "sar"：它【落盘】，也是界面操作列去抖键的一部分。
    // 改前缀之后新建的 bot 与从落盘恢复的 bot 会是两种 id 格式，而这带来的
    // 好处是零——引擎改名是为了消除"一个叫 Sar 的类装着三个策略"的歧义，
    // 一个不可见的 id 前缀不在这个问题里
    b.bot_id     = "sar" + std::to_string(++seq_);
    b.cfg        = cfg;
    b.start_time = host_.now_wall();
    b.last_action = "等待信号";
    bots_[b.bot_id] = b;
    log(cfg.symbol + " SAR bot 已创建（通道" +
        std::to_string(cfg.rule.donchian_period) + " / ATR" +
        std::to_string(cfg.rule.atr_period) + " / k=" + fmt(cfg.rule.atr_mult, 1) + "）");
    return b.bot_id;
}

std::string TrendEngine::restore_bot(TrendBot snap) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (const auto& kv : bots_)
        if (kv.second.cfg.symbol == snap.cfg.symbol &&
            kv.second.state != TrendBot::State::Stopped)
            return {};

    snap.bot_id  = "sar" + std::to_string(++seq_);
    snap.pending = false;              // 落盘时的在途标记一律作废
    // 信号一律作废重拉：落盘的 ATR 可能是几小时前的，而止损线的距离由它决定。
    // sig_ok=false 时 trend::step 会沿用已有的止损线、不开新仓——正是想要的
    snap.sig_ok  = false;
    snap.atr = snap.atr_pct = 0;
    snap.dc_ok = false;
    snap.sig_time = {};
    // 冷却与"这根K线数过没有"跨重启都失去意义：bar_open_ms 对不上新拉的K线
    snap.bar_open_ms = snap.last_counted_bar_ms = 0;
    if (snap.start_time.time_since_epoch().count() == 0)
        snap.start_time = host_.now_wall();

    const std::string id = snap.bot_id;
    bots_[id] = std::move(snap);
    const auto& b = bots_[id];
    if (b.st.pos != trend::Pos::Flat)
        log(b.cfg.symbol + " SAR 从落盘恢复：持" + trend::pos_name(b.st.pos) +
            " qty=" + fmt(b.qty, 8) + " 开仓价=$" + fmt(b.st.entry_price) +
            " 止损线=$" + fmt(b.st.stop));
    else
        log(b.cfg.symbol + " SAR 从落盘恢复：空仓");
    return id;
}

std::vector<std::string> TrendEngine::reconcile_positions(
        const std::vector<ExchangePos>& exchange) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<std::string> issues;

    for (auto& kv : bots_) {
        auto& b = kv.second;
        // 在途的跳过：订单可能已在交易所生效而本地还没入账，此刻比对必然误判
        if (b.pending) continue;

        const bool local_has = (b.st.pos != trend::Pos::Flat && b.qty > 0);
        const int  local_dir = (b.st.pos == trend::Pos::Long) ? 1 : -1;

        // ⚠ 必须按【品种 + 方向】配对，不能只按品种取第一条命中。
        //
        // 调用方传进来的快照是从 pos_cache_ 摊平来的，而那是一个以
        // symbol+"_L"/"_S" 为键的 unordered_map——双向持仓模式下同一个品种会有
        // 两条记录（多腿一条、空腿一条），而 unordered_map 的遍历顺序是不确定的。
        // 只按品种取第一条，拿到的可能是【反向】那条，于是本地持多、比到交易所
        // 的空腿，报成"方向不一致"并停掉 bot——而 bot 一停，那笔仓位就真的没人
        // 管了：止损线不再推进、触线不再平仓。一次误判换来一个裸敞口。
        //
        // 实盘日志里的 "SAR对账: ZECUSDT 本地方向(多)与交易所(空)不一致" 就是这条。
        // CcgEngine::reconcile_positions 一直是按 (品种, 方向) 配对的，这里漏了。
        const ExchangePos* same = nullptr;   // 与本地同向的那条
        const ExchangePos* opp  = nullptr;   // 反向的那条（真正的方向冲突才看它）
        for (const auto& e : exchange) {
            if (e.symbol != b.cfg.symbol || !(e.qty > 0)) continue;
            if (!local_has) { if (!same) same = &e; continue; }   // 空仓：任一条都算"交易所有仓"
            if (e.direction == local_dir) { if (!same) same = &e; }
            else                          { if (!opp)  opp  = &e; }
        }
        const ExchangePos* ex = same;

        if (!local_has && !ex) continue;               // 两边都空，一致

        if (!local_has && ex) {
            // 孤儿仓：最危险的一种。不停的话，下一个突破信号会再开一笔，
            // 而交易所上那笔无人管理——净敞口翻倍且没有任何止损线守着
            issues.push_back(b.cfg.symbol + " 交易所有仓位(" +
                             std::string(ex->direction > 0 ? "多" : "空") + " " +
                             fmt(ex->qty, 8) + ")但本地无跟踪，已停止该bot");
            b.state = TrendBot::State::Stopped;
            b.last_action = "⚠ 孤儿仓，已停止待核对";
            continue;
        }

        if (local_has && !ex) {
            // 同向的那条没有。若反向【有】仓，那是真正的方向冲突——本地以为持多、
            // 交易所实际持空，任何自动收敛都是在猜，停下来让人看。
            // 反向也没有，才是"外部已平/被强平"
            if (opp) {
                issues.push_back(b.cfg.symbol + " 本地方向(" + trend::pos_name(b.st.pos) +
                                 ")与交易所(" + (opp->direction > 0 ? "多" : "空") +
                                 " " + fmt(opp->qty, 8) + ")不一致，已停止该bot");
                b.state = TrendBot::State::Stopped;
                b.last_action = "⚠ 方向不一致，已停止";
                continue;
            }
            issues.push_back(b.cfg.symbol + " 本地有仓位但交易所没有（外部已平/被强平），"
                             "已清空本地状态并停止该bot");
            trend::on_closed(b.st);
            clear_ds_runtime(b);
            // 这条路径【没有】撤单动作（对账在锁内，不能做 HTTP），所以要自己
            // 把单号清掉。不清的话 try_place_hard_stop 会因为"已有单号"而永远
            // 不再挂新单——静默失去保护。
            // 代价是交易所那张单可能残留：但走到这里说明仓位已被外部平掉，
            // 而 closePosition 单在仓位归零后本就不会再成交
            b.disaster_stop_id.clear();
            b.disaster_stop_price = 0;
            b.qty = 0;
            b.state = TrendBot::State::Stopped;
            b.last_action = "⚠ 交易所侧已无仓位，已停止";
            continue;
        }

        // 走到这里 ex 必然与本地同向（配对时就是按方向挑的），只剩数量要比。
        // 双向模式下反向腿可能同时存在，那是另一套东西开的（同品种只允许一套策略，
        // 所以不是本引擎的仓），不在这里管——它由 DCA 侧的孤儿仓核查或人工处理
        const double diff = b.qty - ex->qty;
        if (diff > 1e-12) {
            issues.push_back(b.cfg.symbol + " 外部部分平仓：本地 " + fmt(b.qty, 8) +
                             " → 交易所 " + fmt(ex->qty, 8) + "，已收敛");
            b.qty = ex->qty;   // 开仓价与止损线保留，它们仍然成立
        } else if (diff < -1e-12) {
            // 只告警不动：外部手动加仓的话，按交易所数量接管等于让止损线
            // 去管一笔不是自己开的仓，开仓价基准也不再成立
            issues.push_back(b.cfg.symbol + " 交易所持仓(" + fmt(ex->qty, 8) +
                             ")多于本地跟踪(" + fmt(b.qty, 8) +
                             ")，可能有外部加仓，本地状态未改动");
        }
    }
    return issues;
}

void TrendEngine::stop_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    it->second.state = TrendBot::State::Stopped;
    it->second.last_action = "已暂停";
}

void TrendEngine::resume_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    it->second.state = TrendBot::State::Running;
    it->second.last_action = "已恢复";
}

void TrendEngine::close_bot(const std::string& id) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(id);
        if (it == bots_.end() || it->second.pending) return;
        if (it->second.st.pos == trend::Pos::Flat || it->second.qty <= 0) return;
        it->second.pending = true;
    }
    submit_close(id, "手动平仓", trend::Pos::Flat);
}

void TrendEngine::remove_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    bots_.erase(id);
}

void TrendEngine::stop_all() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (auto& kv : bots_) {
        kv.second.state = TrendBot::State::Stopped;
        kv.second.last_action = "已暂停";
    }
}

std::vector<TrendBot> TrendEngine::get_bots() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<TrendBot> out;
    out.reserve(bots_.size());
    for (const auto& kv : bots_) out.push_back(kv.second);
    return out;
}

void TrendEngine::set_pending_for_test(const std::string& id, bool v) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end()) it->second.pending = v;
}

void TrendEngine::update_signal(const std::string& id, double atr, double atr_pct,
                              bool dc_ok, double dc_up, double dc_dn,
                              int64_t bar_open_ms) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    auto& b = it->second;
    b.atr     = atr;
    b.atr_pct = atr_pct;
    b.dc_ok   = dc_ok;
    b.dc_up   = dc_up;
    b.dc_dn   = dc_dn;
    b.sig_time = host_.now_steady();
    // ⚠ 只有海龟的 sig_ok 由这里决定。裸K 与 抛物线SAR 根本不算通道也不用 ATR，
    //   它们的 sig_ok 由 update_bars 设——这里若一并覆盖，会把刚设好的 true
    //   打回 false，表现为"永远数据不足、一单不开"
    if (b.cfg.rule.strategy == trend::Strategy::Turtle)
        b.sig_ok = (atr > 0 && dc_ok);
    // bar_open_ms 变了 = 跨入新K线。冷却按K线计数，不按 tick——
    // 3 秒一个 tick 的话，"冷却3根4h线"会在 9 秒内走完，等于没有冷却
    if (bar_open_ms > 0 && bar_open_ms != b.bar_open_ms) {
        b.bar_open_ms = bar_open_ms;   // 实际的冷却递减在 tick 里，见 last_counted_bar_ms
    }
}

int TrendEngine::signal_period_sec(const std::string& interval) {
    return std::max(15, std::min(60, TrendConfig::bar_seconds(interval) / 4));
}

void TrendEngine::update_bars(const std::string& id, const BarSnap& s) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    auto& b = it->second;
    b.bar_open        = s.open;
    b.prev_close      = s.prev_close;
    b.prev_high       = s.prev_high;
    b.prev_low        = s.prev_low;
    b.psar_prev_high  = s.prev_high;
    b.psar_prev_low   = s.prev_low;
    b.psar_prev2_high = s.prev2_high;
    b.psar_prev2_low  = s.prev2_low;
    b.swing_low       = s.swing_low;
    b.swing_high      = s.swing_high;
    // "K线数据就绪"必须按【这个策略真正用到的字段】判定，不能一刀切成
    // "前一根高低点都在"。③ 盘中即时入场只用本根开盘价，前一根的高低点对它
    // 毫无意义——一刀切的话，数据层哪天少给一个它用不到的字段，这个 bot 就会
    // 静默地一单也不开，而配置、日志、界面全都正常
    if (b.cfg.rule.strategy == trend::Strategy::BareK &&
        b.cfg.rule.bare_entry == trend::BareEntry::Immediate)
        b.bar_ok = (s.open > 0);
    else
        b.bar_ok = (s.prev_high > 0 && s.prev_low > 0);
    b.sig_time        = host_.now_steady();
    // 裸K 与 抛物线SAR 都不需要 ATR，所以它们的 sig_ok 只看K线本身。
    // 海龟的 sig_ok 归 update_signal 管（它要 ATR + 通道），这里不碰
    if (b.cfg.rule.strategy != trend::Strategy::Turtle) b.sig_ok = b.bar_ok;
    if (s.bar_open_ms > 0 && s.bar_open_ms != b.bar_open_ms) b.bar_open_ms = s.bar_open_ms;
}

double TrendEngine::plan_qty(const TrendConfig& cfg, double price,
                           double stop_price) const {
    if (!std::isfinite(price) || price <= 0) return 0;

    double notional = cfg.budget_usdt;
    if (cfg.size_mode == TrendConfig::SizeMode::RiskBased) {
        // 止损时亏的钱 = 数量 × |现价 − 止损线|，令它等于 risk_usdt 反推数量。
        // 用真实止损线而不是 k×ATR：裸K线模式的止损是摆动低点，与 ATR 无关
        const double stop_dist = std::fabs(price - stop_price);
        if (!(stop_price > 0) || !(stop_dist > 0) || !(cfg.risk_usdt > 0)) return 0;
        notional = cfg.risk_usdt * price / stop_dist;
        // 名义上限：ATR 极小时上面那个除法会算出荒谬的大仓位。
        // 没有这道帽子，一个刚上市、K线还平着的品种能把整个账户吃掉
        if (cfg.budget_usdt > 0) notional = std::min(notional, cfg.budget_usdt);
    }
    if (!std::isfinite(notional) || notional <= 0) return 0;

    double q = client_->round_qty(cfg.symbol, notional / price);
    return (std::isfinite(q) && q > 0) ? q : 0;
}

void TrendEngine::set_max_total_margin(double usdt) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    max_total_margin_ = (std::isfinite(usdt) && usdt > 0) ? usdt : 0.0;
}

void TrendEngine::set_max_open_positions(int n) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    max_open_positions_ = (n > 0) ? n : 0;
}

// 账户级闸门。返回空串 = 放行，否则是给人看的拦截原因。
// ⚠ 调用方必须已持 mtx_（它要遍历 bots_）。声明在 tick 的决策循环内部调用，
//   那里本来就持着锁
std::string TrendEngine::open_gate_block(const TrendBot& self) const {
    if (max_total_margin_ <= 0 && max_open_positions_ <= 0) return {};

    double used_margin = 0;
    int    open_n      = 0;
    for (const auto& kv : bots_) {
        const auto& b = kv.second;
        if (b.qty <= 0) continue;
        ++open_n;
        const double px = (b.current_price > 0) ? b.current_price : b.st.entry_price;
        const int    lv = (b.cfg.leverage > 0) ? b.cfg.leverage : 1;
        if (px > 0) used_margin += b.qty * px / lv;
    }

    if (max_open_positions_ > 0 && open_n >= max_open_positions_) {
        return "同时持仓品种数已达上限 " + std::to_string(open_n) + "/" +
               std::to_string(max_open_positions_);
    }
    if (max_total_margin_ > 0) {
        // 新仓占用按 budget_usdt ÷ leverage 估。等风险模式下实际名义可能更小，
        // 所以这是【保守】估计——宁可早拦一点，不要漏拦
        const int    lv   = (self.cfg.leverage > 0) ? self.cfg.leverage : 1;
        const double want = self.cfg.budget_usdt / lv;
        if (used_margin + want > max_total_margin_) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "账户总保证金将达 $%.2f，超过上限 $%.2f（已占用 $%.2f，本笔约 $%.2f）",
                          used_margin + want, max_total_margin_, used_margin, want);
            return buf;
        }
    }
    return {};
}

void TrendEngine::tick(const std::string& symbol, double price) {
    // NaN 与任何数比较都是 false，`price <= 0` 拦不住它。这道守卫和
    // CcgEngine::tick 同源——曾经放进去之后会带着 NaN 数量去下单
    if (!std::isfinite(price) || price <= 0) return;

    // ⚠ 每个品种最多【一个】Running 的 bot（add_bot/restore_bot 强制），而 tick
    //   是按品种调用的——所以下面的循环每次最多命中一个 bot，"一个槽位"就够。
    //   这个前提很重要：trend::step 的出场分支有副作用（重置连续反手计数、
    //   写入冷却），它假定给出的动作【一定会被执行】。如果某天允许同品种多个
    //   bot 而这里还是单槽，被挤掉的那个 bot 会带着已改写的状态等下一个 tick，
    //   再次判定时走的是另一条分支——动作就错了。到那时这里必须改成向量派发
    std::string to_close, close_reason, to_open, to_ds_sync;
    trend::Pos reverse_to = trend::Pos::Flat, open_dir = trend::Pos::Flat;

    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& kv : bots_) {
            auto& b = kv.second;
            if (b.cfg.symbol != symbol)            continue;
            if (b.state != TrendBot::State::Running) continue;
            if (b.pending)                         continue;

            b.current_price = price;

            // 信号新鲜度：过期的快照不参与【开新仓】。已持仓不受影响——
            // trend::step 在 atr<=0 时会沿用上一条止损线，保护不会因数据断流而撤销
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                 host_.now_steady() - b.sig_time).count();
            const bool fresh = b.sig_ok && age <= b.cfg.effective_max_age_sec();

            trend::Inputs in;
            in.price = price;
            in.atr   = fresh ? b.atr : 0;
            in.dc_ok = fresh && b.dc_ok;
            in.dc_up = b.dc_up;
            in.dc_dn = b.dc_dn;
            in.bar_ok      = fresh && b.bar_ok;
            in.bar_open    = b.bar_open;
            in.prev_close  = b.prev_close;
            in.prev_high   = b.prev_high;
            in.prev_low    = b.prev_low;
            in.swing_low   = b.swing_low;
            in.swing_high  = b.swing_high;
            in.psar_prev_high  = b.psar_prev_high;
            in.psar_prev_low   = b.psar_prev_low;
            in.psar_prev2_high = b.psar_prev2_high;
            in.psar_prev2_low  = b.psar_prev2_low;
            in.bar_open_ms = b.bar_open_ms;
            // new_bar 由信号喂入翻新 bar_open_ms 驱动；这里每根只传一次 true
            in.new_bar = (b.bar_open_ms != 0 && b.bar_open_ms != b.last_counted_bar_ms);
            if (in.new_bar) b.last_counted_bar_ms = b.bar_open_ms;

            const auto v = trend::step(b.st, b.cfg.rule, in);

            {
                std::ostringstream d;
                d << trend::pos_name(b.st.pos);
                if (b.st.pos != trend::Pos::Flat) d << " 止损线=" << fmt(b.st.stop);
                else if (b.cfg.rule.strategy == trend::Strategy::BareK && in.bar_ok)
                    d << " 摆动[" << fmt(in.swing_low) << "," << fmt(in.swing_high) << "]";
                else if (b.cfg.rule.strategy == trend::Strategy::ParabolicSar && in.bar_ok)
                    d << " 前根[" << fmt(in.psar_prev_low) << "," << fmt(in.psar_prev_high) << "]";
                else if (in.dc_ok) d << " 通道[" << fmt(in.dc_dn) << "," << fmt(in.dc_up) << "]";
                if (!fresh) d << " 信号过期";
                d << " | " << v.reason;
                b.last_decision = d.str();
            }

            // 硬止损的【重试驱动】。固定语义下，挂上了就再也不碰——所以这里
            // 只在"该挂还没挂上"时才排队。
            //
            // 重试由 tick 驱动而不是自己起定时器：这样它用的是 EngineHost 的
            // 单调钟，回放时跟着虚拟时钟走，测试完全确定可复现。
            // 代价是喂价停了重试也停——但那时候有更大的问题，而且本地移动止损
            // 同样停了，不差这一个
            if (b.cfg.use_disaster_stop && b.st.pos != trend::Pos::Flat &&
                b.qty > 0 && b.disaster_stop_id.empty() && !b.ds_syncing &&
                to_ds_sync.empty() &&
                (b.ds_next_try.time_since_epoch().count() == 0 ||
                 host_.now_steady() >= b.ds_next_try))
                to_ds_sync = b.bot_id;

            switch (v.action) {
            case trend::Action::Hold:
                break;
            case trend::Action::OpenLong:
            case trend::Action::OpenShort:
                // ── 账户级闸门：只拦【开新仓】────────────────────────────────
                // 刻意不拦 Add / Close / 反手：
                //   · Close 与反手里的平仓腿【必须】放行——拦住出场等于把一笔
                //     该止损的仓位困在原地，这道闸门就从风控变成了风险源
                //   · 反手的开仓腿走 submit_close 内部那条路，不经过这里。
                //     那是有意的：反手是"这一笔已经结束、趋势翻了"的延续，
                //     拦掉它只会留下一个方向已经证伪的空仓状态
                //   · Add（金字塔）用的是浮盈在推，且只在已持仓时发生，
                //     不增加"同时压着几个品种"这个维度
                if (const std::string why = open_gate_block(b); !why.empty()) {
                    b.last_decision = "空仓 | " + why;
                    // 日志去重：闸门在每个 tick 都会命中，不去重就是每 3 秒
                    // 一条同样的话。拦截原因始终在 last_decision 里，界面看得到
                    if (cap_logged_.insert(b.bot_id).second)
                        log("⚠ " + b.cfg.symbol + " 开仓被账户级闸门拦下：" + why);
                    break;
                }
                cap_logged_.erase(b.bot_id);
                if (to_open.empty()) {
                    b.pending = true;
                    to_open   = b.bot_id;
                    open_dir  = (v.action == trend::Action::OpenLong) ? trend::Pos::Long
                                                                    : trend::Pos::Short;
                }
                break;
            case trend::Action::Close:
            case trend::Action::CloseReverseLong:
            case trend::Action::CloseReverseShort:
                if (to_close.empty()) {
                    b.pending    = true;
                    to_close     = b.bot_id;
                    close_reason = v.reason;
                    reverse_to = (v.action == trend::Action::CloseReverseLong)  ? trend::Pos::Long
                               : (v.action == trend::Action::CloseReverseShort) ? trend::Pos::Short
                                                                             : trend::Pos::Flat;
                }
                break;
            }
        }
    }

    // 派发在锁外：submit 可能同步执行（测试/回放的内联执行器），
    // 持锁派发会在同一线程上重入取锁——recursive_mutex 救得了死锁，
    // 救不了"别人看到的是半更新状态"
    if (!to_close.empty()) submit_close(to_close, close_reason, reverse_to);
    if (!to_open.empty())  submit_open (to_open,  open_dir, false);

    // 硬止损的挂单放在最后：要平仓/反手的那一拍不该再去挂这张单——
    // submit_close 会把它撤掉，这里再挂一张就成了孤儿单
    if (!to_ds_sync.empty() && to_close.empty())
        host_.submit([this, id = to_ds_sync]() {
            try { try_place_hard_stop(id); }
            catch (const std::exception& e) {
                log("⚠ 硬止损挂单异常: " + std::string(e.what()));
            } catch (...) { log("⚠ 硬止损挂单未知异常"); }
        });
}

void TrendEngine::clear_pending_after_throw(const std::string& id, const std::string& what) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end()) {
        it->second.pending = false;
        it->second.last_action = "异常已复位";
    }
    log("⚠ " + what);
}

void TrendEngine::submit_open(const std::string& id, trend::Pos dir, bool from_reverse) {
    host_.submit([this, id, dir, from_reverse]() {
        try {
            TrendConfig cfg;
            // 这里不再需要 atr：v4.4.0 起 plan_qty 按【真实止损线】算等风险数量，
            // 而初始止损线在下面用 trend::initial_stop 直接算好（它自己取 si.atr）
            double price = 0, init_stop = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                const auto& b = it->second;
                cfg   = b.cfg;
                price = b.current_price;
                // 初始止损线在【持锁时】按当前快照算好带出去：下单是异步的，
                // 等回来再算的话快照可能已经换了一根K线，止损线会和成交价错配
                trend::Inputs si;
                si.price      = price;
                si.atr        = b.atr;
                si.swing_low  = b.swing_low;
                si.swing_high = b.swing_high;
                si.psar_prev_high = b.psar_prev_high;
                si.psar_prev_low  = b.psar_prev_low;
                init_stop = trend::initial_stop(dir, price, cfg.rule, si);
            }

            // 杠杆失败不拦下单：敞口由 qty×价格决定，与杠杆无关，错过入场的代价
            // 更大。但必须让人看见——本地保证金估算会与实际不符
            if (!client_->set_leverage(cfg.symbol, cfg.leverage))
                log("⚠ " + cfg.symbol + " 杠杆设置失败（目标 " +
                    std::to_string(cfg.leverage) + "x），按交易所原有杠杆开仓");

            bool need_ds_sync = false;   // 成交后要不要挂灾难止损（派发在锁外）
            const double qty = plan_qty(cfg, price, init_stop);
            // 没有止损线就不开仓——这条对两种模式都成立，只是缺的东西不同
            // （唐奇安缺 ATR，裸K线缺摆动低点）
            const bool stop_ok = init_stop > 0 &&
                (dir == trend::Pos::Long ? init_stop < price : init_stop > price);
            if (qty <= 0 || !stop_ok) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.last_action = !stop_ok ? "止损线缺失，跳过开仓"
                                                      : "数量不足，跳过开仓";
                }
                log(cfg.symbol + (!stop_ok ? " 算不出有效止损线，不开仓"
                                           : " 开仓数量不足，跳过"));
                return;
            }

            auto r = client_->place_market_order(cfg.symbol, side_of(dir, false), qty, false);

            // ⚠ 这个锁必须【显式加一层作用域】：底下要在成交后同步交易所侧灾难
            //   止损，而那是 HTTP。持着 mtx_ 做网络 IO 会把整个引擎卡住几百毫秒
            //   （recursive_mutex 不死锁，但 tick / get_bots / 对账全在等它）
            {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(id);
            if (it == bots_.end()) return;
            auto& b = it->second;
            b.pending = false;

            // 零成交防幽灵仓：r.ok 但 executedQty=0（无流动性 EXPIRED）。
            // 绝不能把下单前的目标数量当成交入账
            if (r.ok && r.executed_qty <= 0) {
                b.last_action = "下单零成交，下个tick重试";
                log(cfg.symbol + " 开仓已提交但零成交（流动性不足?），不入账");
                return;
            }
            if (r.ok) {
                const double fill = (r.avg_price > 0) ? r.avg_price : price;
                // 成交价可能偏离下单时的快照价，但止损线是【结构位】（摆动低点
                // 或入场时的 Chandelier），不该随滑点漂移——用持锁时算好的那条
                // 把当前K线的开盘时间一并写进去：裸K 的 once_per_bar 护栏靠它
                // 判定"这一根已经开过了"
                trend::on_filled(b.st, dir, fill, init_stop, cfg.rule, from_reverse,
                                 b.bar_open_ms);
                b.qty = r.executed_qty;
                std::ostringstream ss;
                ss << cfg.symbol << " 开" << trend::pos_name(dir)
                   << (from_reverse ? "（反手）" : "")
                   << " qty=" << r.executed_qty << " @$" << fmt(fill)
                   << " 止损线=$" << fmt(b.st.stop)
                   << "（距离 " << fmt(std::fabs(fill - b.st.stop) / fill * 100.0, 2) << "%）";
                log(ss.str());
                b.last_action = std::string("持") + trend::pos_name(dir) + "@" + fmt(fill, 2);
                // 开仓成交即挂硬止损，不等下一个 tick——那中间的一拍是没有任何
                // 进程外保护的窗口，而新开的仓位恰恰最可能立刻遇到急跌。
                //
                // ⚠ 顺序只能是"先开仓再挂止损"：closePosition 单在没有仓位时
                //   会被交易所拒。所以这个窗口是结构性的，消不掉，只能缩到最短
                need_ds_sync = b.cfg.use_disaster_stop;
            } else if (r.uncertain) {
                // 开仓状态不明：可能已成交而本地没记录。盲目重试会变双倍仓位——
                // 停掉等人工核对。这和 reduceOnly 平仓不同，开仓【不幂等】
                b.state = TrendBot::State::Stopped;
                b.last_action = "⚠ 开仓状态不明，已停止待核对";
                log("⚠ " + cfg.symbol + " 开仓状态不明（" + r.error +
                    "），已停止该bot，请核对交易所仓位后手动恢复");
            } else {
                b.last_action = "开仓失败";
                log(cfg.symbol + " 开仓失败: " + r.error);
            }
            }   // ← mtx_ 在此释放，下面才敢做 HTTP
            if (need_ds_sync) try_place_hard_stop(id);
        } catch (const std::exception& e) {
            clear_pending_after_throw(id, "submit_open 异常: " + std::string(e.what()));
        } catch (...) {
            clear_pending_after_throw(id, "submit_open 未知异常");
        }
    });
}
// ── 交易所侧硬止损（"保命单"）─────────────────────────────────────────────────
//
// 语义：开仓成交后挂【一次】，此后不动，直到仓位关闭才撤。
//
// v5.0.1 之前它镜像移动止损（线每移动超 0.5% 就撤旧挂新）。改成固定的理由：
//   · 撤挂之间有一个"旧单已撤、新单未挂"的空窗，而这张单的全部职责就是
//     "进程死了兜住"——空窗期正是最不该有它不在的时候
//   · 每次重挂都吃限流额度，趋势走顺时会很频繁
//   · closePosition 单不带数量，金字塔加仓后它照样平掉当时的整个仓位，
//     所以"固定"这个语义天然兼容加仓，不需要跟着改
// 代价：趋势走远之后这张单仍停在开仓时的位置，此刻断电的损失比移动止损大。
// 这是取舍，不是遗漏——它的定位是【最大风险兜底】，不是第二条移动止损。
void TrendEngine::clear_ds_runtime(TrendBot& b) {
    b.ds_attempts    = 0;
    b.ds_unprotected = false;
    b.ds_next_try    = {};
    // ⚠ 不碰 ds_fail_closes：熔断要跨仓位累计才看得出是系统性故障。
    //
    // ⚠ 也【不碰 disaster_stop_id / disaster_stop_price】。它们由
    //   cancel_disaster_stop 负责清——那个函数要先读 id 才知道去交易所撤哪张单。
    //   在这里顺手清掉的话 cancel 会读到空串直接返回，交易所那张单永远不撤，
    //   而 closePosition 单同向只能有一张 ⇒ 下一轮开仓挂新单被拒 ⇒ 静默失去保护。
    //   （这个 bug 真的写出来过，被"平仓后必须撤掉硬止损单"那条断言当场抓住）
}

void TrendEngine::try_place_hard_stop(const std::string& bot_id) {
    std::string sym, side;
    double target = 0;
    int    attempt = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        auto& b = it->second;
        if (!b.cfg.use_disaster_stop)                       return;
        if (b.st.pos == trend::Pos::Flat || b.qty <= 0)       return;
        if (!b.disaster_stop_id.empty())                    return;  // 已经挂上了，固定不动
        if (b.ds_syncing)                                   return;  // 已有一次在途

        // 目标价在【开仓成交那一刻】就冻结进 disaster_stop_price，之后所有重试
        // 都用它。重试阶梯要跑二十多秒，期间止损线可能已经棘轮走了——跟着走
        // 就不叫"固定"了
        if (!(b.disaster_stop_price > 0)) {
            if (!(b.st.stop > 0)) return;   // 还没有止损线，等下一拍
            const bool is_long = (b.st.pos == trend::Pos::Long);
            const double buf = std::max(0.0, b.cfg.disaster_stop_buffer_pct) / 100.0;
            // 挂在止损线【之外】：多头往下、空头往上。缓冲不能是 0——交易所用
            // 标记价连续触发、本地按成交价采样，挂在线上会让交易所抢先，
            // 于是正常止损变成"外部平仓 → 对账 → 停 bot"，要人工介入
            b.disaster_stop_price = is_long ? b.st.stop * (1.0 - buf)
                                            : b.st.stop * (1.0 + buf);
        }
        target = b.disaster_stop_price;
        sym    = b.cfg.symbol;
        // 传【持仓方向】，由 TradingClient 内部换成平仓方向，别自己反
        side   = (b.st.pos == trend::Pos::Long) ? "BUY" : "SELL";
        attempt = b.ds_attempts + 1;
        b.ds_syncing = true;
    }

    // 多个提前返回点，标记必须每条路径都清掉——漏一条这个 bot 的硬止损就
    // 【永久不再重试】（比并发挂两张更糟：静默失去保护）。交给析构，不靠人记
    struct SyncFlagGuard {
        TrendEngine* self; const std::string& id;
        ~SyncFlagGuard() {
            std::lock_guard<std::recursive_mutex> lk(self->mtx_);
            auto it = self->bots_.find(id);
            if (it != self->bots_.end()) it->second.ds_syncing = false;
        }
    } flag_guard{this, bot_id};

    if (!(target > 0)) return;

    const auto placed = client_->place_disaster_stop(sym, target, side);

    bool give_up = false;
    std::string give_up_why;
    std::string orphan_to_cancel;   // 挂上了但已经不需要的单，撤销放在锁外
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        auto& b = it->second;

        if (placed.ok()) {
            // ⚠ 派单这几百毫秒里仓位可能已经被平掉了（平仓走的是另一条任务），
            //   那时 cancel_disaster_stop 已经把 id 与触发价一起清过一遍。
            //   此刻若还盲目写回 id，留下的是"有单号、没触发价"的状态——
            //   而本函数开头见到非空 id 就直接 return，于是【下一个仓位永远
            //   挂不上硬止损】，静默失去保护。
            //   这条是并发压力测试的"有止损单号则必有触发价"断言抓出来的
            if (b.st.pos == trend::Pos::Flat || b.qty <= 0 ||
                !(b.disaster_stop_price > 0)) {
                orphan_to_cancel = placed.order_id;
                log(sym + " 硬止损挂上时仓位已不在，撤掉这张孤儿单"
                          "（单号 " + placed.order_id + "）");
            } else {
                b.disaster_stop_id = placed.order_id;
                if (b.ds_unprotected)
                    log("✅ " + sym + " 硬止损已补挂成功（第 " + std::to_string(attempt) +
                        " 次），该仓位重新获得进程外保护");
                else
                    log(sym + " 硬止损已挂 @" + fmt(target) + "（止损线 " +
                        fmt(b.st.stop) + " 外扩 " +
                        fmt(b.cfg.disaster_stop_buffer_pct, 2) + "%，进程死了也在）");
                b.ds_attempts    = 0;
                b.ds_unprotected = false;
                b.ds_fail_closes = 0; // 挂成功 = 不是系统性故障，熔断计数清零
            }
        // ⚠ 成功分支这里【不能 return】：孤儿单必须在锁外撤，return 会跳过那一步。
        //   所以失败处理整体放进 else，而不是靠提前返回
        } else {

        b.ds_attempts = attempt;

        // 参数类错误重试没有意义（精度不对、触发价在错误的一侧）。硬等完
        // 二十多秒的阶梯只是让仓位多裸二十多秒，结果一样——直接走兜底
        if (!placed.retryable) {
            give_up = true;
            give_up_why = "挂单被拒且重试无意义（" + placed.error + "）";
        } else if (attempt >= kDsMaxTries) {
            give_up = true;
            give_up_why = "连续 " + std::to_string(kDsMaxTries) +
                          " 次挂单失败（最后一次：" + placed.error + "）";
        } else {
            b.ds_next_try = host_.now_steady() +
                            std::chrono::milliseconds(ds_backoff_ms(attempt));
            // 两条告警，语义不同：
            //   第 kDsAlertAt 次 —— "正在重试"，让人尽早知道有事，但还没到兜底
            //   第 kDsBackoffEnd 次 —— "已无保护，即将平仓"
            // 只在第一次失败就叫人的话，一次网络抖动就会刷告警
            if (attempt == kDsAlertAt) {
                log("⚠ " + sym + " 硬止损已连续 " + std::to_string(attempt) +
                    " 次挂单失败（" + placed.error + "），正在重试。"
                    "此刻该仓位【没有进程外保护】，本地移动止损仍在工作");
            } else if (attempt == kDsBackoffEnd && !b.ds_unprotected) {
                b.ds_unprotected = true;
                log("⚠⚠ " + sym + " 硬止损挂单已失败 " + std::to_string(attempt) +
                    " 次（" + placed.error + "）。该仓位【无交易所侧保护】——"
                    "再试 " + std::to_string(kDsMaxTries - kDsBackoffEnd) +
                    " 次仍失败将立即平掉该仓位");
            }
        }
        }   // ← else（挂单失败）结束
    }       // ← mtx_ 在此释放

    // 撤孤儿单必须在锁外：cancel 是 HTTP
    if (!orphan_to_cancel.empty()) client_->cancel_disaster_stop(sym, orphan_to_cancel);
    if (give_up) abandon_and_close(bot_id, give_up_why);
}

// 挂不上硬止损的兜底：平掉这一仓。
//
// 为什么是平仓而不是"带着裸仓位继续跑"：使用者把"哪怕断电断网也要有底"列成了
// 硬要求。挂不上就是这个前提不成立，而前提不成立时继续持仓等于默默降级成
// 另一套风险模型——那正是这个仓库一路在消灭的那类"静默降级"。
void TrendEngine::abandon_and_close(const std::string& bot_id, const std::string& why) {
    std::string sym;
    int fails = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        auto& b = it->second;
        if (b.st.pos == trend::Pos::Flat || b.qty <= 0) return;   // 已经没仓位了
        sym   = b.cfg.symbol;
        fails = ++b.ds_fail_closes;
        b.ds_unprotected = true;
    }

    log("⚠⚠ " + sym + " " + why + " —— 立即平掉该仓位。"
        "开仓的前提是「断电断网也有交易所侧的底」，这个前提不成立了");

    // 走正常的平仓路径：它自带幂等下单、部分成交续平、-2022（交易所侧已无仓位）
    // 的处理。reverse_to=Flat —— 这是兜底平仓，不是策略出场，不该触发反手
    submit_close(bot_id, "硬止损挂不上，兜底平仓", trend::Pos::Flat);

    // 熔断。持续性故障（账户受限、品种不支持 closePosition、参数系统性错误）
    // 会让 开仓→挂不上→平仓→等信号→开仓 无限循环，每轮付两次手续费，
    // 而单边行情里信号可能每根K线都来
    if (fails >= kDsCircuitBreak) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end()) return;
        it->second.state = TrendBot::State::Stopped;
        it->second.last_action = "⚠ 硬止损连续挂不上，已停止";
        log("⚠⚠ " + sym + " 已连续 " + std::to_string(fails) +
            " 次因挂不上硬止损而平仓，判定为系统性故障，**已停止该 bot**。"
            "继续重试只会不断支付开平手续费——请检查账户权限、品种是否支持"
            "closePosition、以及止损价精度");
    }
}

void TrendEngine::cancel_disaster_stop(const std::string& bot_id) {
    std::string sym, id;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(bot_id);
        if (it == bots_.end() || it->second.disaster_stop_id.empty()) return;
        sym = it->second.cfg.symbol;
        id  = it->second.disaster_stop_id;
        // 先就地清掉，再去撤。反了的话撤单这几百毫秒里如果又派发一次 sync，
        // 会拿着同一个已撤单号再撤一次
        it->second.disaster_stop_id.clear();
        it->second.disaster_stop_price = 0;
    }
    client_->cancel_disaster_stop(sym, id);
}

void TrendEngine::submit_close(const std::string& id, const std::string& reason,
                             trend::Pos reverse_to) {
    host_.submit([this, id, reason, reverse_to]() {
        try {
            TrendConfig cfg;
            double qty_have = 0, entry = 0, price = 0, atr = 0;
            trend::Pos pos = trend::Pos::Flat;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                cfg      = it->second.cfg;
                qty_have = it->second.qty;
                entry    = it->second.st.entry_price;
                pos      = it->second.st.pos;
                price    = it->second.current_price;
                atr      = it->second.atr;
            }
            if (pos == trend::Pos::Flat || qty_have <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) it->second.pending = false;
                return;
            }

            const double qty = client_->round_qty(cfg.symbol, qty_have);
            // 取整后归零：这张单必被拒，而下个 tick 会再发一张——无限空转、
            // 白耗限流额度、仓位永远不结清。CcgEngine 踩过，这里直接守住
            if (!std::isfinite(qty) || qty <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.state = TrendBot::State::Stopped;
                    it->second.last_action = "⚠ 残量低于最小下单量，已停止";
                }
                log("⚠ " + cfg.symbol + " 残仓 " + fmt(qty_have, 8) +
                    " 取整后为0，无法平仓，已停止该bot待人工处理");
                return;
            }

            auto r = client_->place_market_order(cfg.symbol, side_of(pos, true), qty, true);

            bool closed_ok = false;
            double exit_px = price, closed_qty = 0;
            bool external_gone =
                (!r.ok && r.error.find("[-2022]") != std::string::npos);
            if (r.ok) {
                closed_qty = r.executed_qty;
                closed_ok  = closed_qty > 0;
                if (r.avg_price > 0) exit_px = r.avg_price;
            }

            TrendTrade tr;
            bool emit_trade = false;
            // 要不要撤掉交易所侧那张单。真正的撤单在锁外做——它是 HTTP，
            // 持着 mtx_ 发请求会把 tick / get_bots / 对账全卡住
            bool need_ds_cancel = false;
            // -2022（交易所侧已无仓位）那条路本该直接 return，但撤单必须在锁外，
            // 所以改成标记 + 跳过后续，出锁之后统一处理
            bool ds_only_cancel = false;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                auto& b = it->second;

                if (external_gone) {
                    // reduceOnly 被拒 = 交易所侧已无仓位（手动平过/被强平）。
                    // 本地留着幽灵仓会每 tick 重试一次 -2022，无限循环
                    need_ds_cancel = !b.disaster_stop_id.empty();
                    trend::on_closed(b.st);
                    clear_ds_runtime(b);
                    b.qty = 0;
                    b.pending = false;
                    b.state = TrendBot::State::Stopped;
                    b.last_action = "⚠ 交易所侧已无仓位，已停止待核对";
                    log("⚠ " + cfg.symbol + " 平仓被拒(-2022)：交易所侧已无该仓位，"
                        "已清空本地状态并停止，请核对后手动恢复");
                    ds_only_cancel = true;   // 不能在这里 return：撤单要在锁外
                }
                if (!ds_only_cancel) {
                if (!closed_ok) {
                    // 含 uncertain：reduceOnly 天然幂等，若实际已平，下个 tick
                    // 重试会被交易所拒绝并走上面的 -2022 分支。重试是安全的
                    b.pending = false;
                    b.last_action = "平仓未成交，下个tick重试";
                    log(cfg.symbol + " 平仓失败: " + r.error +
                        (r.uncertain ? "（状态不明，reduceOnly 重试安全）" : ""));
                    return;
                }

                const double sign = (pos == trend::Pos::Long) ? 1.0 : -1.0;
                const double pnl  = (exit_px - entry) * closed_qty * sign;

                tr.symbol      = cfg.symbol;
                tr.side        = pos;
                tr.entry_price = entry;
                tr.exit_price  = exit_px;
                tr.qty         = closed_qty;
                tr.pnl         = pnl;
                tr.reason      = reason;
                tr.reversed    = (reverse_to != trend::Pos::Flat);
                tr.close_time  = host_.now_wall();
                emit_trade = true;

                b.realized_pnl += pnl;
                ++b.trade_count;
                if (pnl > 0) ++b.win_count;
                // 仓位已清空，撤掉交易所侧那张单。不撤的话它会留在挂单列表里，
                // 而 closePosition 单同方向只能有一张——下一轮开仓再挂会被拒
                need_ds_cancel = !b.disaster_stop_id.empty();
                trend::on_closed(b.st);
                clear_ds_runtime(b);
                b.qty = 0;

                std::ostringstream ss;
                ss << cfg.symbol << " 平" << trend::pos_name(pos) << " @$" << fmt(exit_px)
                   << " 盈亏=" << fmt(pnl, 2) << "U（" << reason << "）";
                log(ss.str());
                b.last_action = (pnl >= 0 ? "盈利出场 " : "止损出场 ") + fmt(pnl, 2) + "U";

                // ⚠ 铁律：只有【确认平掉】才允许反手。平仓失败的分支上面全都
                //   return 掉了，走到这里说明 closed_ok 为真
                if (reverse_to == trend::Pos::Flat) {
                    b.pending = false;
                }
                // reverse_to != Flat 时【保持 pending=true】，紧接着开反向，
                // 整段不放开，别的 tick 插不进来
                }   // ← if (!ds_only_cancel)
            }       // ← mtx_ 在此释放
            // 撤单放在锁外。ds_only_cancel 时后面的反手/回调一律跳过——
            // 仓位在交易所侧已经没了，没有可平的也没有可反的
            if (need_ds_cancel) cancel_disaster_stop(id);
            if (ds_only_cancel) return;
            if (emit_trade && trade_cb_) trade_cb_(tr);

            if (reverse_to != trend::Pos::Flat) {
                // ⚠ 这道闸原本是无条件的 atr<=0，那是只有海龟一个策略时留下的。
                //   ② 抛物线SAR 与 ③ 纯裸K 根本不用 ATR，无条件拦的话它们
                //   【永远反不了手】。对 PSAR 尤其致命：它的入场信号只有翻转
                //   这一个来源，等于开完第一笔就再也不交易了，而日志里只有一行
                //   "ATR缺失未反手"、界面上一切正常。
                //   非海龟策略的止损线可用性由 submit_open 里的 stop_ok 统一把关
                if (cfg.rule.strategy == trend::Strategy::Turtle && atr <= 0) {
                    std::lock_guard<std::recursive_mutex> lk(mtx_);
                    auto it = bots_.find(id);
                    if (it != bots_.end()) {
                        it->second.pending = false;
                        it->second.last_action = "已平仓，ATR缺失未反手";
                    }
                    log(cfg.symbol + " 已平仓，但 ATR 缺失无法定止损线，本次不反手");
                    return;
                }
                log(cfg.symbol + " 反手开" + trend::pos_name(reverse_to));
                submit_open(id, reverse_to, true);
            }
        } catch (const std::exception& e) {
            clear_pending_after_throw(id, "submit_close 异常: " + std::string(e.what()));
        } catch (...) {
            clear_pending_after_throw(id, "submit_close 未知异常");
        }
    });
}

} // namespace ccbot
