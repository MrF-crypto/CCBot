// SAR 引擎压力测试 —— 实盘前的最后一道自检。
//
// 接替随网格DCA 一并删除的 ccg_stress_tests。为什么必须有一个接替者：其余各套
// 测试都在验证"给定输入产生正确输出"，但实盘杀死交易系统的往往不是算错，而是
// 【并发下的状态撕裂】和【没人想过的极端输入】。这两类在单线程、行情正常的测试里
// 永远不会出现。而且它是 TSan 矩阵的目标之一——DCA 走了，这份覆盖不能跟着走。
//
// 分两部分：
//   A. 并发压力 —— 真线程池 + 多线程同时 tick / 喂信号 / 读快照 / 对账 /
//      停开 bot，交易所返回随机化（全成交/零成交/失败/状态不明/-2022）。
//      GUI 里 tick() 本来就会被多个线程调用（WS 价格走界面线程、REST 兜底走
//      数据池线程），所以这不是臆想的场景。
//   B. 极端行情 —— 闪崩、天地针、非法价格（0/负/NaN/inf）、极端量级、
//      ATR 缺失、连环反手。用虚拟时钟 + 内联执行器，完全确定可复现。
//
// 判据用【不变量】而不是"期望值"：并发下没有唯一正确的输出，但
// "pos==Flat 与 qty==0 必须同真同假"这类约束在任何交错下都得成立，
// 一旦破了就是真的状态撕裂。
#include "core/trend_engine.h"
#include "core/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
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

// ── 随机化的假交易所 ─────────────────────────────────────────────────────────
// 每一笔都可能是：全成交 / 零成交 / 失败 / 状态不明 / reduceOnly 被拒。
// 固定种子，失败可复现
class ChaosClient : public ITradingClient {
public:
    explicit ChaosClient(uint32_t seed) : rng_(seed) {}

    std::atomic<int>    orders{0};
    std::atomic<double> price{100.0};

    OrderOutcome place_market_order(const std::string&, const std::string&,
                                    double qty, bool reduce_only) override {
        ++orders;
        OrderOutcome o;
        int roll;
        {
            std::lock_guard<std::mutex> lk(m_);
            roll = (int)(rng_() % 100);
        }
        if (roll < 8) {                       // 硬失败
            o.ok = false; o.error = "boom";
            return o;
        }
        if (roll < 14) {                      // 状态不明：最凶险的一类
            o.ok = false; o.uncertain = true; o.error = "timeout";
            return o;
        }
        if (roll < 20 && reduce_only) {       // reduceOnly 被拒（仓位已不存在）
            o.ok = false; o.error = "-2022 ReduceOnly Order is rejected";
            return o;
        }
        o.ok = true;
        o.order_id = "M" + std::to_string(orders.load());
        o.avg_price = price.load();
        o.executed_qty = (roll < 26) ? 0.0                       // 零成交
                       : (roll < 34) ? qty * 0.4                 // 部分成交
                                     : qty;
        return o;
    }
    double round_qty(const std::string&, double q) override {
        return std::floor(q / 0.001) * 0.001;
    }
    StopPlacement place_disaster_stop(const std::string&, double sp,
                                      const std::string&) override {
        if (!(sp > 0)) {
            ++bad_stop_price;
            StopPlacement p; p.error = "非正触发价"; p.retryable = false;
            return p;
        }
        std::lock_guard<std::mutex> lk(m_);
        if (rng_() % 5 == 0) { StopPlacement p; p.error = "随机挂单失败"; return p; }
        StopPlacement p; p.order_id = "DS" + std::to_string(++ds_);
        return p;
    }
    bool cancel_disaster_stop(const std::string&, const std::string&) override {
        return true;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }

    // 交易所侧止损单的触发价必须永远是正数。挂一张触发价 <=0 的单在真实交易所上
    // 会被拒，但更糟的情况是被接受——那等于一张会立刻成交的单
    std::atomic<int> bad_stop_price{0};

private:
    std::mutex m_;
    std::mt19937 rng_;
    int ds_ = 0;
};

// ── 不变量 ───────────────────────────────────────────────────────────────────
// 这些在【任何】线程交错下都必须成立。破了就是状态撕裂，不是"结果不理想"。
// ⚠ 不能叫 finite()：POSIX 的 <math.h> 里有个同名的 finite(double)，
//   在 glibc 上会撞成 "ambiguating new declaration"。MSVC 没有那个函数，
//   所以这个错只有 Linux 报
static bool is_finite(double v) { return std::isfinite(v); }

static void check_invariants(const std::vector<TrendBot>& bots, const std::string& tag) {
    for (const auto& b : bots) {
        const std::string p = tag + " [" + b.cfg.symbol + "] ";

        // ① 数量永不为负。负数量会让后续所有名义/保证金计算符号翻转
        if (!(b.qty >= 0)) { check(false, p + "qty 不为负"); return; }

        // ② 【核心不变量】方向与数量必须同真同假。
        //    撕裂的后果正是这套策略最致命的失败模式：引擎以为自己空仓，
        //    下一个信号会再开一笔 → 净敞口翻倍且方向不明；反过来则是
        //    "以为有仓"，止损线守着一个不存在的仓位
        const bool flat_by_pos = (b.st.pos == trend::Pos::Flat);
        const bool flat_by_qty = (b.qty <= 0);
        if (flat_by_pos != flat_by_qty) {
            check(false, p + "pos 与 qty 一致（pos=" +
                  std::string(flat_by_pos ? "Flat" : "非Flat") +
                  " qty=" + std::to_string(b.qty) + "）");
            return;
        }

        // ③ 有仓位就必须有开仓价和止损线。缺任何一个都是一笔没有底的裸仓位
        if (!flat_by_pos) {
            if (!(b.st.entry_price > 0)) { check(false, p + "持仓时开仓价 > 0"); return; }
            if (!(b.st.stop > 0))        { check(false, p + "持仓时止损线 > 0"); return; }
        }
        // ⚠ 这里【不】断言"止损线在极值的不利侧"。那条关系在两种真实情况下会被
        //   合法地打破，而两者都不是状态损坏——引擎下一个 tick 就会把仓位平掉：
        //     ① 市价单滑点：止损线是按信号价算的，成交价可能更差。空头成交在
        //        止损线【之上】时，仓位在建立的那一刻就已经越线
        //     ② 浮点 ULP：k×ATR 小于价格的最小间隔时止损线与极值重合
        //        （1e9 - 3e-9 == 1e9），此时 price <= stop 成立，立即出场
        //   这条关系在【成交价精确】的确定性场景里才该成立，见 Part B 的专门用例

        // ④ 任何浮点字段都不得是 NaN/Inf。这条单独立着是因为 NaN 会
        //    【静默传播】：带着 NaN 数量去下单，比较运算全为假，所有闸门失效
        if (!is_finite(b.qty) || !is_finite(b.st.stop) || !is_finite(b.st.entry_price) ||
            !is_finite(b.st.peak) || !is_finite(b.realized_pnl) ||
            !is_finite(b.current_price)) {
            check(false, p + "无 NaN/Inf"); return;
        }

        // ⑤ 统计计数自洽
        if (!(b.win_count >= 0 && b.trade_count >= 0 && b.win_count <= b.trade_count)) {
            check(false, p + "胜场数 ≤ 总笔数"); return;
        }

        // ⑥ 记了单号就必须有触发价，否则重启后会去撤一张不知道挂在哪的单
        if (!b.disaster_stop_id.empty() && !(b.disaster_stop_price > 0)) {
            check(false, p + "有止损单号则必有触发价"); return;
        }
    }
}

// ── A. 并发压力 ──────────────────────────────────────────────────────────────
static void part_a_concurrency() {
    std::printf("\n── A. 并发压力（真线程池，多线程同时读写）──\n");

    auto client = std::make_shared<ChaosClient>(20260929u);
    auto pool   = std::make_shared<ThreadPool>(4);
    TrendEngine eng(client, pool);

    const std::vector<std::string> syms = {
        "BTCUSDT", "ETHUSDT", "SOLUSDT", "ZECUSDT", "PEPEUSDT", "LTCUSDT"
    };
    // ⚠ bot_id 是引擎给的序号（"sar1"、"sar2"…），不是品种名派生的。
    //   必须把 add_bot 的返回值存下来——自己拼一个 id 去喂信号，update_signal
    //   会静默找不到那个 bot，于是【一笔都开不出来】，而所有不变量都平凡成立。
    //   这份测试第一版就是这么假绿的：跑了 256 万次 tick、下单 0 笔
    std::map<std::string, std::string> ids;
    // 三个策略必须同时在跑。只跑一个的话，另外两个的喂入路径（update_bars 的
    // 两套字段、PSAR 的 AF 递推）根本没有并发压力，而它们各自都会写 State
    std::set<std::string> bare_syms, psar_syms;
    for (const auto& s : syms) {
        TrendConfig c;
        c.symbol      = s;
        c.budget_usdt = 500;
        c.leverage    = 3;
        c.interval    = "3m";

        if (s == "ZECUSDT") {                       // ③ 裸K · 等收盘突破
            c.rule.strategy   = trend::Strategy::BareK;
            c.rule.bare_entry = trend::BareEntry::BreakPrevBar;
            c.rule.swing_bars = 3;
            c.rule.reverse    = trend::ReverseMode::Immediate;
            bare_syms.insert(s);
        } else if (s == "LTCUSDT") {                // ③ 裸K · 盘中即时 + 护栏
            c.rule.strategy     = trend::Strategy::BareK;
            c.rule.bare_entry   = trend::BareEntry::Immediate;
            c.rule.once_per_bar = true;
            c.rule.swing_bars   = 3;
            c.rule.reverse      = trend::ReverseMode::None;
            bare_syms.insert(s);
        } else if (s == "PEPEUSDT") {               // ② 抛物线SAR
            c.rule.strategy = trend::Strategy::ParabolicSar;
            c.rule.reverse  = trend::ReverseMode::Immediate;
            c.rule.max_consecutive_reverses = 3;
            c.rule.cooldown_bars            = 2;
            psar_syms.insert(s);
        } else {                                    // ① 海龟
            c.rule.strategy = trend::Strategy::Turtle;
            c.rule.atr_mult = 3.0;
            c.rule.reverse  = (s == "SOLUSDT") ? trend::ReverseMode::Immediate
                                               : trend::ReverseMode::None;
        }

        c.use_disaster_stop          = (s != "SOLUSDT");
        c.signal_max_age_sec         = 3600;
        const std::string id = eng.add_bot(c);
        check(!id.empty(), s + " bot 已建立");
        ids[s] = id;
    }

    std::atomic<bool> stop{false};
    std::atomic<long> ticks{0}, reads{0}, recons{0};
    std::vector<std::thread> ths;

    // 喂价线程 ×3：同一个品种会被多个线程同时 tick，这正是 GUI 的真实情况
    for (int t = 0; t < 3; ++t) {
        ths.emplace_back([&, t]() {
            std::mt19937 rng(1000u + (uint32_t)t);
            while (!stop.load()) {
                for (const auto& s : syms) {
                    // 价格随机游走，偶尔来一次 ±20% 的跳空
                    double px = client->price.load();
                    const double step = ((double)(rng() % 2001) - 1000.0) / 10000.0;  // ±10%
                    px *= (1.0 + step * 0.05);
                    if (rng() % 97 == 0) px *= (rng() % 2 ? 1.2 : 0.8);
                    if (px < 0.01) px = 0.01;
                    client->price.store(px);
                    eng.tick(s, px);
                    ++ticks;
                }
            }
        });
    }

    // 信号线程：两种模式各自的喂入接口
    ths.emplace_back([&]() {
        std::mt19937 rng(7u);
        int64_t bar = 1'700'000'000'000LL;
        while (!stop.load()) {
            const double px = client->price.load();
            const double atr = px * 0.02;
            for (const auto& s : syms) {
                const std::string& id = ids[s];
                if (bare_syms.count(s) || psar_syms.count(s)) {
                    // ⚠ v5.1 起 update_bars【自己】置 sig_ok，非海龟策略只调这一次。
                    //   上一版必须成对调用 update_signal 才能把 sig_ok 置起，
                    //   漏一次的表现是"配置正常、日志正常、一单不开"
                    TrendEngine::BarSnap bs;
                    bs.open = px;
                    // 收盘价随机落在前一根区间内外，于是突破信号时有时无
                    bs.prev_close = px * (0.95 + (double)(rng() % 100) / 1000.0);
                    bs.prev_high  = px * 1.02; bs.prev_low  = px * 0.98;
                    bs.prev2_high = px * 1.03; bs.prev2_low = px * 0.97;
                    bs.swing_low  = px * 0.97; bs.swing_high = px * 1.03;
                    bs.bar_open_ms = bar;
                    eng.update_bars(id, bs);
                } else {
                    // 通道要【落在现价之内】才可能被突破：dc_up 高于现价的话
                    // "价格 > 上沿"永远不成立，一笔都开不出来
                    eng.update_signal(id, atr, 2.0, true,
                                      px * 0.99, px * 1.01, bar);
                }
            }
            bar += 180'000;   // 3m
            std::this_thread::yield();
        }
    });

    // 快照读取线程：界面每 100ms 就在干这件事
    ths.emplace_back([&]() {
        while (!stop.load()) {
            auto snap = eng.get_bots();
            check_invariants(snap, "并发中");
            ++reads;
            if (g_fail) return;   // 已经破了，别再刷屏
        }
    });

    // 对账线程：周期对账与 tick 并发，这是实盘的真实节奏
    ths.emplace_back([&]() {
        std::mt19937 rng(99u);
        while (!stop.load()) {
            std::vector<TrendEngine::ExchangePos> ex;
            for (const auto& s : syms) {
                if (rng() % 3 == 0) continue;               // 交易所"没有"这个仓位
                TrendEngine::ExchangePos p;
                p.symbol      = s;
                p.direction   = (rng() % 2) ? 1 : -1;
                p.qty         = (double)(rng() % 50) / 10.0;
                p.entry_price = client->price.load();
                ex.push_back(p);
            }
            eng.reconcile_positions(ex);
            ++recons;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // 停开线程：用户在界面上点停止/继续，与上面全部并发。
    // 只碰固定的两个品种——全都随机停的话大部分时间没有 bot 在跑，
    // 压力测试就退化成了"停开接口不崩"
    ths.emplace_back([&]() {
        std::mt19937 rng(555u);
        const std::vector<std::string> toggling = {"SOLUSDT", "PEPEUSDT"};
        while (!stop.load()) {
            const std::string& id = ids[toggling[rng() % toggling.size()]];
            if (rng() % 2) eng.stop_bot(id); else eng.resume_bot(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    stop.store(true);
    for (auto& t : ths) t.join();
    pool->wait_idle(15000);

    std::printf("   tick %ld 次 / 快照 %ld 次 / 对账 %ld 次 / 下单 %d 笔\n",
                ticks.load(), reads.load(), recons.load(), client->orders.load());
    check(ticks.load() > 1000, "确实跑起了规模（tick > 1000）");
    // ⚠ 这条是这份测试的【有效性自检】，不是业务断言。一笔单都没下过的话，
    //   所有 bot 全程空仓，上面那些不变量平凡成立——测试全绿而什么都没测。
    //   第一版正是这样假绿的（bot_id 拼错，信号一条都没进去）
    check(client->orders.load() > 0,
          "确实产生了下单（否则全程空仓，不变量平凡成立、等于没测）");

    // 风暴过后的静态检查
    auto final_bots = eng.get_bots();
    check_invariants(final_bots, "风暴后");
    check(g_fail == 0 ? true : false, "并发全程不变量成立");

    check(client->bad_stop_price.load() == 0,
          "从未用非正触发价去挂交易所止损单");

    // pending 卡死检查：在途标记永久为真的话那个 bot 从此再也不响应任何 tick，
    // 界面显示"运行中"而实际已经僵死——这种故障不报错，只是安静地不再交易
    int stuck = 0;
    for (const auto& b : final_bots) if (b.pending) ++stuck;
    check(stuck == 0, "线程池排空后没有 bot 的 pending 卡在 true（僵死检查）");

    // stop_all 必须真的把每一个都停掉：漏一个的表现是它还在跑而用户以为停了
    eng.stop_all();
    bool all_stopped = true;
    for (const auto& b : eng.get_bots())
        if (b.state != TrendBot::State::Stopped) all_stopped = false;
    check(all_stopped, "stop_all 之后无一例外全部为 Stopped");
}

// ── B. 极端行情 ──────────────────────────────────────────────────────────────
static std::chrono::steady_clock::time_point g_now{};
static EngineHost inline_host() {
    EngineHost h;
    h.submit     = [](std::function<void()> fn) { fn(); };
    h.now_steady = [] { return g_now; };
    return h;
}

static void part_b_extremes() {
    std::printf("\n── B. 极端行情（虚拟时钟 + 内联执行器，确定可复现）──\n");

    // 这里要的是"极端输入不会破坏状态"，所以交易所侧一律正常成交，
    // 把故障源收敛到输入本身
    class OkClient : public ITradingClient {
    public:
        double px = 100.0;
        int n = 0;
        OrderOutcome place_market_order(const std::string&, const std::string&,
                                        double qty, bool) override {
            OrderOutcome o;
            o.ok = true; o.order_id = "M" + std::to_string(++n);
            o.avg_price = px; o.executed_qty = qty;
            return o;
        }
        double round_qty(const std::string&, double q) override {
            return std::floor(q / 0.001) * 0.001;
        }
        bool set_leverage(const std::string&, int) override { return true; }
        bool is_dual_mode() const override { return false; }
    };

    auto mk = [](const std::string& sym) {
        TrendConfig c;
        c.symbol      = sym;
        c.budget_usdt = 1000;
        c.leverage    = 3;
        c.interval    = "4h";
        c.rule.atr_mult = 3.0;
        c.signal_max_age_sec = 100000;
        return c;
    };

    // 通道必须【落在现价之内】才可能被突破：dc_up 高于现价的话"价格 > 上沿"
    // 永远不成立，一笔都开不出来（这份测试第一版全栽在这上面）
    constexpr double kUp = 99.0, kDn = 98.0;   // 配 price=100 → 向上突破

    // ① 非法价格一律不得改变任何状态
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        const std::string id = eng.add_bot(mk("BTCUSDT"));
        eng.update_signal(id, 2.0, 2.0, true, kUp, kDn, 1);
        eng.tick("BTCUSDT", 100.0);          // 正常建仓
        check(eng.get_bots().at(0).st.pos != trend::Pos::Flat, "基准：已建仓");
        const auto before = eng.get_bots().at(0);

        const double nan_v = std::numeric_limits<double>::quiet_NaN();
        const double inf_v = std::numeric_limits<double>::infinity();
        for (double bad : {0.0, -1.0, -1e9, nan_v, inf_v, -inf_v}) {
            eng.tick("BTCUSDT", bad);
        }
        const auto after = eng.get_bots().at(0);
        check(after.qty == before.qty && after.st.pos == before.st.pos &&
              after.st.stop == before.st.stop,
              "0 / 负数 / NaN / ±Inf 价格全部被拒，状态一个字节都没动");
        check_invariants(eng.get_bots(), "非法价格后");
    }

    // ①b 止损线必须在极值的【不利侧】——成交价精确时这条必须成立。
    // 反了的话止损线会在开仓瞬间触发，策略变成"开一笔立刻平一笔"。
    // 放在这里而不是 check_invariants 里，理由见那个函数里的说明（滑点与 ULP）
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        const std::string id = eng.add_bot(mk("BTCUSDT"));
        eng.update_signal(id, 2.0, 2.0, true, kUp, kDn, 1);
        eng.tick("BTCUSDT", 100.0);                 // 向上突破 → 做多
        {
            const auto b = eng.get_bots().at(0);
            check(b.st.pos == trend::Pos::Long, "向上突破开多");
            check(b.st.stop < b.st.peak, "  多头止损线在极值下方");
            check(b.st.stop < b.st.entry_price, "  且在开仓价下方");
        }
        // 价格走高 → 棘轮必须只上移，且始终留在极值下方
        for (double px : {101.0, 105.0, 110.0, 108.0, 112.0}) {
            const double prev = eng.get_bots().at(0).st.stop;
            cl->px = px;
            eng.tick("BTCUSDT", px);
            const auto b = eng.get_bots().at(0);
            if (b.st.pos == trend::Pos::Flat) break;      // 被打掉了，后面不用再看
            check(b.st.stop >= prev, "  棘轮只上移（不回退）");
            check(b.st.stop < b.st.peak, "  始终在极值下方");
        }
    }
    {
        // 空头镜像
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        const std::string id = eng.add_bot(mk("ETHUSDT"));
        // 向下突破：通道下沿要在现价【之上】
        eng.update_signal(id, 2.0, 2.0, true, 102.0, 101.0, 1);
        eng.tick("ETHUSDT", 100.0);
        const auto b = eng.get_bots().at(0);
        check(b.st.pos == trend::Pos::Short, "向下突破开空");
        check(b.st.stop > b.st.peak,        "  空头止损线在极值上方");
        check(b.st.stop > b.st.entry_price, "  且在开仓价上方");
    }

    // ② 闪崩：一个 tick 直接穿到止损线远下方。
    //    必须出场，而且出场后 pos 与 qty 要同时归零
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        auto c = mk("ETHUSDT");
        c.rule.reverse = trend::ReverseMode::None;
        const std::string id = eng.add_bot(c);
        eng.update_signal(id, 2.0, 2.0, true, kUp, kDn, 1);
        eng.tick("ETHUSDT", 100.0);
        check(eng.get_bots().at(0).st.pos != trend::Pos::Flat, "闪崩前已持仓");

        cl->px = 5.0;
        eng.tick("ETHUSDT", 5.0);       // -95%
        const auto b = eng.get_bots().at(0);
        check(b.st.pos == trend::Pos::Flat && b.qty <= 0,
              "闪崩 -95% 触发出场，pos 与 qty 同时归零");
        check_invariants(eng.get_bots(), "闪崩后");
    }

    // ③ 天地针 + 极端量级：微价币与高价币各来一次
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        const std::string id = eng.add_bot(mk("PEPEUSDT"));
        eng.update_signal(id, 1e-9, 2.0, true, 0.99e-7, 0.98e-7, 1);
        cl->px = 1e-7;
        eng.tick("PEPEUSDT", 1e-7);
        check(eng.get_bots().at(0).st.pos != trend::Pos::Flat, "微价也能正常建仓");
        check_invariants(eng.get_bots(), "微价建仓");
        for (double px : {1e-7 * 50, 1e-7 * 0.02, 1e-7, 1e9}) {
            cl->px = px;
            eng.tick("PEPEUSDT", px);
            check_invariants(eng.get_bots(), "天地针中");
        }
        check(g_fail == 0, "极端量级（1e-9 ~ 1e9）全程不变量成立");
    }

    // ④ ATR 缺失（数据断流）时，已有的止损线必须【沿用】而不是被清掉。
    //    清掉等于行情断流的那一刻保护自己消失了——而断流恰恰是最需要它的时候
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        const std::string id = eng.add_bot(mk("SOLUSDT"));
        eng.update_signal(id, 2.0, 2.0, true, kUp, kDn, 1);
        eng.tick("SOLUSDT", 100.0);
        const double stop0 = eng.get_bots().at(0).st.stop;
        check(stop0 > 0, "已建立止损线");

        eng.update_signal(id, 0, 0, false, 0, 0, 2);   // ATR=0 表示数据不足
        cl->px = 101.0;
        eng.tick("SOLUSDT", 101.0);
        const auto b = eng.get_bots().at(0);
        check(b.st.pos != trend::Pos::Flat, "ATR 缺失不会凭空平仓");
        check(b.st.stop >= stop0, "ATR 缺失时止损线沿用旧值（不得被清零或回退）");
        check_invariants(eng.get_bots(), "ATR 缺失后");
    }

    // ⑤ 连环反手：来回穿越，反手次数必须被 max_consecutive_reverses 顶住。
    //    不顶的话震荡市就是绞肉机——每次往返吃掉约 2k×ATR 的名义
    {
        auto cl = std::make_shared<OkClient>();
        TrendEngine eng(cl, inline_host());
        auto c = mk("LTCUSDT");
        c.rule.reverse                   = trend::ReverseMode::Immediate;  // 最激进
        c.rule.max_consecutive_reverses  = 2;
        c.rule.cooldown_bars             = 3;
        const std::string id = eng.add_bot(c);

        int64_t bar = 1;
        double px = 100.0;
        for (int i = 0; i < 40; ++i) {
            // 通道跟在【当前价】之内，下一拍的价格跳到通道外 → 必然产生突破。
            // 这样才真的在来回穿越，而不是喂一个永远撞不到的通道
            eng.update_signal(id, 2.0, 2.0, true, px * 0.99, px * 0.98, ++bar);
            px = (i % 2) ? 90.0 : 110.0;    // 每根K线反向 20%
            cl->px = px;
            eng.tick("LTCUSDT", px);
            check_invariants(eng.get_bots(), "连环反手中");
            if (g_fail) break;
        }
        const auto b = eng.get_bots().at(0);
        check(g_fail == 0, "40 根来回穿越的K线，不变量全程成立");
        check(b.trade_count > 0, "确实产生了成交（测试本身有效）");
        std::printf("   连环反手后：成交 %d 笔，实现盈亏 %.2f\n",
                    b.trade_count, b.realized_pnl);
    }
}

int main() {
    part_a_concurrency();
    part_b_extremes();
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
