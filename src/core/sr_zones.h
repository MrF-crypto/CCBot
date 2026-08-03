#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

// 支撑/阻力区域自动检测：摆动点分形 → ATR容差聚类 → 强度评分 + FVG缺口检测。
// 全部是纯数学函数，不依赖网络/引擎状态，方便单元测试。
//
// 设计要点：
//  - 支撑/阻力是"带厚度的区域"不是线，厚度来自聚类容差（0.5×ATR，波动率自适应）
//  - 评分 = 触碰次数 × 时间衰减 × 成交量权重，"攻防转换"过的区域（既当过支撑又
//    当过阻力）加成 ——历史上被双向验证过的位置最硬
//  - 每次全量重算（滚动窗口），区域自然生灭：被跌破的支撑区在现价之上就自动
//    变成阻力（角色在查询时按现价判定，攻防互换是涌现行为不需要显式状态机）
//  - FVG（公允价值缺口）：三根K线的跳空形态，未被回补的作为另一类区域并入
namespace ccbot::srzones {

struct Bar {
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

// 区域来源类型（位掩码，一个区域可以由多种来源共振叠加而成）。
// "≥2种独立点位叠在同一价位"是整套关键位方法论的灵魂——共振数越高越值得动手
enum Src : unsigned {
    SrcSwing = 1,   // 摆动点聚类（订单块/支撑阻力块的程序化等价物）
    SrcFVG   = 2,   // 公允价值缺口
    SrcPOC   = 4,   // 筹码峰（成交量分布峰值价位）
    SrcFib   = 8,   // 斐波那契回撤位（0.5/0.618/0.786）
};

struct Zone {
    double   lo       = 0;   // 区域下沿
    double   hi       = 0;   // 区域上沿
    double   score    = 0;   // 强度评分（相对值，只用于排序/展示）
    int      touches  = 0;   // 触碰次数（聚类成员数）
    unsigned src_mask = 0;   // 来源类型位掩码（Src 组合）
    bool     flipped  = false; // 既有摆动高点又有摆动低点=攻防转换过

    double mid() const { return (lo + hi) / 2.0; }
    bool contains(double price) const { return price >= lo && price <= hi; }
    // 共振数 = 叠加的独立来源类型个数
    int confluence() const {
        unsigned m = src_mask; int c = 0;
        while (m) { c += (int)(m & 1u); m >>= 1; }
        return c;
    }
};

// 来源标签（展示/告警用）
inline std::string src_label(const Zone& z) {
    std::string s;
    auto add = [&](const char* name) { if (!s.empty()) s += "+"; s += name; };
    if (z.src_mask & SrcSwing) add(z.flipped ? "攻防转换" : "摆动");
    if (z.src_mask & SrcFVG)   add("FVG");
    if (z.src_mask & SrcPOC)   add("POC");
    if (z.src_mask & SrcFib)   add("斐波");
    return s.empty() ? "未知" : s;
}

// ── ATR（Wilder平滑）。数据不够返回 0 ─────────────────────────────────────────
inline double atr(const std::vector<Bar>& bars, int period = 14) {
    if (period <= 0 || (int)bars.size() < period + 1) return 0;
    double a = 0;
    for (int i = 1; i <= period; ++i) {
        double tr = std::max({bars[i].high - bars[i].low,
                              std::fabs(bars[i].high - bars[i - 1].close),
                              std::fabs(bars[i].low  - bars[i - 1].close)});
        a += tr;
    }
    a /= period;
    for (int i = period + 1; i < (int)bars.size(); ++i) {
        double tr = std::max({bars[i].high - bars[i].low,
                              std::fabs(bars[i].high - bars[i - 1].close),
                              std::fabs(bars[i].low  - bars[i - 1].close)});
        a = (a * (period - 1) + tr) / period;
    }
    return a;
}

// ── 摆动点（分形）：第 i 根的高/低点比左右各 n 根都【严格】极端。
//    严格比较：并列平台不算摆动点（否则横盘期每根K线都是"摆动点"，聚类被噪音淹没；
//    真实行情几乎不存在完全相等的极值，近似相等的双底由聚类容差自然合并）─────────
inline std::vector<int> pivot_highs(const std::vector<Bar>& bars, int n = 3) {
    std::vector<int> out;
    for (int i = n; i + n < (int)bars.size(); ++i) {
        bool ok = true;
        for (int j = 1; j <= n && ok; ++j)
            if (bars[i].high <= bars[i - j].high || bars[i].high <= bars[i + j].high) ok = false;
        if (ok) out.push_back(i);
    }
    return out;
}

inline std::vector<int> pivot_lows(const std::vector<Bar>& bars, int n = 3) {
    std::vector<int> out;
    for (int i = n; i + n < (int)bars.size(); ++i) {
        bool ok = true;
        for (int j = 1; j <= n && ok; ++j)
            if (bars[i].low >= bars[i - j].low || bars[i].low >= bars[i + j].low) ok = false;
        if (ok) out.push_back(i);
    }
    return out;
}

// ── FVG：看涨=第i根低点高于第i-2根高点（中间那根拉出缺口），看跌镜像。
//    只保留未被完全回补的（后续价格没有整体穿回缺口另一侧），且缺口≥min_gap ────
inline std::vector<Zone> detect_fvg(const std::vector<Bar>& bars, double min_gap) {
    std::vector<Zone> out;
    for (int i = 2; i < (int)bars.size(); ++i) {
        // 看涨FVG：区域 [bars[i-2].high, bars[i].low]
        if (bars[i].low - bars[i - 2].high >= min_gap) {
            bool filled = false;
            for (int j = i + 1; j < (int)bars.size() && !filled; ++j)
                if (bars[j].low <= bars[i - 2].high) filled = true;
            if (!filled) {
                Zone z; z.lo = bars[i - 2].high; z.hi = bars[i].low;
                z.src_mask = SrcFVG; z.touches = 1; z.score = 0.8;
                out.push_back(z);
            }
        }
        // 看跌FVG：区域 [bars[i].high, bars[i-2].low]
        if (bars[i - 2].low - bars[i].high >= min_gap) {
            bool filled = false;
            for (int j = i + 1; j < (int)bars.size() && !filled; ++j)
                if (bars[j].high >= bars[i - 2].low) filled = true;
            if (!filled) {
                Zone z; z.lo = bars[i].high; z.hi = bars[i - 2].low;
                z.src_mask = SrcFVG; z.touches = 1; z.score = 0.8;
                out.push_back(z);
            }
        }
    }
    return out;
}

// ── POC 筹码峰：按价格分桶叠加成交量，峰值桶价位 = 机构吸筹密集区。
//    返回主峰 + 显著次峰（≥主峰60%且距离≥3桶），区域厚度 ±0.25×ATR ────────────
inline std::vector<Zone> detect_poc(const std::vector<Bar>& bars, double atr_val,
                                    int buckets = 50) {
    std::vector<Zone> out;
    if (bars.empty() || buckets < 4 || atr_val <= 0) return out;

    double pmin = bars[0].low, pmax = bars[0].high;
    for (const auto& b : bars) { pmin = std::min(pmin, b.low); pmax = std::max(pmax, b.high); }
    if (pmax <= pmin) return out;
    const double bw = (pmax - pmin) / buckets;

    std::vector<double> vol(buckets, 0.0);
    for (const auto& b : bars) {
        double typical = (b.high + b.low + b.close) / 3.0;
        int idx = std::min(buckets - 1, std::max(0, (int)((typical - pmin) / bw)));
        vol[idx] += b.volume;
    }

    int main_i = 0;
    for (int i = 1; i < buckets; ++i) if (vol[i] > vol[main_i]) main_i = i;
    if (vol[main_i] <= 0) return out;

    auto mk = [&](int i, double base_score) {
        Zone z;
        double center = pmin + (i + 0.5) * bw;
        z.lo = center - atr_val * 0.25;
        z.hi = center + atr_val * 0.25;
        z.src_mask = SrcPOC; z.touches = 1; z.score = base_score;
        return z;
    };
    out.push_back(mk(main_i, 1.2));

    // 次峰：足够显著且离主峰足够远（避免同一个筹码堆被计两次）
    int sec_i = -1;
    for (int i = 0; i < buckets; ++i) {
        if (std::abs(i - main_i) < 3) continue;
        if (sec_i < 0 || vol[i] > vol[sec_i]) sec_i = i;
    }
    if (sec_i >= 0 && vol[sec_i] >= vol[main_i] * 0.6)
        out.push_back(mk(sec_i, 0.9));
    return out;
}

// ── 斐波那契回撤位：取窗口内的最高点和最低点作为主波段，按先后顺序判定方向，
//    回撤 0.5 / 0.618 / 0.786 三档入池（0.618 黄金分割权重最高），厚度 ±0.15×ATR ──
inline std::vector<Zone> detect_fib(const std::vector<Bar>& bars, double atr_val) {
    std::vector<Zone> out;
    if ((int)bars.size() < 10 || atr_val <= 0) return out;

    int ih = 0, il = 0;
    for (int i = 1; i < (int)bars.size(); ++i) {
        if (bars[i].high > bars[ih].high) ih = i;
        if (bars[i].low  < bars[il].low)  il = i;
    }
    double high = bars[ih].high, low = bars[il].low;
    double range = high - low;
    if (range < atr_val) return out;   // 波段太小，回撤位没有意义

    const double ratios[] = {0.5, 0.618, 0.786};
    const double scores[] = {0.6, 0.9,   0.6};
    for (int k = 0; k < 3; ++k) {
        // 低点在前=上升波段：从高点往下回撤；高点在前=下降波段：从低点往上回撤
        double level = (il < ih) ? high - ratios[k] * range
                                  : low  + ratios[k] * range;
        Zone z;
        z.lo = level - atr_val * 0.15;
        z.hi = level + atr_val * 0.15;
        z.src_mask = SrcFib; z.touches = 1; z.score = scores[k];
        out.push_back(z);
    }
    return out;
}

// ── 共振合并：不同来源的区域若价位重叠则合并成一个区域——
//    来源掩码取并集、评分累加，最后按共振数加权（每多一种独立来源 +50%）。
//    "≥2种点位叠在同一价位"共振越多的区域越硬，排序自然靠前 ───────────────────
inline std::vector<Zone> merge_zones(std::vector<Zone> zones) {
    std::sort(zones.begin(), zones.end(),
              [](const Zone& a, const Zone& b) { return a.score > b.score; });
    std::vector<Zone> merged;
    for (const auto& z : zones) {
        bool absorbed = false;
        for (auto& m : merged) {
            if (z.lo <= m.hi && z.hi >= m.lo) {   // 区间重叠
                m.src_mask |= z.src_mask;
                m.flipped   = m.flipped || z.flipped;
                m.score    += z.score * 0.8;
                m.touches  += z.touches;
                m.lo = std::min(m.lo, z.lo);
                m.hi = std::max(m.hi, z.hi);
                absorbed = true;
                break;
            }
        }
        if (!absorbed) merged.push_back(z);
    }
    for (auto& m : merged)
        m.score *= 1.0 + 0.5 * (m.confluence() - 1);
    std::sort(merged.begin(), merged.end(),
              [](const Zone& a, const Zone& b) { return a.score > b.score; });
    return merged;
}

// ── 主入口：摆动点聚类 + 评分 + FVG，返回按评分降序的区域列表 ─────────────────
// pivot_n: 分形左右根数；cluster_atr_mult: 聚类容差(×ATR)；max_zones: 最多返回个数
inline std::vector<Zone> detect_zones(const std::vector<Bar>& bars,
                                      int pivot_n = 3,
                                      double cluster_atr_mult = 0.5,
                                      int max_zones = 12) {
    std::vector<Zone> zones;
    if ((int)bars.size() < pivot_n * 2 + 5) return zones;

    double a = atr(bars, 14);
    if (a <= 0) return zones;
    const double tol = a * cluster_atr_mult;
    const int    n   = (int)bars.size();

    // 收集摆动点：{价格, K线序号, 是否高点}
    struct Piv { double price; int idx; bool is_high; };
    std::vector<Piv> pivs;
    for (int i : pivot_highs(bars, pivot_n)) pivs.push_back({bars[i].high, i, true});
    for (int i : pivot_lows (bars, pivot_n)) pivs.push_back({bars[i].low,  i, false});
    if (pivs.empty()) return zones;

    // 平均成交量（成交量权重的基准）
    double avg_vol = 0;
    for (const auto& b : bars) avg_vol += b.volume;
    avg_vol = std::max(avg_vol / n, 1e-12);

    // 按价格排序后贪心合并：相邻点差 ≤ tol 归同一簇
    std::sort(pivs.begin(), pivs.end(),
              [](const Piv& x, const Piv& y) { return x.price < y.price; });

    size_t start = 0;
    while (start < pivs.size()) {
        size_t end = start;
        while (end + 1 < pivs.size() && pivs[end + 1].price - pivs[end].price <= tol) ++end;

        Zone z;
        z.lo = pivs[start].price;
        z.hi = pivs[end].price;
        // 最小厚度：单点/极窄簇也给 0.25×ATR 的半厚度，容差带防插针
        double min_half = a * 0.25;
        double mid = (z.lo + z.hi) / 2.0;
        if (z.hi - z.lo < min_half * 2) { z.lo = mid - min_half; z.hi = mid + min_half; }

        z.src_mask = SrcSwing;
        bool has_high = false, has_low = false;
        for (size_t k = start; k <= end; ++k) {
            const auto& pv = pivs[k];
            (pv.is_high ? has_high : has_low) = true;
            // 时间衰减：最近的触碰权重高（半衰期约100根K线）
            double age    = (double)(n - 1 - pv.idx);
            double decay  = std::exp(-age / 144.0);
            // 成交量权重：放量触碰更有意义（上限3倍防单根异常量主导）
            double volw   = 1.0 + std::min(bars[pv.idx].volume / avg_vol, 3.0) * 0.5;
            z.score += decay * volw;
            z.touches++;
        }
        z.flipped = has_high && has_low;
        if (z.flipped) z.score *= 1.3;   // 攻防转换过的位置加成

        // 单点区域（touches==1）噪音大，只有近期的才保留
        if (z.touches >= 2 || (n - 1 - pivs[start].idx) < 60)
            zones.push_back(z);

        start = end + 1;
    }

    // 其他来源并入区域池：FVG（缺口显著性阈值 0.3×ATR）、POC 筹码峰、斐波那契回撤位
    for (auto& z : detect_fvg(bars, a * 0.3)) zones.push_back(z);
    for (auto& z : detect_poc(bars, a))       zones.push_back(z);
    for (auto& z : detect_fib(bars, a))       zones.push_back(z);

    // 共振合并：跨来源重叠的区域合成一个，共振数加权后重新排序
    zones = merge_zones(std::move(zones));
    if ((int)zones.size() > max_zones) zones.resize(max_zones);
    return zones;
}

// ── 查询辅助：现价下方最近的支撑区 / 上方最近的阻力区（角色按现价即时判定）────
inline const Zone* nearest_support(const std::vector<Zone>& zones, double price) {
    const Zone* best = nullptr;
    for (const auto& z : zones)
        if (z.hi < price && (!best || z.hi > best->hi)) best = &z;
    return best;
}

inline const Zone* nearest_resistance(const std::vector<Zone>& zones, double price) {
    const Zone* best = nullptr;
    for (const auto& z : zones)
        if (z.lo > price && (!best || z.lo < best->lo)) best = &z;
    return best;
}

} // namespace ccbot::srzones
