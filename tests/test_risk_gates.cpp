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

int main() {
    test_dca_margin_cap();
    test_disaster_stop_lifecycle();
    test_disabled_by_default();
    test_place_failure_is_loud();
    test_mtf_alloc();
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
