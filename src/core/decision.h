#pragma once
#include <string>
#include <cmath>

// 首仓的宏观许可层：日线%B + 24小时涨幅 + 近7日涨幅，三条平级独立。
// 微观扳机（1h BOLL+RSI+站稳）与趋势状态机沿用引擎既有实现，不在此重复。
// 全部为纯函数——输入进出都是值，不碰网络/引擎状态，方便单元测试。
//
// ⚠ v4.0.16 起【结构层已整体移除】：支撑拦截、净空拦截、SR 区域检测、
//   止盈锚定阻力区、结构性止损全部删除（约 610 行）。原因是那套东西需要
//   每品种每 15 分钟拉 400 根 K 线做区域检测，占着共用线程池，而实际取向上
//   用不到；留着只是让每个 tick 的决策路径更长、配置面更宽。
//   历史实证结论仍记录在 docs/BACKTEST.md，代码在 git 历史里可查。
namespace ccbot::decision {

// 高周期%B：价格在带中的相对位置。0=下轨 1=上轨，可越界；数据非法返回 -1
inline double pct_b(double price, double lb, double ub) {
    if (lb <= 0 || ub <= lb || price <= 0) return -1.0;
    return (price - lb) / (ub - lb);
}

struct Inputs {
    bool is_long = true;
    // strict=true（启用拦截时）：数据缺失【等同条件不满足】，不开仓——既然明确
    // 启用了闸门，闸门就该真的守住，"数据没到"和"条件不满足"同等对待
    bool strict = false;
    // 宏观：日线%B（htf_ok=false 表示数据缺失/过期）
    bool   use_htf     = true;
    bool   htf_ok      = false;
    double htf_pct_b   = 0.5;
    double htf_pos_max = 0.80;
    // 涨幅与 %B 口径正交：
    //   %B  问"价格在波动区间的什么位置"
    //   涨幅问"最近涨得多急"
    // 窄幅横盘时 %B 可以贴着上轨而涨幅微乎其微；急涨突破时涨幅巨大而 %B 未必越界。
    // 两者会给出相反的答案，所以是独立判据而不是 %B 的替代。
    // 各自 0=关，且不受 use_htf 约束（可以只用涨幅、不用 %B）
    //
    // ⚠ 三个判据【数据来源不同】，就绪状态必须分开报：
    //     %B 与 7日涨幅  ← 日线K线（REST，每5分钟一拉）
    //     24h 涨幅       ← @ticker 推送流（WebSocket）
    //   v4.0.11 之前它们共用 htf_ok 一个标志：24h 拿不到时会把 %B 也标成"缺失"，
    //   日志显示 "%B=缺失✗" 并提示"新上市品种需等日线21根历史"——而 %B 其实好好的，
    //   真正缺的是那条 WebSocket 流。排查方向被完全带偏
    double day_chg_pct  = 0;   // 24h 滚动涨幅%
    double week_chg_pct = 0;   // 近 7 根日线涨幅%
    double day_chg_max  = 0;   // 0=关；做多时涨幅高于此值拦截，做空镜像
    double week_chg_max = 0;   // 0=关
    bool   day_chg_ok   = false;   // 24h 涨幅数据是否就绪（独立于 htf_ok）
};

struct Verdict {
    bool htf_block      = false;   // 日线高位（做多）/低位（做空）
    bool day_chg_block  = false;   // 24h 涨幅过热
    bool week_chg_block = false;   // 7日涨幅过热
    bool htf_missing    = false;   // 日线K线缺失（%B 与 7日涨幅）
    bool day_chg_missing= false;   // 24h 涨幅缺失（@ticker 推送流没到）
    bool data_block     = false;   // strict 模式下因数据缺失而拦截

    bool pass() const {
        return !htf_block && !day_chg_block && !week_chg_block && !data_block;
    }
};

inline Verdict evaluate(const Inputs& in) {
    Verdict v;
    // ── 日线K线来源：%B 与 7日涨幅 ──
    if (in.use_htf || in.week_chg_max > 0) {
        if (!in.htf_ok) {
            v.htf_missing = true;
            if (in.strict) v.data_block = true;   // 严格模式：数据没到不开仓
        } else {
            if (in.use_htf && (in.is_long ? (in.htf_pct_b > in.htf_pos_max)
                                          : (in.htf_pct_b < 1.0 - in.htf_pos_max)))
                v.htf_block = true;     // 做多拦高位；做空镜像拦低位
            // 做多拦"涨太急"，做空镜像拦"跌太急"
            if (in.week_chg_max > 0 && (in.is_long ? in.week_chg_pct >  in.week_chg_max
                                                   : in.week_chg_pct < -in.week_chg_max))
                v.week_chg_block = true;
        }
    }
    // ── @ticker 推送流来源：24h 涨幅。就绪状态独立判，不能拖累上面那组 ──
    if (in.day_chg_max > 0) {
        if (!in.day_chg_ok) {
            v.day_chg_missing = true;
            if (in.strict) v.data_block = true;
        } else if (in.is_long ? in.day_chg_pct >  in.day_chg_max
                              : in.day_chg_pct < -in.day_chg_max) {
            v.day_chg_block = true;
        }
    }
    return v;
}

// 决策快照的人读摘要（日志用），形如 "%B=0.87✗高位 | 24h涨6.20%✗过热"
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
    // 两条涨幅只在启用时才占位，免得默认关闭的用户每行日志都看到两段"关"。
    // 各自用【自己那条数据线】的缺失标志：24h 来自 WebSocket 推送流，
    // 7日涨幅来自日线K线，把两者混报会让排查方向完全错掉
    if (in.day_chg_max > 0) {
        s += " | 24h涨";
        if (v.day_chg_missing) s += std::string(miss) + "(推送流未到)";
        else                   s += num(in.day_chg_pct) + "%" + (v.day_chg_block ? "✗过热" : "✓");
    }
    if (in.week_chg_max > 0) {
        s += " | 7日涨";
        if (v.htf_missing) s += miss;
        else               s += num(in.week_chg_pct) + "%" + (v.week_chg_block ? "✗过热" : "✓");
    }
    return s;
}

} // namespace ccbot::decision
