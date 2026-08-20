#include "core/scanner.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace ccbot {

namespace {

// 综合超卖分。%B 主导、RSI 微调，理由：
//
// %B 是【波动率归一化】的——同样跌 5%，在低波动品种上是极端事件，在高波动
// 品种上很平常。%B 把这个差异消掉了，所以它天生可跨品种比较，而"跌了多少%"
// 不能。全市场扫描的本质就是跨品种排序，这一条是决定性的。
//
// RSI 只做次级修正（除以 100 压到 %B 的量级之下）：它衡量的是动能而非位置，
// 两个 %B 相同的品种里，RSI 更低的那个跌势更持续——但这不足以盖过位置本身。
double oversold_score(double pct_b, double rsi) {
    return (-pct_b) + (50.0 - rsi) / 100.0;
}

std::string fmt(double v, int dp) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(dp) << v;
    return o.str();
}

} // namespace

std::string ScanCandidate::to_text() const {
    return symbol + " %B=" + fmt(pct_b, 3) + " RSI=" + fmt(rsi, 1) +
           " 24h=" + fmt(change_24h, 1) + "%" +
           " 额=" + fmt(quote_vol / 1e6, 0) + "M" +
           " 分=" + fmt(score, 3);
}

std::string MarketScanner::Stats::to_text() const {
    return "全市场 " + std::to_string(total_symbols) +
           " → 过滤后 " + std::to_string(after_filters) +
           " → 探测 " + std::to_string(probed) +
           " → 合格 " + std::to_string(qualified);
}

std::vector<ScanCandidate> MarketScanner::scan(const ScanFeed& feed,
                                               const std::set<std::string>& exclude) const {
    stats_ = Stats{};
    std::vector<ScanCandidate> out;
    if (!feed.all_tickers || !feed.indicators) return out;

    // ── 第一阶段：粗筛（1 个请求）────────────────────────────────────────
    auto tickers = feed.all_tickers();
    stats_.total_symbols = (int)tickers.size();

    std::vector<ScanFeed::Ticker> pool;
    pool.reserve(tickers.size());
    for (const auto& t : tickers) {
        if (t.last_price <= 0) continue;
        // 只要 USDT 本位：币安上还有 USDC/BTC 本位的永续，它们的报价单位不同，
        // 混进来会让成交额过滤和仓位计算全部失真
        const auto& q = cfg_.quote_suffix;
        if (!q.empty()) {
            if (t.symbol.size() <= q.size()) continue;
            if (t.symbol.compare(t.symbol.size() - q.size(), q.size(), q) != 0) continue;
        }
        if (cfg_.blacklist.count(t.symbol))              continue;
        if (exclude.count(t.symbol))                     continue;
        if (t.quote_volume < cfg_.min_quote_vol_24h)     continue;
        if (t.change_pct   > cfg_.max_change_24h)        continue;
        pool.push_back(t);
    }
    stats_.after_filters = (int)pool.size();
    if (pool.empty()) return out;

    // 按 24h 跌幅排序取前 N。这一步是【成本控制】而不是选股：真正的判据是
    // 下一阶段的 %B，这里只是决定"把有限的 K 线请求花在谁身上"。
    // 用跌幅当代理指标是因为它在粗筛阶段免费——虽然它没有波动率归一化，
    // 但作为"谁更可能超卖"的排序器已经够用
    std::sort(pool.begin(), pool.end(),
              [](const ScanFeed::Ticker& a, const ScanFeed::Ticker& b) {
                  return a.change_pct < b.change_pct;
              });
    if ((int)pool.size() > cfg_.coarse_top_n)
        pool.resize((size_t)std::max(0, cfg_.coarse_top_n));

    // ── 第二阶段：精算（每个候选一次 K 线请求）──────────────────────────
    const int total = (int)pool.size();
    int done = 0;
    for (const auto& t : pool) {
        auto ind = feed.indicators(t.symbol);
        ++done;
        ++stats_.probed;
        if (feed.on_progress) feed.on_progress(done, total);
        if (!ind.ok) continue;

        // 带宽合法性【自己判断】，不能借 decision::pct_b 的返回值来判：
        // 它用 -1 当"非法输入"的哨兵，而 -1 同时是一个完全合法的 %B
        // （价格恰在下轨下方一个带宽处）。靠哨兵值过滤会把【最深度超卖】的品种
        // 挡在门外——而那正是扫描器要找的东西。第一版就是这么写错的，
        // 测试里 %B=-0.8 的品种被静默丢掉了。
        if (ind.price <= 0 || ind.boll_lb <= 0 || ind.boll_ub <= ind.boll_lb) continue;
        const double pb = (ind.price - ind.boll_lb) / (ind.boll_ub - ind.boll_lb);

        if (pb > cfg_.max_pct_b)   continue;
        if (ind.rsi > cfg_.max_rsi) continue;

        ScanCandidate c;
        c.symbol     = t.symbol;
        c.price      = ind.price > 0 ? ind.price : t.last_price;
        c.pct_b      = pb;
        c.rsi        = ind.rsi;
        c.change_24h = t.change_pct;
        c.quote_vol  = t.quote_volume;
        c.score      = oversold_score(pb, ind.rsi);
        out.push_back(std::move(c));
    }
    stats_.qualified = (int)out.size();

    std::sort(out.begin(), out.end(),
              [](const ScanCandidate& a, const ScanCandidate& b) { return a.score > b.score; });
    if ((int)out.size() > cfg_.final_top_n)
        out.resize((size_t)std::max(0, cfg_.final_top_n));
    return out;
}

} // namespace ccbot
