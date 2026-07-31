#pragma once
#include <algorithm>

// 动态W模式的参数推导：全部是纯数学函数，不依赖网络/引擎状态，方便单元测试。
// 设计依据（详见 README 策略说明）：
//   间隔     = W/3      —— 带宽的1/3，典型超跌能吃进2~3层
//   追踪止盈 = 0.15*W   —— 吃突破段的鱼尾，又不把半个带宽的利润吐回去
//   追踪建仓 = 0.10*W   —— 等反弹企稳再接，把补仓价往下摊
// 各自带夹逼上下限，防止极端行情下 W 异常放大/缩小把参数推到不合理区间。
namespace ccbot::dynparams {

inline double clampv(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

// 带宽百分比 W = (上轨-下轨)/下轨*100；数据非法（下轨<=0 或上下轨倒挂）返回 0
inline double band_width_pct(double lb, double ub) {
    if (lb <= 0 || ub <= lb) return 0;
    return (ub - lb) / lb * 100.0;
}

// W 为百分比（如 2.0 表示 2%），返回值同样是百分比
inline double interval_pct   (double W) { return clampv(W / 3.0,  0.3,  1.5); }
inline double trail_tp_pct   (double W) { return clampv(0.15 * W, 0.2,  0.6); }
inline double trail_entry_pct(double W) { return clampv(0.10 * W, 0.15, 0.4); }

} // namespace ccbot::dynparams
