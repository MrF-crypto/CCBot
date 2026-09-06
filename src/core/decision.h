#pragma once
#include "core/sr_zones.h"
#include <string>
#include <vector>
#include <cmath>

// v3.0 三层决策核：宏观许可（日线%B）× 结构定位（支撑质量+头顶净空）。
// 微观扳机（1h BOLL+RSI+站稳）与趋势状态机沿用引擎既有实现，不在此重复。
// 全部为纯函数——输入进出都是值，不碰网络/引擎状态，方便单元测试。
namespace ccbot::decision {

// 高周期%B：价格在带中的相对位置。0=下轨 1=上轨，可越界；数据非法返回 -1
inline double pct_b(double price, double lb, double ub) {
    if (lb <= 0 || ub <= lb || price <= 0) return -1.0;
    return (price - lb) / (ub - lb);
}

// 结构摘要：从区域池提炼首仓决策需要的全部结构信息（相对当前价格）。
// 支撑/阻力都只认共振数 >= min_conf 的"够格"区域，弱区域不参与决策
struct StructDigest {
    bool   ok       = false;   // 有区域数据
    bool   at_support = false; // 价格正处于够格支撑区内（脚下有支撑的最强形态）
    double sup_lo = 0, sup_hi = 0; int sup_conf = 0;   // 下方最近够格支撑区
    double res_lo = 0, res_hi = 0; int res_conf = 0;   // 上方最近够格阻力区
    double deep_sup_lo = 0;    // 下方最深够格支撑区的下沿（结构止损参考位）
};

// 结构判定的两个可选加固（默认关=历史行为，效果由回测裁决）：
//  independent_conf：共振按【独立证据族】计数（摆动与其派生的斐波归为一族）
//  lower_half_only ：价格须落在区域【下半部】才算踩住支撑——区域最宽可达2×ATR，
//                    刚碰到顶边和踩到区域底部的含义完全不同
// res_min_conf：阻力侧单独的计票门槛（0=沿用 min_conf）。两侧同门槛存在不对称：
// 支撑够不上票数 ⇒ 判"无支撑" ⇒ 拦（保守）；阻力够不上票数 ⇒ 这堵墙被忽略 ⇒
// 净空显示∞ ⇒ 放行（激进）。同一个门槛，一边把关一边放水。给阻力设更低的门槛
// 可以让两侧都朝"保守"对齐——是否值得由回测裁决
struct DigestOpts {
    int  min_conf         = 2;
    int  res_min_conf     = 0;
    bool independent_conf = false;
    bool lower_half_only  = false;
};

inline StructDigest digest_zones(const std::vector<srzones::Zone>& zones,
                                 double price, const DigestOpts& opt) {
    StructDigest d;
    if (zones.empty() || price <= 0) return d;
    const int res_th = opt.res_min_conf > 0 ? opt.res_min_conf : opt.min_conf;
    d.ok = true;
    for (const auto& z : zones) {
        int conf = opt.independent_conf ? z.confluence_independent() : z.confluence();
        // 上方区域走阻力门槛，其余走支撑门槛
        if (conf < (z.lo > price ? res_th : opt.min_conf)) continue;
        if (z.contains(price)) {
            // 正处区内：既是脚下支撑也可能是头顶阻力，按"支撑在场"处理
            if (!opt.lower_half_only || price <= z.mid()) d.at_support = true;
            if (d.sup_conf == 0 || z.lo > d.sup_lo) {
                d.sup_lo = z.lo; d.sup_hi = z.hi; d.sup_conf = conf;
            }
        } else if (z.hi < price) {                       // 下方支撑
            if (d.sup_conf == 0 || z.hi > d.sup_hi) {
                d.sup_lo = z.lo; d.sup_hi = z.hi; d.sup_conf = conf;
            }
            if (d.deep_sup_lo == 0 || z.lo < d.deep_sup_lo) d.deep_sup_lo = z.lo;
        } else {                                         // 上方阻力 (z.lo > price)
            if (d.res_conf == 0 || z.lo < d.res_lo) {
                d.res_lo = z.lo; d.res_hi = z.hi; d.res_conf = conf;
            }
        }
    }
    // 处于区内时，该区自身下沿也是深支撑候选
    if (d.at_support && (d.deep_sup_lo == 0 || d.sup_lo < d.deep_sup_lo))
        d.deep_sup_lo = d.sup_lo;
    return d;
}

// 兼容旧签名（等价于 DigestOpts{min_conf, false, false}）
inline StructDigest digest_zones(const std::vector<srzones::Zone>& zones,
                                 double price, int min_conf) {
    DigestOpts o; o.min_conf = min_conf;
    return digest_zones(zones, price, o);
}

// 头顶净空比：到上方最近阻力的距离 ÷ 预期止盈距离。
// 无阻力（res_lo<=0）视为净空无限大；tp_distance 非法返回 -1
inline double headroom_ratio(double price, double res_lo, double tp_distance) {
    if (price <= 0 || tp_distance <= 0) return -1.0;
    if (res_lo <= price) return 1e9;    // 头顶没有够格阻力
    return (res_lo - price) / tp_distance;
}

// ── 三层判定 ─────────────────────────────────────────────────────────────────
struct Inputs {
    bool is_long = true;
    // strict=true（启用拦截时）：数据缺失【等同条件不满足】，不开仓——既然明确
    // 启用了闸门，闸门就该真的守住，"数据没到"和"条件不满足"同等对待。
    // strict=false（影子模式）：数据缺失只标注不拦（反正影子模式本来就不拦）
    bool strict = false;
    // 宏观：日线%B（htf_ok=false 表示数据缺失/过期）
    bool   use_htf     = true;
    bool   htf_ok      = false;
    double htf_pct_b   = 0.5;
    double htf_pos_max = 0.80;
    // 宏观涨幅：与 %B 同源（同一次高周期K线拉取），但口径正交——
    //   %B  问的是"价格在波动区间的什么位置"
    //   涨幅问的是"最近涨得多急"
    // 窄幅横盘时 %B 可以贴着上轨而涨幅微乎其微；急涨突破时涨幅巨大而 %B 未必越界。
    // 两者会给出相反的答案，所以是独立判据而不是 %B 的替代。
    // 各自 0=关，且不受 use_htf 开关约束（可以只用涨幅、不用 %B）
    double day_chg_pct  = 0;   // 近 1 根高周期K线涨幅%
    double week_chg_pct = 0;   // 近 7 根高周期K线涨幅%（htf_interval=1d 时即近7日）
    double day_chg_max  = 0;   // 0=关；做多时涨幅高于此值拦截，做空镜像
    double week_chg_max = 0;   // 0=关
    // 结构层拆成两个独立判据——它们问的是完全不同的问题：
    //   支撑：脚下此刻有没有踩住够格区域（"这里该不该买"）
    //   净空：头顶到最近够格阻力的空间够不够止盈（"买了跑不跑得掉"）
    // 绑在一个开关里时无法知道拦截到底来自哪一条
    bool   use_sr_support  = true;
    bool   use_sr_headroom = true;
    bool   sr_ok         = false;
    bool   at_support    = false;
    double headroom      = 1e9;   // 已算好的净空比
    double headroom_min  = 1.5;
};

struct Verdict {
    bool htf_block      = false;   // 宏观：日线高位（做多）/低位（做空）
    bool day_chg_block  = false;   // 宏观：日涨幅过热
    bool week_chg_block = false;   // 宏观：7日涨幅过热
    bool support_block  = false;   // 结构：脚下无够格支撑
    bool headroom_block = false;   // 结构：头顶净空不足
    bool htf_missing    = false;   // 数据缺失标注
    bool sr_missing     = false;
    bool data_block     = false;   // strict 模式下因数据缺失而拦截

    bool pass() const {
        return !htf_block && !day_chg_block && !week_chg_block
            && !support_block && !headroom_block && !data_block;
    }
};

inline Verdict evaluate(const Inputs& in) {
    Verdict v;
    // %B 与两条涨幅共用同一份高周期K线，所以数据缺失只判一次
    const bool use_chg = in.day_chg_max > 0 || in.week_chg_max > 0;
    if (in.use_htf || use_chg) {
        if (!in.htf_ok) {
            v.htf_missing = true;
            if (in.strict) v.data_block = true;   // 严格模式：数据没到不开仓
        } else {
            if (in.use_htf && (in.is_long ? (in.htf_pct_b > in.htf_pos_max)
                                          : (in.htf_pct_b < 1.0 - in.htf_pos_max)))
                v.htf_block = true;     // 做多拦高位；做空镜像拦低位
            // 做多拦"涨太急"，做空镜像拦"跌太急"
            if (in.day_chg_max > 0 && (in.is_long ? in.day_chg_pct >  in.day_chg_max
                                                  : in.day_chg_pct < -in.day_chg_max))
                v.day_chg_block = true;
            if (in.week_chg_max > 0 && (in.is_long ? in.week_chg_pct >  in.week_chg_max
                                                   : in.week_chg_pct < -in.week_chg_max))
                v.week_chg_block = true;
        }
    }
    if (in.use_sr_support || in.use_sr_headroom) {
        if (!in.sr_ok) {
            // 两条判据都依赖区域数据：净空要知道头顶最近的够格阻力在哪，
            // 没数据就没法判。strict 原则一致——数据没到不开仓
            v.sr_missing = true;
            if (in.strict) v.data_block = true;
        } else {
            if (in.use_sr_support  && !in.at_support)            v.support_block  = true;
            if (in.use_sr_headroom && in.headroom < in.headroom_min) v.headroom_block = true;
        }
    }
    return v;
}

// 决策快照的人读摘要（日志用），形如 "%B=0.87✗高位 | 支撑✓在场 | 净空0.41✗不足"
inline std::string summarize(const Inputs& in, const Verdict& v) {
    auto num = [](double x) {
        std::string s = std::to_string(x);
        return s.substr(0, s.find('.') + 3);   // 保留两位小数
    };
    const char* miss = in.strict ? "缺失✗" : "缺失(放行)";
    std::string s = "%B=";
    if (v.htf_missing)      s += miss;
    else if (!in.use_htf)   s += "关";
    else                    s += num(in.htf_pct_b) + (v.htf_block ? "✗高位" : "✓");
    // 两条涨幅只在启用时才占位，免得默认关闭的用户每行日志都看到两段"关"
    if (in.day_chg_max > 0) {
        s += " | 日涨";
        if (v.htf_missing) s += miss;
        else               s += num(in.day_chg_pct) + "%" + (v.day_chg_block ? "✗过热" : "✓");
    }
    if (in.week_chg_max > 0) {
        s += " | 7日涨";
        if (v.htf_missing) s += miss;
        else               s += num(in.week_chg_pct) + "%" + (v.week_chg_block ? "✗过热" : "✓");
    }
    s += " | 支撑";
    if (!in.use_sr_support)      s += "关";
    else if (v.sr_missing)       s += miss;
    else                         s += v.support_block ? "✗无" : "✓在场";
    s += " | 净空";
    if (!in.use_sr_headroom)     s += "关";
    else if (v.sr_missing)       s += miss;
    else if (in.headroom >= 1e8) s += "∞✓";
    else                         s += num(in.headroom) + (v.headroom_block ? "✗不足" : "✓");
    return s;
}

} // namespace ccbot::decision
