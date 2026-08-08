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
