#pragma once
#include "backtest/bt_data.h"
#include "core/ccg_engine.h"
#include <string>
#include <vector>

// 回放器：把 1m K线喂给【真实引擎】跑完一遍，产出绩效指标。
// 关键点：
//  - 虚拟时钟：回放一年只需几秒，用真实时钟的话冷却/数据新鲜度全部失真
//  - bar内四子tick走价（O→H→L→C 或 O→L→H→C），近似解答"先触下轨还是先反弹"
//  - 指标/趋势/%B/SR区域全部按实盘同款接口喂入，走完全相同的代码路径
namespace ccbot::bt {

struct BacktestResult {
    std::string symbol;
    // 收益
    double total_pnl      = 0;    // 净盈亏（已扣手续费+资金费）
    double return_pct     = 0;    // 相对初始保证金
    double fees           = 0;
    double funding        = 0;
    // 风险
    double max_drawdown   = 0;    // 权益曲线最大回撤（绝对值）
    double max_dd_pct     = 0;    // 相对峰值权益
    double max_notional   = 0;    // 峰值持仓名义价值
    // 交易
    int    cycles         = 0;    // 完成的周期数（平仓次数）
    int    wins           = 0;
    int    orders         = 0;
    double avg_layers     = 0;
    int    max_layers     = 0;
    double time_in_pos_pct = 0;   // 持仓时间占比
    // 三层决策统计（影子/启用都记）
    int    gate_pass      = 0;
    int    gate_block_htf = 0;
    int    gate_block_sr  = 0;
    // 权益曲线（按天采样，画图用）
    std::vector<double> equity_curve;

    double win_rate()  const { return cycles ? 100.0 * wins / cycles : 0; }
    double profit_dd() const { return max_drawdown > 1e-9 ? total_pnl / max_drawdown : 0; }
    std::string to_text() const;
};

struct ReplayOptions {
    CcgConfig cfg;                 // 策略参数（被扫描的对象）
    double    initial_equity = 10000;
    int64_t   start_ms = 0;        // 0=全段
    int64_t   end_ms   = 0;
    bool      verbose  = false;    // 打印每笔成交
};

// 跑一遍回测。series 必须是 1m 数据
BacktestResult run_replay(const Series& series, const ReplayOptions& opt);

} // namespace ccbot::bt
