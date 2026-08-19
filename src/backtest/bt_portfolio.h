#pragma once
#include "backtest/bt_data.h"
#include "core/ccg_engine.h"
#include <string>
#include <vector>

// 组合回测：多品种共享一个账户跑在同一个引擎里——与实盘完全同构
// （实盘就是一个引擎管N个bot、共享账户总保证金上限）。
// 与单品种回测的本质区别：
//   - 所有品种的K线按时间戳合并成一条时间轴，同步推进
//   - 账户级总保证金上限真实生效（决定实际能同时持仓几个品种）
//   - 权益曲线是组合级的，回撤是所有品种浮亏叠加后的真实回撤
namespace ccbot::bt {

struct PortfolioOptions {
    std::vector<std::string> symbols;      // 参与的品种
    CcgConfig  base_cfg;                   // 策略模板（symbol 会被逐个覆盖）
    double     initial_equity   = 10000;   // 账户本金
    double     per_symbol_budget = 2000;   // 每品种名义预算
    double     max_total_margin  = 0;      // 账户总保证金上限（0=按本金×0.9自动）
    int        max_positions     = 50;     // 最大同时持仓品种数
    int64_t    start_ms = 0, end_ms = 0;
    // 周期熊市判定（BTC日线驱动，喂给 CcgEngine::set_market_bearish）。
    // bear_ma_days>0：BTC日收盘 < N日均线；bear_dd_pct>0：距滚动峰值回撤超过此%。
    // 两者都设则须【同时满足】才判熊——单看均线在震荡市会来回翻，单看回撤在
    // 深跌后的反弹初期会一直判熊而错过最好的建仓区
    int        bear_ma_days = 0;
    double     bear_dd_pct  = 0;

    // ── 强平模型 ──────────────────────────────────────────────────────────────
    // 此前回测【完全不模拟强平】，这对本策略是结构性缺陷而非普通缺项：纯多 DCA
    // 的主要死法就是强平，而回测恰好把这条路径挖空了。后果是回测里"扛过去了"的
    // 深度回撤，现实中可能早已被强平出局——保底利润 3.5% 那组"多赚 900U 但回撤
    // 相当于 80% 本金"的对照，那 900U 大概率根本拿不到。
    //
    // 全仓判定：账户权益 ≤ 维持保证金 → 强平，全部仓位按市价强制平掉。
    //   账户权益   = 本金 + 已实现 + 未实现
    //   维持保证金 = Σ(名义价值 × mmr)
    //
    // 默认开启。注意这【不会改变任何本来就没被强平的回测结果】——没触发就完全
    // 不介入，历史数字原样可比；只有本来就该爆的那些组合会显示出来。
    bool       liquidation   = true;
    // 维持保证金率。币安按名义价值分档（BTCUSDT 5万U以内 0.4%，往上逐档抬高），
    // 这里用单一值近似。用小额档位的 0.4% 是【乐观】的一侧——真实分档只会更严，
    // 所以本模型给出的是"最好情况下的存活线"
    double     mmr           = 0.004;
};

struct SymbolStat {
    std::string symbol;
    double pnl = 0;
    int    cycles = 0, wins = 0;
    int    max_layers = 0;
};

struct PortfolioResult {
    double total_pnl     = 0;
    double return_pct    = 0;
    double fees          = 0;
    double funding       = 0;
    double max_drawdown  = 0;
    double max_dd_pct    = 0;
    double final_equity  = 0;
    int    total_cycles  = 0;
    int    total_wins    = 0;
    int    total_orders  = 0;
    int    peak_positions = 0;      // 实际同时持仓品种数峰值
    double peak_margin    = 0;      // 峰值占用保证金
    double peak_notional  = 0;
    int    margin_blocks  = 0;      // 因总保证金上限被拦的次数
    int    symbols_traded = 0;      // 实际开过仓的品种数
    // ── 强平 ──────────────────────────────────────────────────────────────────
    // liquidated=true 时，这一轮的收益/回撤数字【全部作废】——账户在中途就没了，
    // 后面的行情它根本没参与。必须当作"这组参数在这段行情里死了"来读
    bool    liquidated    = false;
    int64_t liq_ts        = 0;      // 强平发生的时间戳
    double  liq_equity    = 0;      // 强平那一刻的账户权益
    double  liq_notional  = 0;      // 强平那一刻的持仓名义价值
    double  min_margin_ratio = 1e18; // 全程最低的 权益÷维持保证金（越接近1越危险）
    std::vector<SymbolStat> per_symbol;
    std::vector<std::pair<int64_t,double>> equity_curve;   // {ts, equity} 按天

    std::string to_text() const;
};

PortfolioResult run_portfolio(const std::vector<Series>& all,
                              const PortfolioOptions& opt);

// 全市场版：流式读取（内存与品种数无关），支持数百品种。
// files: 品种 → 该品种的CSV文件列表（跨年多文件）
PortfolioResult run_portfolio_stream(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& files,
    const PortfolioOptions& opt);

} // namespace ccbot::bt
