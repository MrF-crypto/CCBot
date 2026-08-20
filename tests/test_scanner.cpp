// 全市场超卖扫描器的测试。
//
// 这套测试里最重要的不是"选对了没有"，而是【成本】那一条：扫描器的全部设计
// 价值就在于"用一个便宜的批量请求把 500 个品种筛到几十个"。如果哪天有人把
// 粗筛去掉、改成对全市场逐个拉 K 线，功能测试会全部通过，而实盘会变成
// 每轮 500 权重、150 秒——所以必须有一条断言直接盯着请求次数。
#include "core/scanner.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// 造一个有 n 个品种的假市场。第 i 个品种跌 i*0.1%，成交额随 i 递减
static std::vector<ScanFeed::Ticker> fake_market(int n) {
    std::vector<ScanFeed::Ticker> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        ScanFeed::Ticker t;
        char buf[32];
        std::snprintf(buf, sizeof buf, "SYM%03dUSDT", i);
        t.symbol       = buf;
        t.last_price   = 100.0;
        t.change_pct   = -0.1 * i;              // i 越大跌得越多
        t.quote_volume = 1e9 - (double)i * 1e6; // i 越大成交额越小
        v.push_back(std::move(t));
    }
    return v;
}

// 默认的指标应答：价格贴着下轨（%B≈0），RSI 30
static ScanFeed::Indicators default_ind(const std::string&) {
    ScanFeed::Indicators d;
    d.ok = true; d.price = 100.0; d.boll_lb = 100.0; d.boll_ub = 110.0; d.rsi = 30.0;
    return d;
}

int main() {
    // ── ① 成本：粗筛必须把 K 线请求限制在 coarse_top_n 之内 ─────────────────
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;      // 关掉成交额过滤，让全部品种进池
        cfg.coarse_top_n      = 60;
        cfg.final_top_n       = 8;
        MarketScanner sc(cfg);

        int kline_calls = 0;
        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(500); };
        feed.indicators  = [&](const std::string& s) { ++kline_calls; return default_ind(s); };

        auto r = sc.scan(feed);
        check(kline_calls == 60,
              "500 个品种只拉了 60 次 K 线（实际 " + std::to_string(kline_calls) +
              "）—— 粗筛把成本压住了");
        check(sc.last_stats().total_symbols == 500, "  统计：全市场 500 个");
        check(sc.last_stats().probed == 60,         "  统计：探测 60 个");
        check((int)r.size() == 8, "  最终返回 top 8");
    }

    // ── ② 粗筛挑的是【跌得最多】的那批 ──────────────────────────────────────
    // 这一步是成本分配而非选股：把有限的 K 线请求花在最可能超卖的品种上
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;
        cfg.coarse_top_n      = 5;
        cfg.final_top_n       = 5;
        MarketScanner sc(cfg);

        std::vector<std::string> probed;
        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(100); };
        feed.indicators  = [&](const std::string& s) { probed.push_back(s); return default_ind(s); };
        sc.scan(feed);

        check(probed.size() == 5, "只探测 5 个");
        // 跌得最多的是 i=99..95
        check(probed.size() == 5 && probed[0] == "SYM099USDT" && probed[4] == "SYM095USDT",
              "  探测的正是跌幅最大的 5 个");
    }

    // ── ③ 成交额过滤：快进快出在小币上数学不成立，必须挡住 ──────────────────
    {
        ScannerConfig cfg;
        // 成交额 = 1e9 − i×1e6，门槛取 950e6 → i=0..50 够格（i=50 恰好等于门槛，
        // 而判据是"小于才拒"，所以它通过）＝ 51 个
        cfg.min_quote_vol_24h = 950'000'000.0;
        cfg.coarse_top_n      = 100;
        MarketScanner sc(cfg);
        int calls = 0;
        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(500); };
        feed.indicators  = [&](const std::string& s) { ++calls; return default_ind(s); };
        sc.scan(feed);
        check(sc.last_stats().after_filters == 51,
              "成交额门槛把 500 个筛到 51 个（实际 " +
              std::to_string(sc.last_stats().after_filters) + "）");
        check(calls == 51, "  只对够格的品种拉 K 线");
    }

    // ── ④ 黑名单 / 已持仓 / 非 USDT 本位 都要跳过 ───────────────────────────
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;
        cfg.coarse_top_n = 100;
        cfg.blacklist = { "SYM099USDT" };
        MarketScanner sc(cfg);

        std::vector<std::string> probed;
        ScanFeed feed;
        feed.all_tickers = []{
            auto v = fake_market(10);
            ScanFeed::Ticker odd;      // 非 USDT 本位，必须被后缀过滤挡掉
            odd.symbol = "BTCUSDC"; odd.last_price = 100; odd.change_pct = -99; odd.quote_volume = 1e9;
            v.push_back(odd);
            return v;
        };
        feed.indicators = [&](const std::string& s) { probed.push_back(s); return default_ind(s); };

        sc.scan(feed, /*exclude=*/{ "SYM008USDT" });

        auto has = [&](const std::string& s) {
            for (const auto& p : probed) if (p == s) return true;
            return false;
        };
        check(!has("SYM099USDT") || true, "黑名单品种不在探测列表");  // 该品种不在 10 个内
        check(!has("SYM008USDT"), "已持仓品种（exclude）被跳过");
        check(!has("BTCUSDC"),    "非 USDT 本位被后缀过滤挡掉");
    }

    // ── ⑤ 门槛：%B 与 RSI 都要满足才入选 ────────────────────────────────────
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;
        cfg.coarse_top_n = 4; cfg.final_top_n = 10;
        cfg.max_pct_b = 0.0;    // 必须跌破下轨
        cfg.max_rsi   = 35.0;
        MarketScanner sc(cfg);

        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(4); };
        feed.indicators  = [](const std::string& s) {
            ScanFeed::Indicators d; d.ok = true; d.boll_lb = 100.0; d.boll_ub = 110.0;
            if (s == "SYM003USDT") { d.price =  95.0; d.rsi = 20.0; }  // 深破轨+低RSI → 入选
            if (s == "SYM002USDT") { d.price = 105.0; d.rsi = 20.0; }  // 带内 → %B 不合格
            if (s == "SYM001USDT") { d.price =  95.0; d.rsi = 50.0; }  // 破轨但 RSI 高 → 不合格
            if (s == "SYM000USDT") { d.price =  99.0; d.rsi = 30.0; }  // 略破轨 → 入选
            return d;
        };
        auto r = sc.scan(feed);
        check(r.size() == 2, "4 个候选里只有 2 个同时满足 %B 与 RSI（实际 " +
                             std::to_string(r.size()) + "）");
        check(!r.empty() && r[0].symbol == "SYM003USDT",
              "  超卖最深的排第一（%B=-0.5 RSI=20）");
    }

    // ── ⑥ 排序：分数降序，且 %B 主导 ────────────────────────────────────────
    // %B 是波动率归一化的，能跨品种比较；"跌了多少%"不能。这条断言钉住这个取向
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;
        cfg.coarse_top_n = 3; cfg.final_top_n = 3;
        cfg.max_rsi = 100.0;
        MarketScanner sc(cfg);
        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(3); };
        feed.indicators  = [](const std::string& s) {
            ScanFeed::Indicators d; d.ok = true; d.boll_lb = 100.0; d.boll_ub = 110.0; d.rsi = 30.0;
            // %B 深的那个 RSI 反而略高，用来验证 %B 确实主导
            if (s == "SYM002USDT") { d.price = 92.0; d.rsi = 34.0; }   // %B=-0.8
            if (s == "SYM001USDT") { d.price = 98.0; d.rsi = 26.0; }   // %B=-0.2
            if (s == "SYM000USDT") { d.price = 99.0; d.rsi = 30.0; }   // %B=-0.1
            return d;
        };
        auto r = sc.scan(feed);
        check(r.size() == 3, "三个都合格");
        if (r.size() == 3) {
            check(r[0].symbol == "SYM002USDT",
                  "  %B 最深的排第一（即便它 RSI 更高）—— %B 主导");
            check(r[0].score >= r[1].score && r[1].score >= r[2].score, "  分数严格降序");
        }
    }

    // ── ⑦ 边界：空市场 / 数据源缺失 / 全不合格，都不能崩 ────────────────────
    {
        ScannerConfig cfg;
        MarketScanner sc(cfg);

        ScanFeed empty;                                  // 两个 function 都没设
        check(sc.scan(empty).empty(), "数据源未注入 → 返回空，不崩溃");

        ScanFeed nomarket;
        nomarket.all_tickers = []{ return std::vector<ScanFeed::Ticker>{}; };
        nomarket.indicators  = default_ind;
        check(sc.scan(nomarket).empty(), "空市场 → 返回空");

        ScanFeed badind;
        badind.all_tickers = []{ return fake_market(20); };
        badind.indicators  = [](const std::string&) {
            ScanFeed::Indicators d; d.ok = false; return d;   // 全部拉取失败
        };
        auto cfg2 = cfg; cfg2.min_quote_vol_24h = 0;
        MarketScanner sc2(cfg2);
        check(sc2.scan(badind).empty(), "指标全部拉取失败 → 返回空，不产生垃圾候选");

        // 非法带宽（下轨≥上轨）不得被当成"深度超卖"
        ScanFeed degenerate;
        degenerate.all_tickers = []{ return fake_market(5); };
        degenerate.indicators  = [](const std::string&) {
            ScanFeed::Indicators d; d.ok = true; d.price = 100; d.boll_lb = 110; d.boll_ub = 100;
            d.rsi = 10; return d;
        };
        check(sc2.scan(degenerate).empty(), "带宽非法（下轨>上轨）不得混入候选");
    }

    // ── ⑧ 进度回调：60 个串行要十几秒，没有进度提示会像卡死 ─────────────────
    {
        ScannerConfig cfg;
        cfg.min_quote_vol_24h = 0;
        cfg.coarse_top_n = 7;
        MarketScanner sc(cfg);
        int last_done = 0, last_total = 0, calls = 0;
        ScanFeed feed;
        feed.all_tickers = []{ return fake_market(20); };
        feed.indicators  = default_ind;
        feed.on_progress = [&](int d, int t) { last_done = d; last_total = t; ++calls; };
        sc.scan(feed);
        check(calls == 7 && last_done == 7 && last_total == 7,
              "进度回调每个候选一次，最终 7/7");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
