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

// EMA：标准指数移动平均。前 period 根做 SMA 种子，之后按乘数 2/(period+1) 滚动。
// 数据不够（< period 根）返回 0（调用方以 0 判定"数据不足"）。
inline double ema(const std::vector<double>& closes, int period) {
    if (period <= 0 || (int)closes.size() < period) return 0;
    double e = 0;
    for (int i = 0; i < period; ++i) e += closes[i];
    e /= period;
    const double k = 2.0 / (period + 1);
    for (int i = period; i < (int)closes.size(); ++i)
        e = closes[i] * k + e * (1.0 - k);
    return e;
}

// 以序列末尾往前偏移 offset_from_end 根为终点的 SMA。
// 用于算"中轨斜率"：sma_at(c,20,0) 对比 sma_at(c,20,3) 就是中轨 3 根K线里的位移。
// 数据不够返回 0。
inline double sma_at(const std::vector<double>& closes, int period, int offset_from_end) {
    int end = (int)closes.size() - offset_from_end;   // 不含 end
    if (period <= 0 || offset_from_end < 0 || end < period) return 0;
    double s = 0;
    for (int i = end - period; i < end; ++i) s += closes[i];
    return s / period;
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

// ── OHLC 系列指标 ───────────────────────────────────────────────────────────
// 上面那些只需要收盘价，ATR/唐奇安要最高最低价，所以单独一个输入类型。
// 刻意【不】复用 TradingClient::Bar：本文件的约定是不依赖网络层，调用方自己转一下。
struct Ohlc {
    double high = 0, low = 0, close = 0;
};

// 真实波幅：当根振幅、与上一根收盘的向上跳空、向下跳空，三者取最大。
// 跳空那两项是 ATR 区别于"简单振幅均值"的地方——隔夜跳空也是真实的风险敞口。
inline double true_range(const Ohlc& cur, double prev_close) {
    double a = cur.high - cur.low;
    double b = std::fabs(cur.high - prev_close);
    double c = std::fabs(cur.low  - prev_close);
    return std::max(a, std::max(b, c));
}

// ATR：Wilder 平滑法（和上面的 rsi 同一套），与 TradingView / 币安的 ATR 对齐。
// ⚠ 用简单移动平均算 ATR 会得到不一样的数——Wilder 平滑的等效周期约为 2N-1，
//   衰减慢得多。追踪止损的距离直接由它决定，口径错了止损线就是错的。
//
// 需要 period+1 根（第一根算不出 TR，没有前收）。不够时返回 0，
// 调用方以 0 判定"数据不足"——与 ema() 的约定一致。
//
// 传不传未收盘的当前K线由调用方决定：传了更跟手但会随价格抖动，
// 止损线是棘轮（只朝有利方向移动），抖动不会让线回退，所以传是安全的。
inline double atr(const std::vector<Ohlc>& bars, int period) {
    if (period <= 0 || (int)bars.size() < period + 1) return 0;

    double seed = 0;
    for (int i = 1; i <= period; ++i) seed += true_range(bars[i], bars[i - 1].close);
    double a = seed / period;

    for (int i = period + 1; i < (int)bars.size(); ++i)
        a = (a * (period - 1) + true_range(bars[i], bars[i - 1].close)) / period;

    return a;
}

struct DonchianResult {
    bool   ok = false;
    double up = 0;   // period 根内的最高价
    double dn = 0;   // period 根内的最低价
};

// 唐奇安通道：突破入场信号的来源。
//
// ⚠ exclude_last 默认 1，【不能改成 0】：当前这根K线的最高价本身就是通道上沿的
//   一部分，把它算进去等于让"价格 >= 上沿"永远成立——信号恒真，等于没有信号。
//   通道必须只由【已经走完的】K线构成，当前K线去撞它。
//
// 数据不足（需要 period + exclude_last 根）时 ok=false，调用方不得使用 up/dn。
inline DonchianResult donchian(const std::vector<Ohlc>& bars, int period,
                               int exclude_last = 1) {
    DonchianResult r;
    if (period <= 0 || exclude_last < 0) return r;
    int end = (int)bars.size() - exclude_last;          // 不含 end
    if (end < period) return r;

    r.up = bars[end - period].high;
    r.dn = bars[end - period].low;
    for (int i = end - period + 1; i < end; ++i) {
        r.up = std::max(r.up, bars[i].high);
        r.dn = std::min(r.dn, bars[i].low);
    }
    r.ok = true;
    return r;
}

} // namespace ccbot::indicators
