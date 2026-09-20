#include "core/sar_engine.h"

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
const char* side_of(sar::Pos p, bool closing) {
    // 开仓：多=BUY 空=SELL；平仓方向相反
    const bool buy = closing ? (p == sar::Pos::Short) : (p == sar::Pos::Long);
    return buy ? "BUY" : "SELL";
}
} // namespace

SarEngine::SarEngine(std::shared_ptr<ITradingClient> client,
                     std::shared_ptr<ThreadPool> pool)
    : client_(std::move(client)), pool_(std::move(pool)) {
    auto p = pool_;
    host_.submit = [p](std::function<void()> fn) {
        if (p) p->submit(std::move(fn)); else fn();
    };
}

SarEngine::SarEngine(std::shared_ptr<ITradingClient> client, EngineHost host)
    : client_(std::move(client)), host_(std::move(host)) {
    if (!host_.submit) host_.submit = [](std::function<void()> fn) { fn(); };
}

void SarEngine::log(const std::string& msg) const {
    if (log_cb_) log_cb_(msg);
}

std::string SarEngine::add_bot(const SarConfig& cfg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (const auto& kv : bots_)
        if (kv.second.cfg.symbol == cfg.symbol &&
            kv.second.state != SarBot::State::Stopped)
            return {};

    SarBot b;
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

std::string SarEngine::restore_bot(SarBot snap) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (const auto& kv : bots_)
        if (kv.second.cfg.symbol == snap.cfg.symbol &&
            kv.second.state != SarBot::State::Stopped)
            return {};

    snap.bot_id  = "sar" + std::to_string(++seq_);
    snap.pending = false;              // 落盘时的在途标记一律作废
    // 信号一律作废重拉：落盘的 ATR 可能是几小时前的，而止损线的距离由它决定。
    // sig_ok=false 时 sar::step 会沿用已有的止损线、不开新仓——正是想要的
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
    if (b.st.pos != sar::Pos::Flat)
        log(b.cfg.symbol + " SAR 从落盘恢复：持" + sar::pos_name(b.st.pos) +
            " qty=" + fmt(b.qty, 8) + " 开仓价=$" + fmt(b.st.entry_price) +
            " 止损线=$" + fmt(b.st.stop));
    else
        log(b.cfg.symbol + " SAR 从落盘恢复：空仓");
    return id;
}

std::vector<std::string> SarEngine::reconcile_positions(
        const std::vector<ExchangePos>& exchange) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<std::string> issues;

    for (auto& kv : bots_) {
        auto& b = kv.second;
        // 在途的跳过：订单可能已在交易所生效而本地还没入账，此刻比对必然误判
        if (b.pending) continue;

        const ExchangePos* ex = nullptr;
        for (const auto& e : exchange)
            if (e.symbol == b.cfg.symbol && e.qty > 0) { ex = &e; break; }

        const bool local_has = (b.st.pos != sar::Pos::Flat && b.qty > 0);

        if (!local_has && !ex) continue;               // 两边都空，一致

        if (!local_has && ex) {
            // 孤儿仓：最危险的一种。不停的话，下一个突破信号会再开一笔，
            // 而交易所上那笔无人管理——净敞口翻倍且没有任何止损线守着
            issues.push_back(b.cfg.symbol + " 交易所有仓位(" +
                             std::string(ex->direction > 0 ? "多" : "空") + " " +
                             fmt(ex->qty, 8) + ")但本地无跟踪，已停止该bot");
            b.state = SarBot::State::Stopped;
            b.last_action = "⚠ 孤儿仓，已停止待核对";
            continue;
        }

        if (local_has && !ex) {
            issues.push_back(b.cfg.symbol + " 本地有仓位但交易所没有（外部已平/被强平），"
                             "已清空本地状态并停止该bot");
            sar::on_closed(b.st);
            b.qty = 0;
            b.state = SarBot::State::Stopped;
            b.last_action = "⚠ 交易所侧已无仓位，已停止";
            continue;
        }

        // 两边都有：先看方向，再看数量
        const int local_dir = (b.st.pos == sar::Pos::Long) ? 1 : -1;
        if (local_dir != ex->direction) {
            // 方向都对不上，任何自动收敛都是在猜。停下来让人看
            issues.push_back(b.cfg.symbol + " 本地方向(" + sar::pos_name(b.st.pos) +
                             ")与交易所(" + (ex->direction > 0 ? "多" : "空") +
                             ")不一致，已停止该bot");
            b.state = SarBot::State::Stopped;
            b.last_action = "⚠ 方向不一致，已停止";
            continue;
        }

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

void SarEngine::stop_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    it->second.state = SarBot::State::Stopped;
    it->second.last_action = "已暂停";
}

void SarEngine::resume_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it == bots_.end()) return;
    it->second.state = SarBot::State::Running;
    it->second.last_action = "已恢复";
}

void SarEngine::close_bot(const std::string& id) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = bots_.find(id);
        if (it == bots_.end() || it->second.pending) return;
        if (it->second.st.pos == sar::Pos::Flat || it->second.qty <= 0) return;
        it->second.pending = true;
    }
    submit_close(id, "手动平仓", sar::Pos::Flat);
}

void SarEngine::remove_bot(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    bots_.erase(id);
}

void SarEngine::stop_all() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    for (auto& kv : bots_) {
        kv.second.state = SarBot::State::Stopped;
        kv.second.last_action = "已暂停";
    }
}

std::vector<SarBot> SarEngine::get_bots() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<SarBot> out;
    out.reserve(bots_.size());
    for (const auto& kv : bots_) out.push_back(kv.second);
    return out;
}

void SarEngine::set_pending_for_test(const std::string& id, bool v) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end()) it->second.pending = v;
}

void SarEngine::update_signal(const std::string& id, double atr, double atr_pct,
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
    b.sig_ok   = (atr > 0 && dc_ok);
    // bar_open_ms 变了 = 跨入新K线。冷却按K线计数，不按 tick——
    // 3 秒一个 tick 的话，"冷却3根4h线"会在 9 秒内走完，等于没有冷却
    if (bar_open_ms > 0 && bar_open_ms != b.bar_open_ms) {
        b.bar_open_ms = bar_open_ms;   // 实际的冷却递减在 tick 里，见 last_counted_bar_ms
    }
}

double SarEngine::plan_qty(const SarConfig& cfg, double price, double atr) const {
    if (!std::isfinite(price) || price <= 0) return 0;

    double notional = cfg.budget_usdt;
    if (cfg.size_mode == SarConfig::SizeMode::RiskBased) {
        // 止损距离（绝对价格）= k × ATR。止损时亏的钱 = 数量 × 止损距离，
        // 令它等于 risk_usdt 反推数量，再乘价格得名义
        const double stop_dist = cfg.rule.atr_mult * atr;
        if (!(atr > 0) || !(stop_dist > 0) || !(cfg.risk_usdt > 0)) return 0;
        notional = cfg.risk_usdt * price / stop_dist;
        // 名义上限：ATR 极小时上面那个除法会算出荒谬的大仓位。
        // 没有这道帽子，一个刚上市、K线还平着的品种能把整个账户吃掉
        if (cfg.budget_usdt > 0) notional = std::min(notional, cfg.budget_usdt);
    }
    if (!std::isfinite(notional) || notional <= 0) return 0;

    double q = client_->round_qty(cfg.symbol, notional / price);
    return (std::isfinite(q) && q > 0) ? q : 0;
}

void SarEngine::tick(const std::string& symbol, double price) {
    // NaN 与任何数比较都是 false，`price <= 0` 拦不住它。这道守卫和
    // CcgEngine::tick 同源——曾经放进去之后会带着 NaN 数量去下单
    if (!std::isfinite(price) || price <= 0) return;

    std::string to_close, close_reason, to_open, to_add;
    sar::Pos reverse_to = sar::Pos::Flat, open_dir = sar::Pos::Flat;

    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& kv : bots_) {
            auto& b = kv.second;
            if (b.cfg.symbol != symbol)            continue;
            if (b.state != SarBot::State::Running) continue;
            if (b.pending)                         continue;

            b.current_price = price;

            // 信号新鲜度：过期的快照不参与【开新仓】。已持仓不受影响——
            // sar::step 在 atr<=0 时会沿用上一条止损线，保护不会因数据断流而撤销
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                 host_.now_steady() - b.sig_time).count();
            const bool fresh = b.sig_ok && age <= b.cfg.signal_max_age_sec;

            sar::Inputs in;
            in.price = price;
            in.atr   = fresh ? b.atr : 0;
            in.dc_ok = fresh && b.dc_ok;
            in.dc_up = b.dc_up;
            in.dc_dn = b.dc_dn;
            // new_bar 由 update_signal 翻新 bar_open_ms 驱动；这里每 tick 只传一次 true
            in.new_bar = (b.bar_open_ms != 0 && b.bar_open_ms != b.last_counted_bar_ms);
            if (in.new_bar) b.last_counted_bar_ms = b.bar_open_ms;

            const auto v = sar::step(b.st, b.cfg.rule, in);

            {
                std::ostringstream d;
                d << sar::pos_name(b.st.pos);
                if (b.st.pos != sar::Pos::Flat) d << " 止损线=" << fmt(b.st.stop);
                else if (in.dc_ok) d << " 通道[" << fmt(in.dc_dn) << "," << fmt(in.dc_up) << "]";
                if (!fresh) d << " 信号过期";
                d << " | " << v.reason;
                b.last_decision = d.str();
            }

            switch (v.action) {
            case sar::Action::Hold:
                break;
            case sar::Action::OpenLong:
            case sar::Action::OpenShort:
                if (to_open.empty()) {
                    b.pending = true;
                    to_open   = b.bot_id;
                    open_dir  = (v.action == sar::Action::OpenLong) ? sar::Pos::Long
                                                                    : sar::Pos::Short;
                }
                break;
            case sar::Action::Add:
                if (to_add.empty() && to_open.empty() && to_close.empty()) {
                    b.pending = true;
                    to_add    = b.bot_id;
                }
                break;
            case sar::Action::Close:
            case sar::Action::CloseReverseLong:
            case sar::Action::CloseReverseShort:
                if (to_close.empty()) {
                    b.pending    = true;
                    to_close     = b.bot_id;
                    close_reason = v.reason;
                    reverse_to = (v.action == sar::Action::CloseReverseLong)  ? sar::Pos::Long
                               : (v.action == sar::Action::CloseReverseShort) ? sar::Pos::Short
                                                                             : sar::Pos::Flat;
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
    if (!to_add.empty())   submit_add  (to_add);
}

void SarEngine::clear_pending_after_throw(const std::string& id, const std::string& what) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = bots_.find(id);
    if (it != bots_.end()) {
        it->second.pending = false;
        it->second.last_action = "异常已复位";
    }
    log("⚠ " + what);
}

void SarEngine::submit_open(const std::string& id, sar::Pos dir, bool from_reverse) {
    host_.submit([this, id, dir, from_reverse]() {
        try {
            SarConfig cfg;
            double price = 0, atr = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                cfg   = it->second.cfg;
                price = it->second.current_price;
                atr   = it->second.atr;
            }

            // 杠杆失败不拦下单：敞口由 qty×价格决定，与杠杆无关，错过入场的代价
            // 更大。但必须让人看见——本地保证金估算会与实际不符
            if (!client_->set_leverage(cfg.symbol, cfg.leverage))
                log("⚠ " + cfg.symbol + " 杠杆设置失败（目标 " +
                    std::to_string(cfg.leverage) + "x），按交易所原有杠杆开仓");

            const double qty = plan_qty(cfg, price, atr);
            if (qty <= 0 || atr <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.last_action = (atr <= 0) ? "ATR缺失，跳过开仓"
                                                        : "数量不足，跳过开仓";
                }
                log(cfg.symbol + (atr <= 0 ? " ATR 缺失，不开仓（没有ATR就没有止损线）"
                                           : " 开仓数量不足，跳过"));
                return;
            }

            auto r = client_->place_market_order(cfg.symbol, side_of(dir, false), qty, false);

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
                sar::on_filled(b.st, dir, fill, atr, cfg.rule, from_reverse);
                b.qty = r.executed_qty;
                std::ostringstream ss;
                ss << cfg.symbol << " 开" << sar::pos_name(dir)
                   << (from_reverse ? "（反手）" : "")
                   << " qty=" << r.executed_qty << " @$" << fmt(fill)
                   << " 止损线=$" << fmt(b.st.stop)
                   << "（距离 " << fmt(std::fabs(fill - b.st.stop) / fill * 100.0, 2) << "%）";
                log(ss.str());
                b.last_action = std::string("持") + sar::pos_name(dir) + "@" + fmt(fill, 2);
            } else if (r.uncertain) {
                // 开仓状态不明：可能已成交而本地没记录。盲目重试会变双倍仓位——
                // 停掉等人工核对。这和 reduceOnly 平仓不同，开仓【不幂等】
                b.state = SarBot::State::Stopped;
                b.last_action = "⚠ 开仓状态不明，已停止待核对";
                log("⚠ " + cfg.symbol + " 开仓状态不明（" + r.error +
                    "），已停止该bot，请核对交易所仓位后手动恢复");
            } else {
                b.last_action = "开仓失败";
                log(cfg.symbol + " 开仓失败: " + r.error);
            }
        } catch (const std::exception& e) {
            clear_pending_after_throw(id, "submit_open 异常: " + std::string(e.what()));
        } catch (...) {
            clear_pending_after_throw(id, "submit_open 未知异常");
        }
    });
}

void SarEngine::submit_add(const std::string& id) {
    host_.submit([this, id]() {
        try {
            SarConfig cfg;
            double price = 0, atr = 0, have_qty = 0, avg = 0;
            sar::Pos pos = sar::Pos::Flat;
            int adds_before = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                cfg      = it->second.cfg;
                price    = it->second.current_price;
                atr      = it->second.atr;
                have_qty = it->second.qty;
                avg      = it->second.st.entry_price;
                pos      = it->second.st.pos;
                adds_before = it->second.st.adds_done;
            }
            if (pos == sar::Pos::Flat || have_qty <= 0 || atr <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) it->second.pending = false;
                return;
            }

            const double qty = plan_qty(cfg, price, atr);
            if (qty <= 0) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it != bots_.end()) {
                    it->second.pending = false;
                    it->second.last_action = "加仓数量不足，跳过";
                }
                return;
            }

            // 加仓单与开仓单一样是非 reduceOnly 的同向市价单
            auto r = client_->place_market_order(cfg.symbol, side_of(pos, false), qty, false);

            std::lock_guard<std::recursive_mutex> lk(mtx_);
            auto it = bots_.find(id);
            if (it == bots_.end()) return;
            auto& b = it->second;
            b.pending = false;

            if (r.ok && r.executed_qty <= 0) {
                b.last_action = "加仓零成交，下个tick重试";
                log(cfg.symbol + " 加仓已提交但零成交（流动性不足?），不入账");
                return;
            }
            if (r.ok) {
                const double fill = (r.avg_price > 0) ? r.avg_price : price;
                const double new_qty = have_qty + r.executed_qty;
                // 加权均价。必须更新，否则"出场价≥成本"会拿第一档的价格去判，
                // 把一笔实际亏损的出场误判成盈利出场（进而该反手时不反手）
                const double new_avg = (avg * have_qty + fill * r.executed_qty) / new_qty;
                b.qty = new_qty;
                sar::on_added(b.st, fill, new_avg);

                std::ostringstream ss;
                ss << cfg.symbol << " 顺势加仓 第" << b.st.adds_done << "/"
                   << cfg.rule.pyramid_max_adds << " 档 qty=" << r.executed_qty
                   << " @$" << fmt(fill) << " 新均价=$" << fmt(new_avg)
                   << " 总量=" << fmt(new_qty, 8)
                   << " 止损线=$" << fmt(b.st.stop);
                log(ss.str());
                b.last_action = "加仓至" + std::to_string(b.st.adds_done + 1) + "档";
            } else if (r.uncertain) {
                // 加仓和开仓一样【不幂等】：盲目重试会多出一档。停掉待核对
                b.state = SarBot::State::Stopped;
                b.last_action = "⚠ 加仓状态不明，已停止待核对";
                log("⚠ " + cfg.symbol + " 加仓状态不明（" + r.error +
                    "），已停止该bot，请核对交易所仓位后手动恢复");
            } else {
                // 加仓失败不影响已有仓位，止损线照常守着。下个 tick 若仍满足
                // 间距条件会再试一次——adds_done 没有增加，不会错过这一档
                b.last_action = "加仓失败";
                log(cfg.symbol + " 加仓失败: " + r.error + "（已有仓位不受影响）");
                (void)adds_before;
            }
        } catch (const std::exception& e) {
            clear_pending_after_throw(id, "submit_add 异常: " + std::string(e.what()));
        } catch (...) {
            clear_pending_after_throw(id, "submit_add 未知异常");
        }
    });
}

void SarEngine::submit_close(const std::string& id, const std::string& reason,
                             sar::Pos reverse_to) {
    host_.submit([this, id, reason, reverse_to]() {
        try {
            SarConfig cfg;
            double qty_have = 0, entry = 0, price = 0, atr = 0;
            sar::Pos pos = sar::Pos::Flat;
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
            if (pos == sar::Pos::Flat || qty_have <= 0) {
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
                    it->second.state = SarBot::State::Stopped;
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

            SarTrade tr;
            bool emit_trade = false;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                auto it = bots_.find(id);
                if (it == bots_.end()) return;
                auto& b = it->second;

                if (external_gone) {
                    // reduceOnly 被拒 = 交易所侧已无仓位（手动平过/被强平）。
                    // 本地留着幽灵仓会每 tick 重试一次 -2022，无限循环
                    sar::on_closed(b.st);
                    b.qty = 0;
                    b.pending = false;
                    b.state = SarBot::State::Stopped;
                    b.last_action = "⚠ 交易所侧已无仓位，已停止待核对";
                    log("⚠ " + cfg.symbol + " 平仓被拒(-2022)：交易所侧已无该仓位，"
                        "已清空本地状态并停止，请核对后手动恢复");
                    return;
                }
                if (!closed_ok) {
                    // 含 uncertain：reduceOnly 天然幂等，若实际已平，下个 tick
                    // 重试会被交易所拒绝并走上面的 -2022 分支。重试是安全的
                    b.pending = false;
                    b.last_action = "平仓未成交，下个tick重试";
                    log(cfg.symbol + " 平仓失败: " + r.error +
                        (r.uncertain ? "（状态不明，reduceOnly 重试安全）" : ""));
                    return;
                }

                const double sign = (pos == sar::Pos::Long) ? 1.0 : -1.0;
                const double pnl  = (exit_px - entry) * closed_qty * sign;

                tr.symbol      = cfg.symbol;
                tr.side        = pos;
                tr.entry_price = entry;
                tr.exit_price  = exit_px;
                tr.qty         = closed_qty;
                tr.pnl         = pnl;
                tr.reason      = reason;
                tr.reversed    = (reverse_to != sar::Pos::Flat);
                tr.close_time  = host_.now_wall();
                emit_trade = true;

                b.realized_pnl += pnl;
                ++b.trade_count;
                if (pnl > 0) ++b.win_count;
                sar::on_closed(b.st);
                b.qty = 0;

                std::ostringstream ss;
                ss << cfg.symbol << " 平" << sar::pos_name(pos) << " @$" << fmt(exit_px)
                   << " 盈亏=" << fmt(pnl, 2) << "U（" << reason << "）";
                log(ss.str());
                b.last_action = (pnl >= 0 ? "盈利出场 " : "止损出场 ") + fmt(pnl, 2) + "U";

                // ⚠ 铁律：只有【确认平掉】才允许反手。平仓失败的分支上面全都
                //   return 掉了，走到这里说明 closed_ok 为真
                if (reverse_to == sar::Pos::Flat) {
                    b.pending = false;
                }
                // reverse_to != Flat 时【保持 pending=true】，紧接着开反向，
                // 整段不放开，别的 tick 插不进来
            }
            if (emit_trade && trade_cb_) trade_cb_(tr);

            if (reverse_to != sar::Pos::Flat) {
                if (atr <= 0) {
                    std::lock_guard<std::recursive_mutex> lk(mtx_);
                    auto it = bots_.find(id);
                    if (it != bots_.end()) {
                        it->second.pending = false;
                        it->second.last_action = "已平仓，ATR缺失未反手";
                    }
                    log(cfg.symbol + " 已平仓，但 ATR 缺失无法定止损线，本次不反手");
                    return;
                }
                log(cfg.symbol + " 反手开" + sar::pos_name(reverse_to));
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
