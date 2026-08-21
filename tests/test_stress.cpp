// 引擎压力测试 —— 实盘前的最后一道自检。
//
// 存在的理由：现有六套测试都在验证"给定输入产生正确输出"，但实盘杀死交易系统的
// 往往不是算错，而是【并发下的状态撕裂】和【没人想过的极端输入】。这两类问题在
// 单线程、行情正常的测试里永远不会出现。
//
// 分两部分：
//   A. 并发压力 —— 真线程池 + 多线程同时 tick/喂数据/读快照/对账，交易所返回
//      随机化（全成交/部分成交/零成交/失败/状态不明）。GUI 里 tick() 本来就会被
//      UI 线程和线程池线程同时调用（WS 价格走前者、REST 兜底走后者），所以这
//      不是臆想的场景。跑完检查【不变量】——不管线程怎么交错，这些都必须成立。
//   B. 极端行情 —— 闪崩、天地针、非法价格、极端量级、满层、连环部分成交。
//      用虚拟时钟 + 内联执行器，完全确定可复现。
//
// 不变量比"期望值"更适合压力测试：并发下没有唯一正确的输出，但"持仓量不能超过
// 各层之和"这类约束在任何交错下都必须成立，一旦破了就是真的状态撕裂。
#include "core/ccg_engine.h"
#include "core/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// ── 不变量：任何线程交错、任何行情下都必须成立 ───────────────────────────────
// 违反 = 状态撕裂，实盘里意味着本地记的仓位和交易所对不上
static std::string violated_invariant(const CcgBot& b) {
    auto bad = [](double v) { return !std::isfinite(v); };
    if (bad(b.total_qty) || bad(b.total_cost) || bad(b.avg_price) ||
        bad(b.realized_pnl) || bad(b.last_entry_price) || bad(b.dca_extreme) ||
        bad(b.tp_extreme) || bad(b.current_price))
        return "存在 NaN/Inf 状态量";

    if (b.total_qty < -1e-12)  return "持仓量为负";
    if (b.total_cost < -1e-9)  return "持仓成本为负";
    if (b.avg_price  < -1e-12) return "均价为负";

    if ((int)b.entries.size() > b.cfg.max_entries)
        return "层数 " + std::to_string(b.entries.size()) + " 超过上限 " +
               std::to_string(b.cfg.max_entries);

    double sum_qty = 0;
    for (const auto& e : b.entries) {
        if (bad(e.qty) || bad(e.price) || bad(e.cost_usdt)) return "加仓记录含 NaN/Inf";
        if (e.qty < 0) return "加仓记录数量为负";
        sum_qty += e.qty;
    }
    // 部分平仓只会让 total_qty 下降，绝不会超过各层之和
    if (b.total_qty > sum_qty + std::max(1e-9, sum_qty * 1e-6))
        return "持仓量 " + std::to_string(b.total_qty) + " 超过各层之和 " +
               std::to_string(sum_qty);

    if (b.entries.empty() && b.total_qty > 1e-12)
        return "无加仓记录却有持仓量";

    // 均价必须与 成本/数量 自洽
    if (b.total_qty > 1e-12) {
        double implied = b.total_cost / b.total_qty;
        if (std::fabs(implied - b.avg_price) > std::max(1e-6, b.avg_price * 1e-6))
            return "均价与成本/数量不自洽";
    }

    // ── 状态机不变量 ────────────────────────────────────────────────────────
    // 上面全是【数值】不变量：它们能抓住"数字变成垃圾"，抓不住"标志位组合非法"。
    // 而后者同样致命，且更隐蔽——数字全对，引擎却基于一个不可能的状态做决策。
    //
    // 例：entries 空了（刚平完仓）而 tp_reached 还留着 true，下一轮建仓后
    // should_close 会立刻用上一轮的 tp_extreme 判定，可能开仓即平仓。
    if (b.entries.empty()) {
        if (b.tp_reached)
            return "无持仓却仍处于止盈追踪态（tp_reached=true）";
        if (b.interval_hit)
            return "无持仓却仍处于补仓武装态（interval_hit=true）";
        if (b.avg_price > 1e-12)
            return "无持仓却残留均价 " + std::to_string(b.avg_price);
        if (b.total_cost > 1e-9)
            return "无持仓却残留成本 " + std::to_string(b.total_cost);
    } else {
        // 有加仓记录就必须有均价——止盈线、保底线、灾难止损价全建立在它之上
        if (b.total_qty > 1e-12 && b.avg_price <= 0)
            return "有持仓却没有均价";
    }

    // 冷却态是"平仓之后等下一轮"，此时不该还握着仓位。
    // 若两者并存，说明平仓路径把状态改了却没清仓位，那笔仓位会失去止盈止损照管
    if (b.state == CcgBot::State::Cooldown && b.total_qty > 1e-12)
        return "冷却态却仍持有仓位 " + std::to_string(b.total_qty);

    // 止盈追踪激活时必须有极值锚点，否则 should_close 的回落判定没有基准
    if (b.tp_reached && b.tp_extreme <= 0)
        return "止盈追踪已激活但极值锚点为 0";

    return "";
}

static std::string check_all(const std::vector<CcgBot>& bots) {
    for (const auto& b : bots) {
        auto v = violated_invariant(b);
        if (!v.empty()) return b.bot_id + ": " + v;
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// A. 并发压力
// ─────────────────────────────────────────────────────────────────────────────

// 随机化的假交易所：把实盘会遇到的每种结局都按概率抛出来
class StressClient : public ITradingClient {
public:
    std::atomic<long> orders{0};
    std::atomic<long> nan_qty_orders{0};    // 收到 NaN/Inf 数量的下单（绝不该发生）
    std::atomic<long> bad_open{0};          // <=0 数量的【开仓】单
    std::atomic<long> bad_close{0};         // <=0 数量的【平仓】单

    OrderOutcome place_market_order(const std::string&, const std::string&,
                                    double qty, bool reduce_only) override {
        ++orders;
        if (!std::isfinite(qty))      ++nan_qty_orders;
        else if (qty <= 0)            { if (reduce_only) ++bad_close; else ++bad_open; }

        OrderOutcome o;
        int roll;
        double px;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            roll = dist100_(rng_);
            px   = last_price_;
        }
        o.order_id = "M" + std::to_string(orders.load());
        // 概率按【实盘相对频率】设，而不是均匀分布：状态不明会让 bot 停机，
        // 给它 3% 的话几十笔之后全场停摆，反而测不到成交/平仓那些主路径。
        // 保留它但压到 1%，让稀有分支被覆盖到而不主导整个测试
        if (roll < 70) {                       // 全额成交
            o.ok = true; o.avg_price = px; o.executed_qty = qty;
        } else if (roll < 84) {                // 部分成交
            o.ok = true; o.avg_price = px; o.executed_qty = qty * 0.4;
        } else if (roll < 90) {                // 零成交（无流动性 EXPIRED）
            o.ok = true; o.avg_price = 0; o.executed_qty = 0;
        } else if (roll < 96) {                // 普通失败
            o.ok = false; o.error = "[-2019] Margin is insufficient";
        } else if (roll < 99) {                // 交易所侧已无仓位
            o.ok = false; o.error = "[-2022] ReduceOnly Order is rejected";
        } else {                               // 状态不明（网络中断且查单失败）
            o.ok = false; o.uncertain = true; o.error = "超时且查单失败";
        }
        (void)reduce_only;
        return o;
    }
    double round_qty(const std::string&, double qty) override {
        if (!std::isfinite(qty)) return qty;   // 原样透传：要让引擎自己挡住，而不是靠这里兜
        double r = std::floor(qty * 1000.0) / 1000.0;
        return r < 0.001 ? 0.0 : r;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }
    std::string place_disaster_stop(const std::string&, double, const std::string&) override {
        std::lock_guard<std::mutex> lk(mtx_);
        return (dist100_(rng_) < 85) ? ("S" + std::to_string(++stop_seq_)) : std::string();
    }
    bool cancel_disaster_stop(const std::string&, const std::string&) override { return true; }

    void set_price(double p) { std::lock_guard<std::mutex> lk(mtx_); last_price_ = p; }
    double price() { std::lock_guard<std::mutex> lk(mtx_); return last_price_; }
    void seed(unsigned s)    { std::lock_guard<std::mutex> lk(mtx_); rng_.seed(s); }

private:
    std::mutex mtx_;
    std::mt19937 rng_{12345};
    std::uniform_int_distribution<int> dist100_{0, 99};
    double last_price_ = 100.0;
    int    stop_seq_   = 0;
};

static bool concurrency_stress(unsigned seed, int rounds) {
    auto client = std::make_shared<StressClient>();
    client->seed(seed);
    auto pool = std::make_shared<ThreadPool>(8);
    CcgEngine eng(client, pool);
    eng.set_max_total_margin(5000.0);

    const std::vector<std::string> syms = {
        "AAAUSDT", "BBBUSDT", "CCCUSDT", "DDDUSDT", "EEEUSDT", "FFFUSDT"
    };
    std::vector<std::string> ids;
    for (size_t i = 0; i < syms.size(); ++i) {
        CcgConfig c;
        c.symbol       = syms[i];
        c.direction    = (i % 2) ? CcgConfig::Direction::Short : CcgConfig::Direction::Long;
        c.budget_usdt  = 1000;
        c.max_entries  = 6;
        c.leverage     = 5;
        c.entry_mode   = (i % 3) ? CcgConfig::EntryMode::Indicator
                                 : CcgConfig::EntryMode::Immediate;
        c.use_htf_filter = c.use_sr_support = c.use_sr_headroom = false;
        c.use_trend_filter   = (i % 2 == 0);
        c.use_disaster_stop  = (i % 2 == 1);
        c.cooldown_secs      = 0;
        auto id = eng.add_bot(c);
        if (!id.empty()) ids.push_back(id);
    }
    if (ids.size() != syms.size()) { std::printf("  建 bot 失败\n"); return false; }

    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0};          // 读快照时发现的不变量破坏次数
    std::string       first_violation;
    std::mutex        viol_mtx;

    auto record = [&](const std::string& v) {
        if (v.empty()) return;
        ++torn;
        std::lock_guard<std::mutex> lk(viol_mtx);
        if (first_violation.empty()) first_violation = v;
    };

    // ① 行情线程：两条，各负责一半品种（复刻 GUI 里 UI线程 + REST兜底线程同时 tick）
    auto ticker_fn = [&](int lane, unsigned s) {
        std::mt19937 rng(s);
        std::uniform_real_distribution<double> jitter(-0.06, 0.06);
        double px = 100.0;
        for (int r = 0; r < rounds && !stop.load(); ++r) {
            px *= (1.0 + jitter(rng));
            if (px < 1.0)    px = 1.0;
            if (px > 100000) px = 100000;
            client->set_price(px);
            for (size_t i = lane; i < syms.size(); i += 2) eng.tick(syms[i], px);
            if ((r & 63) == 0) record(check_all(eng.get_bots()));
            // 定期全场复活：停机分支（状态不明/对账/-2022）是【终态】，不复活的话
            // 测试跑不了多久就全员停摆，后面的轮次都是空转。复活相当于"用户看到
            // 告警后点了继续"，正是实盘里会发生的事
            if (lane == 0 && (r % 256) == 0)
                for (const auto& id : ids) eng.resume_bot(id);
        }
    };

    // ② 数据喂入线程：指标/趋势/结构，模拟应用层的异步拉取回调
    auto feeder_fn = [&](unsigned s) {
        std::mt19937 rng(s);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int r = 0; r < rounds && !stop.load(); ++r) {
            // 带子必须【跟着真实价格走】。早先版本喂的是与价格无关的随机带，结果
            // 价格几乎摸不到上轨，所有 bot 补满 6 层后就永远挂着等止盈——两万轮只
            // 下了几十单，止盈/冷却/重开这半条生命周期完全没被覆盖。
            // 覆盖度是压力测试的前提：跑得再久，跑不到的分支也测不出问题
            for (const auto& id : ids) {
                double base = client->price();
                if (!(base > 0)) base = 100.0;
                double mid = base * (0.97 + 0.06 * u(rng));
                double w   = mid * (0.01 + 0.03 * u(rng));
                eng.update_indicator(id, mid - w, mid + w, 100.0 * u(rng));
                eng.update_trend(id, u(rng) < 0.5);
                eng.update_htf(id, u(rng));
                eng.update_sr_structure(id, u(rng) < 0.5, mid * 0.9, mid * 1.1, mid * 0.8);
                for (int t = 0; t < 4; ++t)
                    eng.update_mtf_band(id, t, mid - w * (t + 1), mid + w * (t + 1));
            }
        }
    };

    // ③ 读者线程：持续读快照（GUI 每 100ms 刷表就是这个），顺便查不变量
    // 注意 yield：读者若是纯忙等，会把 recursive_mutex 一直攥在手里，
    // 行情线程几乎进不去（实测下单量掉到 1/20）——那测的就不是引擎而是锁竞争了。
    // GUI 实际是 100ms 刷一次表，这里的节流是【还原真实调用频率】，不是掩盖问题
    auto reader_fn = [&]() {
        while (!stop.load()) {
            record(check_all(eng.get_bots()));
            (void)eng.total_margin_used();
            std::this_thread::yield();
        }
    };

    // ④ 干扰线程：对账 / 停 / 恢复 —— 全都可能和在途订单撞上
    auto chaos_fn = [&](unsigned s) {
        std::mt19937 rng(s);
        std::uniform_int_distribution<int> pick(0, (int)ids.size() - 1);
        std::uniform_int_distribution<int> act(0, 3);
        for (int r = 0; r < rounds / 4 && !stop.load(); ++r) {
            const auto& id = ids[pick(rng)];
            switch (act(rng)) {
            case 0: eng.stop_bot(id);   break;
            case 1: eng.resume_bot(id); break;
            case 2: {
                std::vector<CcgEngine::ExchangePos> ex;
                for (const auto& b : eng.get_bots()) {
                    if (b.total_qty <= 0) continue;
                    CcgEngine::ExchangePos p;
                    p.symbol      = b.cfg.symbol;
                    p.direction   = (b.cfg.direction == CcgConfig::Direction::Long) ? 1 : -1;
                    p.qty         = b.total_qty * 0.8;   // 制造"外部部分平仓"
                    p.entry_price = b.avg_price;
                    ex.push_back(p);
                }
                eng.reconcile_positions(ex);
                break;
            }
            default: eng.resync_disaster_stops(); break;
            }
        }
    };

    std::vector<std::thread> ts;
    ts.emplace_back(ticker_fn, 0, seed + 1);
    ts.emplace_back(ticker_fn, 1, seed + 2);
    ts.emplace_back(feeder_fn, seed + 3);
    ts.emplace_back(feeder_fn, seed + 4);
    ts.emplace_back(chaos_fn,  seed + 5);
    std::thread reader(reader_fn);

    // 看门狗：并发 bug 的另一种表现是死锁，不能让测试无限挂住
    auto joiner = std::async(std::launch::async, [&] { for (auto& t : ts) t.join(); });
    bool timed_out = (joiner.wait_for(std::chrono::seconds(120)) != std::future_status::ready);
    if (timed_out) {
        std::printf("  [FAIL] 并发阶段超时未结束——疑似死锁\n");
        stop.store(true);
        joiner.wait();
    }
    stop.store(true);
    reader.join();

    // 等线程池把在途订单跑完，再做最终检查
    pool.reset();

    auto final_v = check_all(eng.get_bots());
    record(final_v);

    // 覆盖度诊断：下单量太低说明 bot 早早全员停摆，后面的轮次都是空转——
    // "全部通过"在那种情况下没有意义，所以把它打出来而不是藏起来
    int st_run = 0, st_cool = 0, st_stop = 0, with_pos = 0;
    for (const auto& b : eng.get_bots()) {
        if (b.state == CcgBot::State::Running)  ++st_run;
        else if (b.state == CcgBot::State::Cooldown) ++st_cool;
        else ++st_stop;
        if (b.total_qty > 0) ++with_pos;
    }
    std::printf("  下单 %ld 笔 | 不变量破坏 %d 次 | 终态 运行%d/冷却%d/停止%d，持仓中%d\n",
                client->orders.load(), torn.load(), st_run, st_cool, st_stop, with_pos);
    if (client->nan_qty_orders.load() > 0)
        std::printf("  [FAIL] 有 %ld 笔下单带着 NaN/Inf 数量\n", client->nan_qty_orders.load());
    if (client->bad_open.load() > 0)
        std::printf("  [FAIL] 有 %ld 笔【开仓】单数量 <=0\n", client->bad_open.load());
    if (client->bad_close.load() > 0)
        std::printf("  [FAIL] 有 %ld 笔【平仓】单数量 <=0\n", client->bad_close.load());
    if (torn.load() > 0)
        std::printf("  首个破坏: %s\n", first_violation.c_str());

    return !timed_out && torn.load() == 0 && client->nan_qty_orders.load() == 0 &&
           client->bad_open.load() == 0 && client->bad_close.load() == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// B. 极端行情（确定性：虚拟时钟 + 内联执行器）
// ─────────────────────────────────────────────────────────────────────────────

class ScriptClient : public ITradingClient {
public:
    double fill_price = 0;
    double fill_ratio = 1.0;    // 成交比例（0=零成交）
    bool   fail       = false;
    bool   uncertain  = false;
    long   orders     = 0;
    long   bad_qty    = 0;      // NaN/Inf/<=0 的下单次数

    OrderOutcome place_market_order(const std::string&, const std::string&,
                                    double qty, bool) override {
        ++orders;
        if (!std::isfinite(qty) || qty <= 0) ++bad_qty;
        OrderOutcome o;
        if (uncertain) { o.ok = false; o.uncertain = true; o.error = "状态不明"; return o; }
        if (fail)      { o.ok = false; o.error = "[-2019]"; return o; }
        o.ok = true;
        o.order_id = "M" + std::to_string(orders);
        o.avg_price = fill_price;
        o.executed_qty = qty * fill_ratio;
        return o;
    }
    double round_qty(const std::string&, double qty) override {
        if (!std::isfinite(qty)) return qty;
        double r = std::floor(qty * 1000.0) / 1000.0;
        return r < 0.001 ? 0.0 : r;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }
};

static EngineHost make_host(int64_t& vnow_ms) {
    EngineHost h;
    h.now_wall = [&vnow_ms] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    h.now_steady = [&vnow_ms] {
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    h.submit = [](std::function<void()> fn) { fn(); };   // 内联=确定性
    return h;
}

static CcgConfig base_cfg(const std::string& sym) {
    CcgConfig c;
    c.symbol          = sym;
    c.direction       = CcgConfig::Direction::Long;
    c.budget_usdt     = 1000;
    c.max_entries     = 6;
    c.leverage        = 5;
    c.entry_mode      = CcgConfig::EntryMode::Immediate;
    c.dynamic_band_mode = false;      // 用静态参数，行情脚本才好控制
    c.interval_pct    = 5.0;
    c.trail_entry     = 0.5;
    c.tp_pct          = 3.0;
    c.trail_tp        = 1.0;
    c.use_trend_filter = false;
    c.use_htf_filter = c.use_sr_support = c.use_sr_headroom = false;
    c.auto_restart    = true;
    c.cooldown_secs   = 0;
    return c;
}

// 跑一段价格序列，每 tick 后查不变量
static std::string run_prices(CcgEngine& eng, ScriptClient& cli, int64_t& vnow,
                              const std::string& sym,
                              const std::vector<double>& prices) {
    for (double p : prices) {
        cli.fill_price = p;
        vnow += 3000;
        eng.tick(sym, p);
        auto v = check_all(eng.get_bots());
        if (!v.empty()) return v + "（价格 " + std::to_string(p) + "）";
    }
    return "";
}

static void extreme_scenarios() {
    // B1 闪崩：满仓后一根 -70%
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        std::vector<double> px = {100, 95, 94, 89, 88, 30, 29, 28};
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B1 闪崩 -70%：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
    }

    // B2 天地针：一根砸穿再一根拉回
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        std::vector<double> px = {100, 99, 50, 100, 101, 50, 102, 103};
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B2 天地针：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
    }

    // B3 非法价格：0 / 负数 / NaN / Inf 都不得产生下单，也不得污染状态
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        cli->fill_price = 100;
        vnow += 3000; eng.tick("XUSDT", 100);       // 先正常建首仓
        long before = cli->orders;

        const double nan_v = std::numeric_limits<double>::quiet_NaN();
        const double inf_v = std::numeric_limits<double>::infinity();
        for (double bad : {0.0, -1.0, -1e9, nan_v, inf_v, -inf_v}) {
            vnow += 3000;
            eng.tick("XUSDT", bad);
        }
        auto v = check_all(eng.get_bots());
        check(v.empty(), "B3 非法价格：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
        check(cli->bad_qty == 0,
              "B3 非法价格：没有下出 NaN/Inf/<=0 数量的单（实际 " +
              std::to_string(cli->bad_qty) + " 笔）");
        (void)before;
    }

    // B4 极端量级：极小价与极大价
    {
        for (double scale : {1e-7, 1e7}) {
            int64_t vnow = 0;
            auto cli = std::make_shared<ScriptClient>();
            CcgEngine eng(cli, make_host(vnow));
            eng.add_bot(base_cfg("XUSDT"));
            std::vector<double> px;
            for (int i = 0; i < 40; ++i) px.push_back(scale * (1.0 - 0.02 * i));
            auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
            check(v.empty(), "B4 极端量级 " + std::to_string(scale) +
                             "：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
            check(cli->bad_qty == 0, "B4 极端量级 " + std::to_string(scale) +
                                     "：无非法数量下单");
        }
    }

    // B5 单边下跌打满层，再 V 形拉回止盈
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        auto id = eng.add_bot(base_cfg("XUSDT"));
        std::vector<double> px;
        for (int i = 0; i < 200; ++i) px.push_back(100.0 * std::pow(0.985, i));
        for (int i = 0; i < 200; ++i) px.push_back(px.back() * 1.02);
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B5 满层后V形反弹：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
        auto bots = eng.get_bots();
        bool layer_ok = true;
        for (const auto& b : bots)
            if ((int)b.entries.size() > b.cfg.max_entries) layer_ok = false;
        check(layer_ok, "B5 层数从不超过 max_entries");
        (void)id;
    }

    // B6 零成交绝不入账（市价单被接受但 EXPIRED）
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        cli->fill_ratio = 0.0;
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        for (int i = 0; i < 20; ++i) { cli->fill_price = 100; vnow += 3000; eng.tick("XUSDT", 100); }
        auto bots = eng.get_bots();
        bool clean = !bots.empty() && bots[0].entries.empty() && bots[0].total_qty == 0;
        check(clean, "B6 零成交不入账（无幽灵仓位）");
        check(check_all(bots).empty(), "B6 零成交：状态不撕裂");
    }

    // B7 连环部分成交：每次只成交 40%
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        cli->fill_ratio = 0.4;
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        std::vector<double> px;
        for (int i = 0; i < 60; ++i) px.push_back(100.0 * (1.0 - 0.01 * (i % 12)));
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B7 连环部分成交：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
    }

    // B8 状态不明必须停机，不能盲目重试造成双倍仓位
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        cli->uncertain = true;
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));
        for (int i = 0; i < 10; ++i) { vnow += 3000; eng.tick("XUSDT", 100); }
        auto bots = eng.get_bots();
        bool stopped = !bots.empty() && bots[0].state == CcgBot::State::Stopped;
        check(stopped, "B8 下单状态不明 → bot 已停机等人工核对");
        check(cli->orders == 1, "B8 状态不明后不再重复下单（实际 " +
                                std::to_string(cli->orders) + " 笔）");
    }

    // B9 止盈-冷却-重开 循环 300 轮，检查无状态残留
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        auto cfg = base_cfg("XUSDT");
        cfg.cooldown_secs = 0;
        eng.add_bot(cfg);
        std::vector<double> px;
        for (int r = 0; r < 300; ++r) {
            px.push_back(100); px.push_back(104); px.push_back(105); px.push_back(102);
        }
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B9 300轮止盈-重开：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
        auto bots = eng.get_bots();
        bool bounded = !bots.empty() && bots[0].entries.size() <= (size_t)cfg.max_entries;
        check(bounded, "B9 300轮后层数仍有界（无累积泄漏）");
    }

    // B11 确定性复现①：NaN 价格 + 空仓 + Immediate 模式 → 会不会下出 NaN 数量的单
    // tick() 的守卫是 `if (price <= 0) return;`，而 NaN 与任何数比较都是 false，
    // 所以 NaN 从这道守卫底下直接穿过去
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        eng.add_bot(base_cfg("XUSDT"));            // 空仓 + Immediate
        vnow += 3000;
        eng.tick("XUSDT", std::numeric_limits<double>::quiet_NaN());
        check(cli->bad_qty == 0,
              "B11 NaN 价格开首仓：不产生非法数量的下单（实际 " +
              std::to_string(cli->bad_qty) + " 笔）");
        auto v = check_all(eng.get_bots());
        check(v.empty(), "B11 NaN 价格：状态不被污染" + (v.empty() ? "" : "  →  " + v));
    }

    // B12 确定性复现②：开仓部分成交到【低于最小下单量】的仓位，还能不能平掉。
    // 关键在于这条路径绕开了"灰尘结算"——灰尘逻辑只在平仓【成功】时才跑，而这里
    // 平仓单本身就发不出去（数量取整为0被交易所拒），于是每个 tick 重发一次，永远卡死。
    // 构造：首仓 3/21≈0.1428U @100 → 0.001428 取整 0.001，只成交 50% → 持仓 0.0005，
    // 已低于最小下单量 0.001
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        auto cfg = base_cfg("XUSDT");
        cfg.budget_usdt = 3.0;
        cfg.max_entries = 6;
        eng.add_bot(cfg);
        cli->fill_price = 100;
        cli->fill_ratio = 0.5;                      // 开仓只成交一半
        vnow += 3000; eng.tick("XUSDT", 100);

        auto b0 = eng.get_bots();
        bool below_min = !b0.empty() && b0[0].total_qty > 0 && b0[0].total_qty < 0.001;
        check(below_min, "B12 构造成功：持仓量低于最小下单量（" +
                         (b0.empty() ? std::string("无bot") : std::to_string(b0[0].total_qty)) + "）");

        cli->fill_ratio = 1.0;
        long before = cli->bad_qty;
        std::vector<double> px = {104, 105, 102, 102, 102, 102, 102, 102};
        for (double p : px) { cli->fill_price = p; vnow += 3000; eng.tick("XUSDT", p); }
        check(cli->bad_qty == before,
              "B12 不可平的残仓：不发数量为0的平仓单（实际新增 " +
              std::to_string(cli->bad_qty - before) + " 笔）");
        auto b1 = eng.get_bots();
        bool cleared = !b1.empty() && (b1[0].total_qty <= 0 ||
                                       b1[0].state == CcgBot::State::Stopped);
        check(cleared, "B12 不可平的残仓：最终被结清或停机，不是每tick空转重试");
    }

    // B13 确定性复现③：平仓在途期间对账插进来，会不会把状态写回成不一致。
    // submit_close 在发 HTTP 时【不持锁】（这是对的，否则整个引擎会卡在网络时长上），
    // 但回来之后用的是【调用前的旧快照】 total_qty 做绝对赋值：
    //     bot.total_qty = total_qty(旧) - closed_qty
    // 这中间若对账把仓位清空了（交易所侧已无仓位 → entries 清空、total_qty=0），
    // 这一行会把一个已经作废的数量重新写回去，得到"没有任何加仓记录、却有持仓量"
    // 的撕裂状态。用一个在下单过程中回调引擎的假客户端把这个窗口固定下来。
    {
        int64_t vnow = 0;
        struct ReentrantClient : ScriptClient {
            CcgEngine* eng = nullptr;
            bool fired = false;
            OrderOutcome place_market_order(const std::string& s, const std::string& side,
                                            double qty, bool reduce_only) override {
                // 只在【平仓】那一次插入对账，模拟"HTTP 往返期间对账线程跑了一轮"
                if (reduce_only && eng && !fired) {
                    fired = true;
                    eng->reconcile_positions({});      // 交易所侧空仓位 = 外部已平掉
                }
                return ScriptClient::place_market_order(s, side, qty, reduce_only);
            }
        };
        auto cli = std::make_shared<ReentrantClient>();
        CcgEngine eng(cli, make_host(vnow));
        cli->eng = &eng;
        auto cfg = base_cfg("XUSDT");
        eng.add_bot(cfg);

        cli->fill_price = 100;
        vnow += 3000; eng.tick("XUSDT", 100);       // 建首仓
        cli->fill_ratio = 0.5;                       // 平仓只成交一半 → 走部分成交分支
        for (double p : {104.0, 105.0, 102.0}) {
            cli->fill_price = p; vnow += 3000; eng.tick("XUSDT", p);
        }
        auto v = check_all(eng.get_bots());
        check(v.empty(), "B13 平仓在途遇对账：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
    }

    // B10 保证金上限：并发压力之外的确定性检查——上限之内绝不越界
    {
        int64_t vnow = 0;
        auto cli = std::make_shared<ScriptClient>();
        CcgEngine eng(cli, make_host(vnow));
        auto cfg = base_cfg("XUSDT");
        cfg.budget_usdt = 1000; cfg.leverage = 5; cfg.max_entries = 6;
        eng.add_bot(cfg);
        eng.set_max_total_margin(60.0);   // 只够开头一两层
        std::vector<double> px;
        for (int i = 0; i < 120; ++i) px.push_back(100.0 * std::pow(0.99, i));
        auto v = run_prices(eng, *cli, vnow, "XUSDT", px);
        check(v.empty(), "B10 保证金上限下：状态不撕裂" + (v.empty() ? "" : "  →  " + v));
        // 允许一笔在途的超额（闸门是"下一笔会不会超"，不是硬截断）
        double used = eng.total_margin_used();
        check(used <= 60.0 + 1000.0 / 6.0 / 5.0 + 1e-6,
              "B10 已用保证金不超过上限+单层额度（实际 " + std::to_string(used) + "）");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // 默认值按【覆盖度】定，不是按耗时定：4000 轮只跑得出几十笔单，止盈/冷却/重开
    // 那半条生命周期基本碰不到，"全部通过"没有意义。50000 轮本机约 2.5 秒，
    // CI 上就算慢 20 倍也远在看门狗之内
    int rounds = (argc > 1) ? std::atoi(argv[1]) : 50000;
    if (rounds < 100) rounds = 100;

    std::printf("── B. 极端行情（确定性）──\n");
    extreme_scenarios();

    std::printf("\n── A. 并发压力（真线程池，每轮 %d 次）──\n", rounds);
    for (unsigned seed : {1u, 7u, 99u}) {
        std::printf("[seed %u]\n", seed);
        bool ok = concurrency_stress(seed, rounds);
        check(ok, "并发压力 seed=" + std::to_string(seed) + "：无撕裂/无死锁/无非法下单");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
