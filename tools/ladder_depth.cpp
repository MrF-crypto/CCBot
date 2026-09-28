// 梯子深度分析 —— 回答"子弹打光时价格跌了多少、还剩多少保证金没花"。
//
// 存在的理由：满层能扛多深这个问题，回测答不了。回测的风险指标是【浮动回撤】，
// 而对一个"套住就长持不止损"的策略来说，浮亏本身不致命——致命的是强平。
// 决定强平距离的是【已投入保证金 vs 剩余可用保证金】这条曲线，不是浮亏数字。
//
// 本工具不跑行情，只做确定性推演：给定固定补仓间隔，算出每一档的累计跌幅、
// 累计投入、以及那一刻的持仓均价。资金分配用的是引擎里【真实】的
// CcgEngine::entry_usdt，不是复刻。
//
// 用法:
//   ladder_depth [--interval 6] [--layers 10] [--budget 3000] [--lev 3]
//                [--curve linear|flat|...] [--price 100] [--mmr 0.4] [--wallet N]
//
// v4.6.0 起补仓间距是单一固定值（动态W与多周期梯子已移除），所以只剩一张表。
#include "core/ccg_engine.h"
#include "version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ccbot;

static double arg_num(int c, char** v, const char* k, double d) {
    for (int i = 1; i + 1 < c; ++i) if (!std::strcmp(v[i], k)) return std::atof(v[i + 1]);
    return d;
}
static std::string arg_str(int c, char** v, const char* k, const char* d) {
    for (int i = 1; i + 1 < c; ++i) if (!std::strcmp(v[i], k)) return v[i + 1];
    return d;
}

struct Row {
    int    slot;
    int    tier;        // -1 = 基线（不分档）
    double gap_pct;     // 相对上一笔成交价
    double drop_pct;    // 自首仓的累计跌幅
    double price;       // 该层成交价（首仓价 = 100）
    double deployed;    // 累计投入名义价值
    double avg_price;   // 该层成交后的持仓均价
    double unreal_pct;  // 该均价下、当前价即成交价时的浮盈亏%
    double liq_price;   // 该层成交后的强平价（<=0 表示不可能被强平）
    double liq_drop;    // 强平价距首仓价的跌幅%
};

// ── 全仓强平价 ────────────────────────────────────────────────────────────────
// 强平条件：账户权益 = 维持保证金
//     wallet + qty×(P − 均价) = qty × P × MMR
//  ⇒  P_liq = (已投入名义 − wallet) / (qty × (1 − MMR))
//
// 注意其中的关键含义：**已投入名义价值 ≤ wallet 时 P_liq ≤ 0，即根本不可能被强平**
// ——那部分仓位是被现金全额抵押的。这正是"子弹还没打出去"的价值所在，而且它是
// 可以精确算出来的，不是感觉。
//
// wallet 默认取 预算÷杠杆（＝只按计划投入这么多保证金）。若账户里放了更多闲钱，
// 用 --wallet 覆盖——多放的每一分都直接把强平价往下推。
static void fill_liq(std::vector<Row>& rows, double wallet, double mmr, double p0) {
    for (auto& r : rows) {
        double qty = r.deployed / r.avg_price;      // 累计持仓量
        double num = r.deployed - wallet;           // 已投入名义 − 现金
        r.liq_price = (num <= 0) ? 0.0 : num / (qty * (1.0 - mmr));
        r.liq_drop  = (r.liq_price <= 0) ? 100.0 : (p0 - r.liq_price) / p0 * 100.0;
    }
}

static double g_p0 = 100.0;   // 首仓价（--price，纯为可读性，不影响任何比例）

static std::vector<Row> walk(const CcgConfig& cfg, const std::vector<double>& gaps,
                             const std::vector<int>& tiers) {
    auto sizes = CcgEngine::entry_usdt(cfg);
    std::vector<Row> out;
    double price = g_p0, qty = 0, cost = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        double gap = (i == 0) ? 0.0 : gaps[i];
        price *= (1.0 - gap / 100.0);
        double q = sizes[i] / price;
        qty  += q;
        cost += sizes[i];
        Row r;
        r.slot      = (int)i;
        r.tier      = tiers.empty() ? -1 : tiers[i];
        r.gap_pct   = gap;
        r.drop_pct  = (g_p0 - price) / g_p0 * 100.0;
        r.price     = price;
        r.deployed  = cost;
        r.avg_price = cost / qty;
        r.unreal_pct = (price - r.avg_price) / r.avg_price * 100.0;
        out.push_back(r);
    }
    return out;
}

static void print_table(const char* title, const std::vector<Row>& rows,
                        double budget, int lev, double wallet) {
    static const char* TN[4] = { "1h", "4h", "12h", "1d" };
    std::printf("\n%s\n", title);
    std::printf("  层  档   本档间距  累计跌幅    成交价    累计投入 占预算     均价    该点浮亏     强平价  强平在跌\n");
    std::printf("  ── ──── ───────── ───────── ────────── ──────── ────── ────────── ───────── ────────── ─────────\n");
    for (const auto& r : rows) {
        char liq[32], liqd[24];
        if (r.liq_price <= 0) { std::snprintf(liq, sizeof liq, "%9s", "不会强平");
                                std::snprintf(liqd, sizeof liqd, "%8s", "—"); }
        else { std::snprintf(liq, sizeof liq, "%10.2f", r.liq_price);
               std::snprintf(liqd, sizeof liqd, "%8.1f%%", r.liq_drop); }
        std::printf("  %2d  %-4s %7.2f%% %8.2f%% %10.2f %7.0fU %5.0f%% %10.2f %8.2f%% %s %s\n",
                    r.slot + 1,
                    r.tier < 0 ? "—" : TN[r.tier],
                    r.gap_pct, r.drop_pct, r.price,
                    r.deployed, r.deployed / budget * 100.0,
                    r.avg_price, r.unreal_pct, liq, liqd);
    }
    const auto& last = rows.back();
    std::printf("  满层：跌 %.2f%% 打光弹药 · 均价 %.2f · 浮亏 %.2f%% · 强平在跌 %.1f%%"
                "（钱包 %.0fU，预算 %.0fU ÷ %dx）\n",
                last.drop_pct, last.avg_price, last.unreal_pct, last.liq_drop,
                wallet, budget, lev);
}

// 给定"当前跌幅"，反查两种梯子各自已投入多少、还剩多少没花
static void compare_at(double drop, const std::vector<Row>& rows,
                       double budget, int lev) {
    double dep = 0, avg = 0; int n = 0;
    for (const auto& r : rows)
        if (r.drop_pct <= drop + 1e-9) { dep = r.deployed; avg = r.avg_price; n = r.slot + 1; }
    const double px = g_p0 * (1.0 - drop / 100.0);
    std::printf("  跌 %5.1f%%   %2d层  投入%6.0fU(%3.0f%%)  均价%8.3f  浮亏%7.2f%%  "
                "剩余保证金%6.0fU\n",
                drop, n, dep, dep / budget * 100.0, avg,
                n ? (px - avg) / avg * 100.0 : 0.0, (budget - dep) / lev);
}

int main(int argc, char** argv) {
    CcgConfig cfg;
    cfg.symbol      = "X";
    cfg.max_entries = (int)arg_num(argc, argv, "--layers", 10);
    cfg.budget_usdt = arg_num(argc, argv, "--budget", 3000);
    cfg.leverage    = (int)arg_num(argc, argv, "--lev", 3);
    cfg.interval_pct = arg_num(argc, argv, "--interval", 6.0);

    std::string curve = arg_str(argc, argv, "--curve", "linear");
    if (curve == "flat")      cfg.strat_type = CcgConfig::StratType::Flat;
    else if (curve == "mart") cfg.strat_type = CcgConfig::StratType::Martingale;
    else                       cfg.strat_type = CcgConfig::StratType::Linear;

    g_p0 = arg_num(argc, argv, "--price", 100.0);
    const int n = cfg.max_entries;

    // 打版本：这份表会被拿去做资金规划，必须能追溯是哪一版算的
    std::printf("═══ 梯子深度分析 · CCBOT %s ═══\n", ccbot::kVersion);
    std::printf("曲线=%s  层数=%d  预算=%.0fU  杠杆=%dx  补仓间隔=%.2f%%\n",
                CcgEngine::strat_name(cfg.strat_type).c_str(), n, cfg.budget_usdt,
                cfg.leverage, cfg.interval_pct);

    const double mmr    = arg_num(argc, argv, "--mmr", 0.4) / 100.0;
    const double wallet = arg_num(argc, argv, "--wallet", cfg.budget_usdt / cfg.leverage);
    std::printf("维持保证金率 MMR=%.2f%%（BTCUSDT 小额档位；随名义价值分档变化，用 --mmr 覆盖）\n",
                mmr * 100.0);
    std::printf("钱包余额 %.0fU（默认=预算÷杠杆，即只按计划投入。多放闲钱用 --wallet）\n", wallet);

    std::vector<double> gaps(n, cfg.interval_pct);
    auto rows = walk(cfg, gaps, {});
    fill_liq(rows, wallet, mmr, g_p0);
    print_table("【梯子】每层间距相同（相对上一笔成交价）", rows,
                cfg.budget_usdt, cfg.leverage, wallet);

    std::printf("\n【同一跌幅下的状态】—— 剩余保证金 = 还没花出去的子弹 = 强平缓冲\n");
    for (double d : {3.0, 6.0, 10.0, 15.0, 20.0, 30.0, 40.0})
        compare_at(d, rows, cfg.budget_usdt, cfg.leverage);

    std::printf("\n注：浮亏按「当前价 = 该层成交价」计算。剩余保证金是尚未投入的部分，\n");
    std::printf("    它同时是抗强平的缓冲——已投入的钱既降均价也吃保证金，未投入的钱只做缓冲。\n");
    return 0;
}
