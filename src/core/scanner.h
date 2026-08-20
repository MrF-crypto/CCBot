#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

// 全市场超卖扫描器。
//
// 职责边界：**只产出候选，不下单、不碰引擎**。这样它可以被单独测试，也可以在
// 影子模式下先跑一段看看它选出来的是什么货色，再决定要不要接上真金白银。
//
// 为什么要两阶段：币安【没有批量 K 线接口】。全市场约 500 个 USDT 永续，逐个拉
// K 线要串行两分半、权重 500。而 /fapi/v1/ticker/24hr 不带参数时一个请求返回
// 全部品种（权重 40），足以按成交额和跌幅把 500 个筛到几十个。
//
//   粗筛  1 个请求  权重 40   500 → 60 个
//   精算 60 个请求  权重 60   60  → 8 个
//   合计            权重 100  约 18 秒
//
// 直接对 500 个拉 K 线是权重 500、150 秒。差一个数量级，而且那还是每轮的成本。
namespace ccbot {

// 一个候选品种及其超卖证据。字段全部保留是为了可复盘——
// 事后要能回答"当初为什么选中它"，只给一个分数是不够的
struct ScanCandidate {
    std::string symbol;
    double price      = 0;
    double pct_b      = 0;    // 布林 %B：0=下轨 1=上轨，跌破下轨时为负
    double rsi        = 50;
    double change_24h = 0;    // 24h 涨跌幅 %
    double quote_vol  = 0;    // 24h 成交额（USDT）
    double score      = 0;    // 综合超卖分，越大越优先

    std::string to_text() const;
};

struct ScannerConfig {
    // ── 粗筛：只花一个批量请求 ────────────────────────────────────────────
    // 成交额下限是【最重要的一道过滤】。快进快出赚的是 1~2% 的差价，而小币的
    // 市价单双边滑点轻松吃掉 0.5%——成交额不够的品种，策略在数学上就不成立。
    double min_quote_vol_24h = 50'000'000.0;
    double max_change_24h    = 0.0;    // 只看跌了的
    int    coarse_top_n      = 60;     // 粗筛后进入精算的个数
    std::string quote_suffix = "USDT"; // 只要 USDT 本位

    // ── 精算：每个候选一次 K 线请求 ───────────────────────────────────────
    std::string kline_interval = "1h";
    int    boll_period = 20;
    double boll_mult   = 2.0;
    int    rsi_period  = 14;

    // ── 入选门槛 ─────────────────────────────────────────────────────────
    // %B ≤ 0 表示价格已跌破布林下轨（统计意义上的超卖位）。这是主判据。
    double max_pct_b = 0.0;
    double max_rsi   = 35.0;
    int    final_top_n = 8;

    // 永不参与的品种。新上市、要下架、或你手工拉黑的
    std::set<std::string> blacklist;
};

// 扫描器需要的数据源。用 std::function 注入而不是继承接口——与 EngineHost
// 同一套做法，测试时塞两个 lambda 就行，不需要网络也不需要造假类
struct ScanFeed {
    struct Ticker {
        std::string symbol;
        double last_price = 0, change_pct = 0, quote_volume = 0;
    };
    struct Indicators {
        bool   ok = false;
        double price = 0, boll_lb = 0, boll_ub = 0, rsi = 50;
    };
    std::function<std::vector<Ticker>()>                     all_tickers;
    std::function<Indicators(const std::string& symbol)>     indicators;
    // 可选：精算每拉一个就回调一次，用来打进度日志。60 个串行要十几秒，
    // 没有进度提示会让人以为卡死了
    std::function<void(int done, int total)>                 on_progress;
};

class MarketScanner {
public:
    explicit MarketScanner(ScannerConfig cfg) : cfg_(std::move(cfg)) {}

    // 扫一轮。exclude 里的品种直接跳过（通常是"已经有仓位的"——
    // 扫描器不知道账户状态，由调用方告知）。
    // 返回按超卖分降序排列、已截断到 final_top_n 的候选。
    std::vector<ScanCandidate> scan(const ScanFeed& feed,
                                    const std::set<std::string>& exclude = {}) const;

    // 上一轮的统计，用于日志与排查："扫了多少、筛掉多少、为什么一个都没选中"
    struct Stats {
        int total_symbols   = 0;   // 交易所返回的全部品种
        int after_filters   = 0;   // 通过成交额/后缀/黑名单/涨跌幅过滤
        int probed          = 0;   // 实际拉了 K 线的
        int qualified       = 0;   // 满足 %B 与 RSI 门槛的
        std::string to_text() const;
    };
    const Stats& last_stats() const { return stats_; }

    const ScannerConfig& config() const { return cfg_; }

private:
    ScannerConfig  cfg_;
    mutable Stats  stats_;
};

} // namespace ccbot
