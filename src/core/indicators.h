#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// 纯数学指标计算：不依赖网络/JSON，方便单独写单元测试验证正确性。
// 输入统一是"按时间正序排列的收盘价"（最后一个元素 = 最新/当前K线的收盘价，
// 可以是还没走完的"未收盘"K线的实时值）。
namespace ccbot::indicators {

struct BollResult {
    bool   ok = false;
    double ub = 0;
    double mb = 0;
    double lb = 0;
};

// 布林带：取最后 period 根收盘价的均值 ± mult * 标准差（总体标准差，除以 N 不是 N-1）
inline BollResult bollinger(const std::vector<double>& closes, int period, double mult) {
    BollResult r;
    if (period <= 1 || (int)closes.size() < period) return r;

    int start = (int)closes.size() - period;
    double sum = 0;
    for (int i = start; i < (int)closes.size(); ++i) sum += closes[i];
    double mean = sum / period;

    double var = 0;
    for (int i = start; i < (int)closes.size(); ++i) {
        double d = closes[i] - mean;
        var += d * d;
    }
    var /= period;

    r.mb = mean;
    r.ub = mean + mult * std::sqrt(var);
    r.lb = mean - mult * std::sqrt(var);
    r.ok = true;
    return r;
}

// RSI：Wilder 平滑法。period 根差分做初始平均，之后逐根滚动平滑。
// 数据不够（< period+1 根）时返回中性值 50。
inline double rsi(const std::vector<double>& closes, int period) {
    if (period <= 0 || (int)closes.size() < period + 1) return 50.0;

    double avg_gain = 0, avg_loss = 0;
    for (int i = 1; i <= period; ++i) {
        double d = closes[i] - closes[i - 1];
        if (d > 0) avg_gain += d; else avg_loss -= d;
    }
    avg_gain /= period;
    avg_loss /= period;

    for (int i = period + 1; i < (int)closes.size(); ++i) {
        double d = closes[i] - closes[i - 1];
        if (d > 0) {
            avg_gain = (avg_gain * (period - 1) + d) / period;
            avg_loss =  avg_loss * (period - 1)      / period;
        } else {
            avg_gain =  avg_gain * (period - 1)      / period;
            avg_loss = (avg_loss * (period - 1) - d) / period;
        }
    }
    if (avg_loss < 1e-10) return 100.0;
    return 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
}

} // namespace ccbot::indicators
