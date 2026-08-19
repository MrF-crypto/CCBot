// 回测强平模型的测试。
//
// 存在的理由：在有强平模型之前，回测的风险指标是【浮动回撤】——而对一个
// "套住就长持不止损"的策略来说，浮亏本身不致命，致命的是强平。回测把这条路径
// 挖空了，导致"扛过去了"的深度回撤在现实中可能早已爆仓出局。
//
// 这套测试要钉住两件事：
//   ① 该爆的时候必须爆，且爆完立即终止回放（账户没了，后面的行情与它无关）
//   ② 【不该爆的时候一个数字都不能动】—— 这是能安心默认开启的前提，
//      也是保证历史回测结论仍然可比的前提
//
// 用合成行情而不是真实K线：强平是个确定性的会计恒等式，用编造的极端行情
// 反而能把边界卡得更准，也不依赖本机有没有数据文件。
#include "backtest/bt_portfolio.h"
#include "backtest/bt_multisim.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ccbot;
using namespace ccbot::bt;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// ── 合成行情 ─────────────────────────────────────────────────────────────────
// 每根 1 分钟。价格按给定的每根跌幅（%）线性推进，spread 固定 2bp
static Series make_series(const std::string& sym, double p0, int bars,
                          double per_bar_pct) {
    Series s;
    s.symbol = sym;
    s.bars.reserve(bars);
    double px = p0;
    int64_t t = 1'600'000'000'000LL;
    for (int i = 0; i < bars; ++i) {
        Bar b;
        b.ts_ms    = t + (int64_t)i * 60'000;
        b.open     = px;
        px        *= (1.0 + per_bar_pct / 100.0);
        b.close    = px;
        b.high     = std::max(b.open, b.close);
        b.low      = std::min(b.open, b.close);
        b.volume   = 10.0;
        b.quote_vol= 10.0 * px;
        b.spread   = 0.0002;
        s.bars.push_back(b);
    }
    return s;
}

static PortfolioOptions make_opt(double equity, double budget, int lev) {
    PortfolioOptions o;
    o.symbols          = {"XUSDT"};
    o.initial_equity   = equity;
    o.per_symbol_budget = budget;
    o.max_total_margin = budget / lev;   // 允许把预算全部投出去
    o.max_positions    = 4;
    auto& c = o.base_cfg;
    c.direction        = CcgConfig::Direction::Long;
    c.strat_type       = CcgConfig::StratType::Flat;
    c.leverage         = lev;
    c.max_entries      = 4;
    c.entry_mode       = CcgConfig::EntryMode::Immediate;   // 立刻建仓，不等信号
    c.dynamic_band_mode = false;
    c.interval_pct     = 2.0;
    c.trail_entry      = 0.3;
    c.tp_pct           = 100.0;    // 实际上关掉止盈，让仓位一路扛到底
    c.trail_tp         = 1.0;
    c.stop_loss_pct    = 0;
    c.use_trend_filter = false;
    c.use_htf_filter   = c.use_sr_support = c.use_sr_headroom = false;
    c.sr_radar         = false;
    c.auto_restart     = false;
    return o;
}

int main() {
    // ── ① 会计恒等式：维持保证金 = 名义 × mmr ───────────────────────────────
    {
        MultiSimClient sim;
        sim.set_market("XUSDT", 100.0, 0.0);
        sim.place_market_order("XUSDT", "BUY", 10.0, false);   // 名义 ≈ 1000
        double notion = sim.total_notional();
        check(std::fabs(notion - 1000.0) < 1.0, "持仓名义价值 ≈ 1000");
        check(std::fabs(sim.maintenance_margin(0.004) - notion * 0.004) < 1e-9,
              "维持保证金 = 名义 × mmr");
        int n = sim.force_close_all();
        check(n == 1, "强制平仓平掉了 1 个品种");
        check(sim.total_notional() < 1e-9, "强制平仓后名义价值归零");
        check(sim.force_close_all() == 0, "已空仓时强制平仓是幂等的（0 个）");
    }

    // ── ② 该爆的必须爆：本金极小 + 一路暴跌 ──────────────────────────────────
    // 本金 300U、预算 3000U（10 倍于本金）、10x 杠杆，价格连续下跌 60%。
    // 满仓之后浮亏必然吃穿本金
    {
        auto s   = make_series("XUSDT", 100.0, 3000, -0.03);   // 累计约 -59%
        auto opt = make_opt(300.0, 3000.0, 10);
        auto res = run_portfolio({s}, opt);
        check(res.liquidated, "深跌行情下账户被强平");
        check(res.liq_notional > 0, "  记录了强平时刻的持仓名义价值");
        check(res.liq_ts > 0,       "  记录了强平时刻");
        // 强平后必须【立即终止】，权益曲线不该延伸到行情末尾
        check(!res.equity_curve.empty() &&
              res.equity_curve.back().first < s.bars.back().ts_ms,
              "  强平后立即终止回放，权益曲线不延伸到行情末尾");
        check(sizeof(res.final_equity) > 0 && res.final_equity < opt.initial_equity,
              "  期末权益低于本金");
    }

    // ── ③ 不该爆的一个数字都不能动 ──────────────────────────────────────────
    // 这是能安心把强平默认开启的前提：同一段温和行情，开与关必须【逐字段相同】。
    // 若这条挂了，说明强平模型在没触发时也污染了结果，历史回测结论将不再可比
    {
        auto s = make_series("XUSDT", 100.0, 1500, -0.005);    // 累计约 -7%，很温和
        auto on  = make_opt(100000.0, 3000.0, 3);              // 本金远大于仓位
        auto off = on; off.liquidation = false;

        auto r_on  = run_portfolio({s}, on);
        auto r_off = run_portfolio({s}, off);

        check(!r_on.liquidated, "温和行情 + 厚本金：没有被强平");
        check(std::fabs(r_on.total_pnl    - r_off.total_pnl)    < 1e-9, "  净盈亏与关闭强平时完全一致");
        check(std::fabs(r_on.max_drawdown - r_off.max_drawdown) < 1e-9, "  最大回撤完全一致");
        check(std::fabs(r_on.final_equity - r_off.final_equity) < 1e-9, "  期末权益完全一致");
        check(r_on.total_cycles == r_off.total_cycles,                  "  周期数完全一致");
        check(r_on.total_orders == r_off.total_orders,                  "  订单数完全一致");
        check(r_on.equity_curve.size() == r_off.equity_curve.size(),    "  权益曲线长度一致");
    }

    // ── ④ 安全边际比值：不爆时也要能看出离死多远 ────────────────────────────
    // 最大回撤这个指标是不可比的——回撤 30% 在本金厚时毫发无伤、在杠杆拉满时
    // 已经爆了。权益÷维持保证金把两者归一化到同一把尺子
    {
        auto s = make_series("XUSDT", 100.0, 1500, -0.005);
        auto thick = make_opt(100000.0, 3000.0, 3);    // 本金厚
        auto thin  = make_opt(1200.0,   3000.0, 3);    // 本金薄

        auto r_thick = run_portfolio({s}, thick);
        auto r_thin  = run_portfolio({s}, thin);

        check(r_thick.min_margin_ratio < 1e17, "厚本金：记录到了安全边际比值");
        check(r_thick.min_margin_ratio > 1.0,  "  厚本金全程 > 1（没爆）");
        if (!r_thin.liquidated) {
            check(r_thin.min_margin_ratio < r_thick.min_margin_ratio,
                  "  同样行情下本金越薄，安全边际越低");
        } else {
            std::printf("[ OK ]    薄本金在同一行情下已被强平（边际归零）\n");
        }
    }

    // ── ⑤ 关掉强平时，比值也不再记录，行为完全回到旧口径 ────────────────────
    {
        auto s = make_series("XUSDT", 100.0, 800, -0.01);
        auto off = make_opt(100000.0, 3000.0, 3);
        off.liquidation = false;
        auto r = run_portfolio({s}, off);
        check(!r.liquidated, "--no-liq：不判强平");
        check(r.min_margin_ratio > 1e17, "--no-liq：不记录安全边际（保持初值）");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
