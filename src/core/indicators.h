#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

// 纯数学指标计算：不依赖网络/JSON，方便单独写单元测试验证正确性。
// K线输入一律按时间正序（最后一个元素 = 最新的一根，可以是还没走完的未收盘K线）。
// （布林带 / EMA / 带偏移 SMA / RSI 是监控时代的遗留，生产代码早已不用，v5.10 删掉）
namespace ccbot::indicators {

// ── OHLC 系列指标 ───────────────────────────────────────────────────────────
// ATR/唐奇安要最高最低价，所以单独一个输入类型。
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

// 量比：vols[idx] ÷ 它之前 window 根的平均量（不含 idx 自己）。
// 海龟拿【当前未收盘根的已成交量】去比（idx = 最后一根），裸K拿【刚收盘那根】去比
// （idx = 倒数第二根）。数据不足或均量为 0 时返回 0（= 拿不到）
inline double volume_ratio(const std::vector<double>& vols, int idx, int window) {
    if (window <= 0 || idx < window || idx >= (int)vols.size()) return 0;
    double sum = 0;
    for (int i = idx - window; i < idx; ++i) sum += vols[i];
    const double avg = sum / window;
    return avg > 0 ? vols[idx] / avg : 0;
}

// 未收盘K线的量比按已走时间折算：raw ÷ 已走比例。已走比例按至少 floor 算——
// 刚开盘几分钟的一点量不能被放大十几倍（那是最容易放进假突破的时候），
// 超过 1（时钟偏差）按 1 算。raw ≤ 0（拿不到）原样返回 0
inline constexpr double kVolPaceFloor = 0.25;
inline double paced_ratio(double raw, double elapsed_frac, double floor = kVolPaceFloor) {
    if (!(raw > 0)) return 0;
    double f = elapsed_frac;
    if (!(f >= floor)) f = floor;   // NaN 也走这里
    if (f > 1) f = 1;
    return raw / f;
}

struct ErResult {
    bool   ok = false;
    double er = 0;   // 带符号，[-1, 1]。正 = 净上涨
};

// 效率比（Kaufman）：净变化 ÷ 逐根收盘价变化的绝对值之和。
// 1 = 一条直线，接近 0 = 走了很多路又回到原地（来回扫）。
//
// 窗口与唐奇安通道对齐：同样排除最后 exclude_last 根，取通道那 period 根的
// 收盘价，再加上它们之前一根的收盘价作起点——这样 period 根K线自己的涨跌
// 全部算进去（period 个差值），而不是少掉第一根。
// 数据不足或总路程为 0（一动不动）时 ok=false
inline ErResult efficiency_ratio(const std::vector<Ohlc>& bars, int period,
                                 int exclude_last = 1) {
    ErResult r;
    if (period <= 0 || exclude_last < 0) return r;
    const int end = (int)bars.size() - exclude_last;   // 不含 end
    const int start = end - period - 1;                // 起点那根的收盘价
    if (start < 0) return r;

    double path = 0;
    for (int i = start + 1; i < end; ++i) path += std::fabs(bars[i].close - bars[i - 1].close);
    if (!(path > 0)) return r;
    r.er = (bars[end - 1].close - bars[start].close) / path;
    r.ok = true;
    return r;
}

} // namespace ccbot::indicators
