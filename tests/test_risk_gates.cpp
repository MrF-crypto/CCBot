// 风控闸门的单元测试：账户级保证金上限、交易所侧灾难止损单。
//
// 这两条都是"出事才知道没写对"的路径，实盘里没法演练——所以用一个假的
// ITradingClient 把引擎单独拎出来跑。EngineHost 用内联执行器 + 虚拟时钟，
// 和回测同一套注入方式，结果完全确定可复现。
#include "core/ccg_engine.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}
static void check_near(double got, double want, double tol, const std::string& what) {
    bool ok = std::fabs(got - want) <= tol;
    std::printf("%s  %s (期望 %.4f，实际 %.4f)\n", ok ? "[ OK ]" : "[FAIL]",
                what.c_str(), want, got);
    if (!ok) ++g_fail;
}

// ── 假交易所：市价单永远全额成交在传入价，并记录灾难止损单的挂/撤 ────────────
class FakeClient : public ITradingClient {
public:
    double next_fill_price = 0;      // 下一笔成交价（由测试驱动）
    int    market_orders   = 0;

    struct StopCall { std::string kind; std::string symbol; double price; std::string id; };
    std::vector<StopCall> stop_log;
    int  place_seq   = 0;
    bool place_fails = false;        // 模拟挂单失败

    OrderOutcome place_market_order(const std::string&, const std::string&,
                                    double qty, bool) override {
        ++market_orders;
        OrderOutcome o;
        o.ok = true;
        o.order_id = "M" + std::to_string(market_orders);
        o.avg_price = next_fill_price;
        o.executed_qty = qty;
        return o;
    }
    double round_qty(const std::string&, double qty) override {
        // 3位小数，和 BTCUSDT 的 stepSize 一致
        return std::floor(qty * 1000.0) / 1000.0;
    }
    bool set_leverage(const std::string&, int) override { return true; }
    bool is_dual_mode() const override { return false; }

    std::string place_disaster_stop(const std::string& sym, double stop_price,
                                    const std::string&) override {
        if (place_fails) { stop_log.push_back({"place_fail", sym, stop_price, ""}); return ""; }
        std::string id = "S" + std::to_string(++place_seq);
        stop_log.push_back({"place", sym, stop_price, id});
        return id;
    }
    bool cancel_disaster_stop(const std::string& sym, const std::string& id) override {
        stop_log.push_back({"cancel", sym, 0, id});
        return true;
    }

    int count(const std::string& kind) const {
        int n = 0;
        for (const auto& c : stop_log) if (c.kind == kind) ++n;
        return n;
    }
    const StopCall* last(const std::string& kind) const {
        for (auto it = stop_log.rbegin(); it != stop_log.rend(); ++it)
            if (it->kind == kind) return &*it;
        return nullptr;
    }
};

// 虚拟时钟 + 内联执行器（同回测）
static EngineHost make_host(int64_t& vnow_ms) {
    EngineHost h;
    h.now_wall = [&vnow_ms] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    h.now_steady = [&vnow_ms] {
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(vnow_ms));
    };
    h.submit = [](std::function<void()> fn) { fn(); };
    return h;
}

// 关掉所有信号类闸门，只留要测的那一条，否则测试会被指标/趋势/结构数据卡住
static CcgConfig base_cfg() {
    CcgConfig c;
    c.symbol       = "BTCUSDT";
    c.direction    = CcgConfig::Direction::Long;
    c.strat_type   = CcgConfig::StratType::Flat;   // 平推：每层预算相同，便于手算
    c.budget_usdt  = 1000.0;
    c.leverage     = 1;                            // 保证金 = 名义价值，心算直观
    c.max_entries  = 4;                            // 每层 250U
    c.entry_mode   = CcgConfig::EntryMode::Immediate;
    c.dynamic_band_mode = false;                   // 用固定间隔，不依赖布林带数据
    c.interval_pct = 10.0;
    c.trail_entry  = 1.0;
    c.tp_pct       = 5.0;
    c.trail_tp     = 2.0;
    c.use_htf_filter   = false;
    c.use_sr_support   = false;
    c.use_sr_headroom  = false;
    c.use_trend_filter = false;
    c.sr_radar         = false;
    c.use_structural_stop = false;
    c.auto_restart     = false;
    return c;
}

// 走完一层补仓：跌破间隔 → 触底 → 反弹超过 trail_entry
static void drive_one_dca(CcgEngine& eng, FakeClient& fc, double from_price) {
    double bottom = from_price * 0.88;             // 跌 12% > 间隔 10%
    fc.next_fill_price = bottom * 1.02;
    eng.tick("BTCUSDT", bottom);                   // 探底，记录 dca_extreme
    eng.tick("BTCUSDT", bottom * 1.02);            // 反弹 2% > trail_entry 1% → 补仓
}

// ── 用例1：账户级保证金上限必须挡住【补仓】，不只是首仓 ──────────────────────
static void test_dca_margin_cap() {
    std::printf("\n── 用例1：补仓路径的账户级保证金上限 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    // 每层 250U、杠杆1 → 每层占用 250U 保证金。上限设 400：
    // 首仓 250 过闸，第二仓 250+250=500 > 400 必须被挡
    eng.set_max_total_margin(400.0);

    auto cfg = base_cfg();
    auto id  = eng.add_bot(cfg);
    check(!id.empty(), "创建 bot");

    fc->next_fill_price = 60000.0;
    eng.tick("BTCUSDT", 60000.0);                  // 首仓
    check(eng.get_bots()[0].entries.size() == 1, "首仓成交（250U 在 400U 上限内）");

    drive_one_dca(eng, *fc, 60000.0);
    auto snap1 = eng.get_bots();
    const auto& b = snap1[0];
    check(b.entries.size() == 1, "第2层补仓被保证金上限挡住（这是本次修复的核心）");
    check(b.last_action.find("保证金上限") != std::string::npos,
          "last_action 说明了拦截原因: " + b.last_action);

    // 放开上限后，同样的行情应该能补进去——证明拦截来自闸门而不是别的原因
    eng.set_max_total_margin(10000.0);
    drive_one_dca(eng, *fc, 60000.0 * 0.88 * 1.02);
    check(eng.get_bots()[0].entries.size() == 2, "放开上限后同样行情可以补仓");
}

// ── 用例2：交易所侧灾难止损单的完整生命周期 ─────────────────────────────────
static void test_disaster_stop_lifecycle() {
    std::printf("\n── 用例2：交易所侧灾难止损单生命周期 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    auto cfg = base_cfg();
    cfg.use_disaster_stop = true;                  // 独立开关，必须显式打开
    cfg.disaster_stop_pct = 30.0;                  // 均价 -30%
    auto id = eng.add_bot(cfg);
    check(!id.empty(), "创建 bot（灾难止损 30%）");

    // ① 首仓成交后应立即挂单，触发价 = 均价 × 0.70
    fc->next_fill_price = 60000.0;
    eng.tick("BTCUSDT", 60000.0);
    check(fc->count("place") == 1, "首仓成交后挂出 1 张灾难止损单");
    check_near(fc->last("place")->price, 60000.0 * 0.70, 1.0, "触发价 = 均价 -30%");

    // ② 补仓拉低均价后应【撤旧挂新】——触发价必须跟着均价下移，
    //    否则止损位相对新均价变得过近，正常回撤就会被打掉
    double avg_before = eng.get_bots()[0].avg_price;
    drive_one_dca(eng, *fc, 60000.0);
    auto snap2 = eng.get_bots();
    const auto& b = snap2[0];
    check(b.entries.size() == 2, "第2层补仓成交");
    check(b.avg_price < avg_before, "均价被补仓拉低");
    check(fc->count("cancel") == 1, "旧单被撤");
    check(fc->count("place")  == 2, "按新均价重新挂单");
    check_near(fc->last("place")->price, b.avg_price * 0.70, 1.0, "新触发价 = 新均价 -30%");
    check(b.disaster_stop_id == fc->last("place")->id, "bot 记住了新单号（会随状态落盘）");

    // ③ 平仓后必须撤单，否则残留挂单会让下一轮同方向挂单被交易所拒绝
    int cancels_before = fc->count("cancel");
    eng.close_bot(id);
    check(fc->count("cancel") == cancels_before + 1, "平仓后撤掉灾难止损单");
    check(eng.get_bots()[0].disaster_stop_id.empty(), "本地单号已清空");
}

// ── 用例3：默认关闭时不得产生任何交易所侧挂单 ────────────────────────────────
static void test_disabled_by_default() {
    std::printf("\n── 用例3：默认关闭（use_disaster_stop=false）──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    CcgConfig c;
    check(c.use_disaster_stop == false, "CcgConfig 默认不启用（不改变既有风险行为）");

    // 关键：比例默认是 30 而不是 0 —— 必须验证"开关关着时比例非零也不生效"，
    // 否则把默认比例改成非零的那天会静默给所有人挂上止损单
    check(c.disaster_stop_pct > 0, "默认比例非零（开关才是唯一的启停依据）");

    auto cfg = base_cfg();                          // use_disaster_stop 保持 false
    eng.add_bot(cfg);
    fc->next_fill_price = 60000.0;
    eng.tick("BTCUSDT", 60000.0);
    drive_one_dca(eng, *fc, 60000.0);
    check(fc->stop_log.empty(), "全程没有任何挂单/撤单调用");
}

// ── 用例4：挂单失败必须留下痕迹，不能静默 ────────────────────────────────────
static void test_place_failure_is_loud() {
    std::printf("\n── 用例4：挂单失败不能静默 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    std::string logged;
    eng.set_log_cb([&logged](const std::string& m) { logged += m + "\n"; });

    auto cfg = base_cfg();
    cfg.use_disaster_stop = true;
    cfg.disaster_stop_pct = 30.0;
    eng.add_bot(cfg);

    fc->place_fails = true;
    fc->next_fill_price = 60000.0;
    eng.tick("BTCUSDT", 60000.0);

    check(logged.find("没有进程外保护") != std::string::npos,
          "挂单失败写了明确的告警日志");
    check(eng.get_bots()[0].disaster_stop_id.empty(),
          "失败后本地不记单号（下次仓位变化会自动重试）");
}


// ── 用例5：多周期梯子的档位分配 ──────────────────────────────────────────────
// 分配逻辑边界很多（手动/权重/层数不足/求和对不齐），而它决定"第几层用哪个
// 周期的下轨"——错一位整个梯子的间距就全错了
static void test_mtf_alloc() {
    std::printf("\n── 用例5：多周期梯子档位分配 ──\n");
    auto A = [](int n, const char* spec) {
        CcgConfig c; c.max_entries = n; c.mtf_tier_layers = spec;
        return CcgEngine::mtf_tier_alloc(c);
    };
    auto eq = [](std::array<int,4> g, int a, int b, int c, int d, const std::string& what) {
        bool ok = (g[0]==a && g[1]==b && g[2]==c && g[3]==d);
        std::printf("%s  %s (期望 %d/%d/%d/%d，实际 %d/%d/%d/%d)\n",
            ok?"[ OK ]":"[FAIL]", what.c_str(), a,b,c,d, g[0],g[1],g[2],g[3]);
        if (!ok) ++g_fail;
    };

    eq(A(8, "3,2,2,1"), 3,2,2,1, "手动 3/2/2/1");
    eq(A(8, ""),        3,2,2,1, "空 → 按 3:2:2:1 权重（8层正好整除）");
    eq(A(8, "2,2,2,2"), 2,2,2,2, "手动 2/2/2/2");
    eq(A(8, "4,2,1,1"), 4,2,1,1, "手动 4/2/1/1");

    // 求和与 max_entries 对不齐：以 max_entries 为准，从最深档裁剪/补足
    eq(A(8, "3,3,3,3"), 3,3,2,0, "手动求和12>8 → 从最深档往前砍");
    eq(A(8, "1,1,1,1"), 5,1,1,1, "手动求和4<8 → 差额补给浅档");

    // 层数不足以铺满四档
    eq(A(3, ""),        2,1,0,0, "3层 → 只用 1h 和 4h（深档砍掉）");
    eq(A(1, ""),        1,0,0,0, "1层 → 只有首仓，全在 1h 档");
    eq(A(2, ""),        1,1,0,0, "2层");

    // 大层数
    auto g16 = A(16, "");
    int sum16 = g16[0]+g16[1]+g16[2]+g16[3];
    std::printf("%s  16层按权重求和=16 (实际 %d：%d/%d/%d/%d)\n",
        sum16==16?"[ OK ]":"[FAIL]", sum16, g16[0],g16[1],g16[2],g16[3]);
    if (sum16 != 16) ++g_fail;

    // 任何配置下总和都必须等于 max_entries——否则会出现"有槽位却没有档位归属"
    for (int n : {1,2,3,5,8,13,20,50}) {
        auto g = A(n, "");
        int sum = g[0]+g[1]+g[2]+g[3];
        if (sum != n) { std::printf("[FAIL]  %d层求和=%d\n", n, sum); ++g_fail; }
    }
    std::printf("[ OK ]  1~50 层的权重分配求和恒等于层数\n");
}

// ── 用例：首仓后瞬间砸穿全部四档下轨再反弹，到底补几层？────────────────────
// 这是实盘里最容易误解的一个场景。直觉上"跌破了日线下轨，深层该解锁了"，
// 但梯子是【按槽位顺序】走的：下一层归哪一档只取决于当前有几层持仓，
// 与"价格砸穿了多少条下轨"无关。而且每成交一层，间距基准就重置到新成交价。
// 结论应当是：一次 V 形急跌急拉只补【一层】，且是下一个槽位那一层。
static void test_v_crash_fills_one_layer() {
    std::printf("\n── 用例：V形急跌砸穿四档下轨后反弹 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    CcgConfig c = base_cfg();
    c.strat_type      = CcgConfig::StratType::Flat;   // 每层等额，便于核对
    c.budget_usdt     = 8000.0;                       // 8 层 × 1000U
    c.max_entries     = 8;
    c.mtf_ladder      = true;
    c.mtf_tier_layers = "5,1,1,1";
    c.mtf_k           = 0.3333;
    c.mtf_min_gap_pct = 0.3;
    auto id = eng.add_bot(c);

    // 四档带子（1h 带宽 2.5%，其余按 √T 缩放），中轨都在 100000
    eng.update_mtf_band(id, 0,  98750.0, 101250.0);   // 1h   W=2.50%
    eng.update_mtf_band(id, 1,  97500.0, 102500.0);   // 4h   W=5.00%
    eng.update_mtf_band(id, 2,  95670.0, 104330.0);   // 12h  W=8.66%
    eng.update_mtf_band(id, 3,  93875.0, 106125.0);   // 1d   W=12.25%

    fc->next_fill_price = 100000.0;
    eng.tick("BTCUSDT", 100000.0);                    // 首仓
    check(eng.get_bots()[0].entries.size() == 1, "首仓已建立");

    // 瞬间砸到 81500 —— 低于【全部四档】下轨（含日线 93875）
    fc->next_fill_price = 81500.0;
    vnow += 3000; eng.tick("BTCUSDT", 81500.0);
    {
        auto b = eng.get_bots()[0];
        check(b.entries.size() == 1,
              "砸穿四档下轨的那一刻【不下单】——只是武装并记录最低点");
        check(std::fabs(b.dca_extreme - 81500.0) < 1e-6, "  最低点已记为 81500");
    }

    // 反弹到 83000（自最低点 +1.84%，超过 1h 档要求的 0.25%）
    fc->next_fill_price = 83000.0;
    vnow += 3000; eng.tick("BTCUSDT", 83000.0);
    {
        auto b = eng.get_bots()[0];
        check(b.entries.size() == 2, "反弹达标 → 补【一】层");
        check(b.entries.back().price > 82000.0,
              "  成交价是【检测到反弹那一刻的价格】(83000)，不是最低点也不是最低点+0.25%");
    }

    // 继续反弹：不该再补。下一槽位仍归 1h 档，且间距基准已重置到 83000，
    // 需要再跌破 83000×(1-0.833%)=82309 才可能武装——价格在往上走
    for (double p : {85000.0, 88000.0, 92000.0, 95000.0}) {
        fc->next_fill_price = p; vnow += 3000; eng.tick("BTCUSDT", p);
    }
    check(eng.get_bots()[0].entries.size() == 2,
          "一路反弹回去不再补仓——V形只吃到一层，深层弹药原封不动");

    // 再砸一次到 80000：这次相对 83000 跌够了，且仍在 1h 下轨外 → 可以武装
    fc->next_fill_price = 80000.0; vnow += 3000; eng.tick("BTCUSDT", 80000.0);
    fc->next_fill_price = 81000.0; vnow += 3000; eng.tick("BTCUSDT", 81000.0);
    check(eng.get_bots()[0].entries.size() == 3,
          "第二次下跌+反弹才补到第3层——每层都要各自的一轮「跌够+止跌」");

    check(fc->market_orders == 3, "全程只发了 3 笔订单（首仓 + 2 次补仓）");
}

// ── 用例：账户级并发持仓上限 ─────────────────────────────────────────────────
// 全市场扫描场景的必需闸门。它与保证金上限管的是不同的事：保证金上限管
// "总共投出去多少钱"，并发上限管"同时压在几个品种上"。只有前者的话，大跌那天
// 几十个品种同时触发信号，钱会被最先触发的吃光，分散度完全失控。
static void test_max_open_positions() {
    std::printf("\n── 用例：账户级并发持仓上限 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));
    eng.set_max_open_positions(2);          // 最多同时持有 2 个品种

    const char* syms[] = { "AAAUSDT", "BBBUSDT", "CCCUSDT", "DDDUSDT" };
    for (const char* s : syms) {
        CcgConfig c = base_cfg();
        c.symbol = s;
        eng.add_bot(c);
    }

    fc->next_fill_price = 100.0;
    for (const char* s : syms) { vnow += 3000; eng.tick(s, 100.0); }

    int holding = 0;
    for (const auto& b : eng.get_bots()) if (b.total_qty > 0) ++holding;
    check(holding == 2, "4 个品种同时满足开仓条件，只开出 2 个（实际 " +
                        std::to_string(holding) + "）");
    check(eng.open_position_count() == 2, "open_position_count 报告 2");
    check(fc->market_orders == 2, "只发了 2 笔订单，没有超发");

    // 平掉一个，额度应当立刻释放给下一个品种
    for (const auto& b : eng.get_bots()) {
        if (b.total_qty > 0) { eng.close_bot(b.bot_id); break; }
    }
    check(eng.open_position_count() == 1, "手动平掉一个后并发数降到 1");

    for (const char* s : syms) { vnow += 3000; eng.tick(s, 100.0); }
    holding = 0;
    for (const auto& b : eng.get_bots()) if (b.total_qty > 0) ++holding;
    check(holding == 2, "空出的额度被后面排队的品种补上，仍不超过上限");

    // 0 = 不限
    CcgEngine eng2(fc, make_host(vnow));
    eng2.set_max_open_positions(0);
    for (const char* s : syms) {
        CcgConfig c = base_cfg(); c.symbol = s;
        eng2.add_bot(c);
    }
    for (const char* s : syms) { vnow += 3000; eng2.tick(s, 100.0); }
    int h2 = 0;
    for (const auto& b : eng2.get_bots()) if (b.total_qty > 0) ++h2;
    check(h2 == 4, "上限设 0 = 不限制，4 个全部开出");
}

// ── 用例：运行中外部平仓要被发现，且不能误伤正常止盈 ─────────────────────────
//
// 实际反馈：在手机 App 上手动平掉仓位后，程序界面仍显示着持仓，要关掉重开才
// 会发现——因为对账此前【只在连接成功时跑一次】。期间 bot 拿着一个不存在的
// 仓位继续算止盈止损、继续补仓。
//
// 周期对账的难点不在"发现"，而在【不误判】：正在平仓的 bot，订单已在交易所
// 生效、本地还没入账，此刻比对必然对不上——若当成"外部平仓"，一次完全正常的
// 止盈会被清空状态并停掉。所以这里三条一起测。
static void test_periodic_reconcile() {
    std::printf("\n── 用例：周期对账（外部平仓检测 + 防误判）──\n");
    using RM = CcgEngine::ReconcileMode;
    int64_t vnow = 1'700'000'000'000LL;
    auto fc = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    CcgConfig c = base_cfg();
    c.auto_restart  = true;
    c.cooldown_secs = 60;
    eng.add_bot(c);

    fc->next_fill_price = 100.0;
    eng.tick("BTCUSDT", 100.0);
    check(eng.get_bots()[0].total_qty > 0, "先建仓");

    // ① 刚成交就对账：持仓快照可能拍摄于成交【之前】，必须跳过不判
    eng.reconcile_positions({}, RM::Periodic);
    check(eng.get_bots()[0].total_qty > 0,
          "  刚成交时的空快照【不】清仓位（快照可能早于成交）");

    // ② 静置期过后，交易所确实已无该仓位 → 认定外部平仓
    vnow += 40'000;     // 越过 30 秒静置期
    eng.reconcile_positions({}, RM::Periodic);
    {
        auto b = eng.get_bots()[0];
        check(b.total_qty <= 0, "静置期后检测到外部平仓，本地仓位已清空");
        check(b.entries.empty(), "  加仓记录也清空了");
        check(b.state == CcgBot::State::Cooldown,
              "  auto_restart 开着 → 进冷却等下一轮，而不是停掉");
    }

    // ③ 冷却期满后能重新开仓——"检测到无持仓并重启策略"的完整闭环
    vnow += 61'000;
    fc->next_fill_price = 90.0;
    eng.tick("BTCUSDT", 90.0);
    {
        auto b = eng.get_bots()[0];
        check(b.state == CcgBot::State::Running, "冷却期满回到运行态");
        check(b.total_qty > 0, "  并按策略重新开了首仓");
    }

    // ④ 【核心防误判】正在平仓中（pending）的 bot 绝不能被判成外部平仓。
    //    构造：手工把 pending 置起来，模拟"平仓单已发出、本地还没入账"
    vnow += 40'000;
    eng.set_pending_for_test(eng.get_bots()[0].bot_id, true);
    eng.reconcile_positions({}, RM::Periodic);
    {
        auto b = eng.get_bots()[0];
        check(b.total_qty > 0,
              "  在途(pending)时的空快照【不】清仓位——否则正常止盈会被误判成外部平仓");
    }
    eng.set_pending_for_test(eng.get_bots()[0].bot_id, false);

    // ⑤ 关掉 auto_restart 时保持原有的保守行为：停止，等人工确认
    {
        auto fc2 = std::make_shared<FakeClient>();
        CcgEngine eng2(fc2, make_host(vnow));
        CcgConfig c2 = base_cfg();
        c2.auto_restart = false;
        eng2.add_bot(c2);
        fc2->next_fill_price = 100.0;
        eng2.tick("BTCUSDT", 100.0);
        vnow += 40'000;
        eng2.reconcile_positions({}, RM::Periodic);
        auto b = eng2.get_bots()[0];
        check(b.total_qty <= 0, "未开自动循环：同样清空仓位");
        check(b.state == CcgBot::State::Stopped, "  但停止该bot而不是重启（原有的保守行为）");
    }
}

// ── 用例：认领必须按成本反推层数，否则满仓之上还能再补 ───────────────────────
//
// 补仓闸门看的是 entries.size() >= max_entries。认领若把整个仓位塞进一笔 entry，
// 一个【已经满仓】的 bot 就会以为自己还在第 1 层，于是还能再补满剩下的层——
// 总名义可以到预算的两倍，直接打破"名义仓位 ≤ 权益 ⇒ 强平价 ≤ 0"这个不变式。
//
// 账户级总保证金上限确实也会挡，但它默认是 0（=不限），所以默认配置下
// 没有任何东西拦这件事。
static void test_adopt_reconstructs_levels() {
    std::printf("\n── 用例：对账认领按成本反推层数 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    CcgConfig c = base_cfg();       // 平推 4 层 / 每层 250U / 预算 1000U
    c.entry_mode = CcgConfig::EntryMode::Indicator;   // 不自己开首仓，只等认领
    eng.add_bot(c);

    // 交易所上是一笔【已经满仓】的仓位：10 × 100 = 1000U = 全部预算
    CcgEngine::ExchangePos full;
    full.symbol = "BTCUSDT"; full.direction = 1;
    full.qty = 10.0; full.entry_price = 100.0;
    eng.reconcile_positions({ full });

    {
        auto b = eng.get_bots()[0];
        check((int)b.entries.size() == c.max_entries,
              "认领后按成本反推为满层（实际 " + std::to_string(b.entries.size()) +
              "/" + std::to_string(c.max_entries) + "）");
        check(std::fabs(b.total_qty - 10.0) < 1e-9, "  总量与交易所完全一致");
        check(std::fabs(b.avg_price - 100.0) < 1e-9, "  均价与交易所完全一致");
        double sum = 0;
        for (const auto& e : b.entries) sum += e.qty;
        check(std::fabs(sum - 10.0) < 1e-9, "  各层数量之和精确等于总量（末层吃掉舍入残差）");
    }

    // 【核心】满仓之后，再怎么"跌够间隔+反弹确认"都不该继续补。
    // 每轮先跌破 10% 间隔、再反弹 1% 完成建仓确认——这正是 DCA 会真正触发的形态。
    // 修复前 entries.size()==1，闸门放行，会在 1000U 之上再补三层共 750U
    fc->next_fill_price = 80.0;
    const double seq[][2] = { {85, 87}, {70, 72}, {60, 62} };
    for (const auto& r : seq) {
        vnow += 3000; eng.tick("BTCUSDT", r[0]);
        vnow += 3000; eng.tick("BTCUSDT", r[1]);
    }
    {
        auto b = eng.get_bots()[0];
        check((int)b.entries.size() == c.max_entries,
              "  三轮「跌够+反弹」之后仍是满层，没有在满仓之上继续补（实际 " +
              std::to_string(b.entries.size()) + " 层）");
        check(std::fabs(b.total_qty - 10.0) < 1e-9,
              "  总量未增加（实际 " + std::to_string(b.total_qty) + "）");
    }

    // 反面：只认领了一层的量，层数也要算对，且后续补仓照常可用
    {
        auto fc2 = std::make_shared<FakeClient>();
        CcgEngine eng2(fc2, make_host(vnow));
        CcgConfig c2 = base_cfg();
        c2.entry_mode = CcgConfig::EntryMode::Indicator;
        eng2.add_bot(c2);
        CcgEngine::ExchangePos one;
        one.symbol = "BTCUSDT"; one.direction = 1;
        one.qty = 2.0; one.entry_price = 100.0;      // 200U < 第一层的 250U
        eng2.reconcile_positions({ one });
        auto b = eng2.get_bots()[0];
        check((int)b.entries.size() == 1,
              "  只认领一层的量时反推为 1 层（实际 " +
              std::to_string(b.entries.size()) + "）");
    }
}

// ── 用例：对账认领的仓位不能被冷却逻辑清掉 ───────────────────────────────────
// 这是压力测试的状态机不变量（"冷却态却仍持有仓位"）抓出来的一条完整 bug 链：
//
//   ① bot 止盈完成 → 进入 Cooldown（仓位已清零）
//   ② 对账发现交易所有个孤儿仓位（典型是崩溃期间成交、还没落盘的那笔）
//   ③ adopter 的选择【只看 !pending，完全不看 state】→ 选中这个 Cooldown 的 bot
//   ④ 认领代码写入 total_qty/avg_price/entries，但【不改 state】
//   ⑤ 冷却结束 → tick 里执行 "entries.clear(); total_qty = 0"
//      → 刚认领回来的仓位被【静默清零】
//   ⑥ 那笔仓位变成交易所有、本地无人管的真孤儿，没有任何止盈止损保护
//
// 最恶劣的是它会先打一条"已认领孤儿仓位"的日志让人放心，几分钟后再悄悄清掉。
static void test_adopted_position_survives_cooldown() {
    std::printf("\n── 用例：对账认领的仓位不能被冷却清掉 ──\n");
    int64_t vnow = 1'700'000'000'000LL;
    auto fc  = std::make_shared<FakeClient>();
    CcgEngine eng(fc, make_host(vnow));

    CcgConfig c = base_cfg();
    c.auto_restart  = true;
    c.cooldown_secs = 60;
    auto id = eng.add_bot(c);

    // 建仓 → 止盈 → 进入冷却
    fc->next_fill_price = 100.0;
    eng.tick("BTCUSDT", 100.0);
    // tp_pct=5 → 止盈线 105；trail_tp=2 → 自最高点 106 回落到 103.88 才触发
    fc->next_fill_price = 106.0;
    vnow += 3000; eng.tick("BTCUSDT", 106.0);   // 激活止盈追踪
    fc->next_fill_price = 102.0;
    vnow += 3000; eng.tick("BTCUSDT", 102.0);   // 跌破 103.88 → 追踪平仓
    {
        auto b = eng.get_bots()[0];
        check(b.state == CcgBot::State::Cooldown, "止盈后进入冷却态");
        check(b.total_qty <= 0, "  冷却时仓位已清零");
    }

    // 对账：交易所冒出一个本地没跟踪的仓位（崩溃期间成交的那种）
    CcgEngine::ExchangePos orphan;
    orphan.symbol = "BTCUSDT"; orphan.direction = 1;
    orphan.qty = 0.5; orphan.entry_price = 98.0;
    eng.reconcile_positions({ orphan });

    {
        auto b = eng.get_bots()[0];
        check(b.total_qty > 0.49, "对账认领了孤儿仓位");
        check(b.state != CcgBot::State::Cooldown,
              "  认领之后必须退出冷却态——冷却与持仓是互斥的");
    }

    // 推进到冷却本该结束的时刻：认领的仓位必须【原样】还在。
    // 注意要比对【具体数量】而不是 ">0"——修复前这里也是 >0，但那是"认领的 0.5
    // 被清零后又开了一笔新首仓"的结果。交易所实际持有 0.5+新仓，本地只记新仓，
    // 也就是【双倍仓位】：比单纯丢记录更糟
    const int orders_before = fc->market_orders;
    vnow += 120'000;
    eng.tick("BTCUSDT", 97.0);
    {
        auto b = eng.get_bots()[0];
        check(std::fabs(b.total_qty - 0.5) < 1e-6,
              "冷却期满后仓位仍是认领的那 0.5（实际 " + std::to_string(b.total_qty) + "）");
        check(!b.entries.empty(), "  加仓记录也还在，没被清空");
        check(b.avg_price > 90.0, "  均价保留，止盈线才有基准");
        check(fc->market_orders == orders_before,
              "  没有因为误判空仓而重新开首仓（那会变成双倍仓位）");
    }
}

int main() {
    test_dca_margin_cap();
    test_adopted_position_survives_cooldown();
    test_v_crash_fills_one_layer();
    test_max_open_positions();
    test_disaster_stop_lifecycle();
    test_disabled_by_default();
    test_place_failure_is_loud();
    test_mtf_alloc();
    test_adopt_reconstructs_levels();
    test_periodic_reconcile();
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
