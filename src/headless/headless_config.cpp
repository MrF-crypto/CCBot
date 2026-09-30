#include "headless/headless_config.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>
#include <set>
// ⚠ <string> 显式写出来：本文件用了 std::string / std::to_string。MSVC 的标准库
//   与 <sstream> 都会传递包含它，所以本机编得过、clang --driver-mode=g++ 那道
//   扫描也查不到（它用的是 MSVC 的头文件）——libstdc++ 严格起来就是推上去才炸。
//   v5.9.4 那次 CI 失败就是这个（少了 <cmath>），别再交第二次学费
#include <string>

namespace ccbot {

namespace {

double get_num(simdjson::dom::element val, double def) {
    double d;
    if (val.get(d) == simdjson::SUCCESS) return d;
    int64_t i;
    if (val.get(i) == simdjson::SUCCESS) return (double)i;
    uint64_t u;
    if (val.get(u) == simdjson::SUCCESS) return (double)u;
    return def;
}

double get_num(simdjson::dom::object& o, const char* key, double def) {
    simdjson::dom::element val;
    if (o[key].get(val) != simdjson::SUCCESS) return def;
    return get_num(val, def);
}

bool get_bool(simdjson::dom::object& o, const char* key, bool def) {
    bool v;
    if (o[key].get(v) == simdjson::SUCCESS) return v;
    return def;
}

std::string get_str(simdjson::dom::object& o, const char* key, const std::string& def) {
    std::string_view v;
    if (o[key].get(v) == simdjson::SUCCESS) return std::string(v);
    return def;
}

} // namespace

bool load_headless_config(const std::string& path, HeadlessConfig& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "打不开配置文件: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.empty()) { err = "配置文件是空的: " + path; return false; }

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto padded = simdjson::padded_string(text);
    if (parser.parse(padded).get(doc) != simdjson::SUCCESS) {
        err = "配置文件不是合法 JSON: " + path;
        return false;
    }
    simdjson::dom::object root;
    if (doc.get(root) != simdjson::SUCCESS) {
        err = "配置文件根节点必须是 JSON 对象";
        return false;
    }

    out.api_key           = get_str(root, "api_key", "");
    out.api_secret        = get_str(root, "api_secret", "");
    out.testnet           = get_bool(root, "testnet", false);
    out.account_mode      = get_str(root, "account_mode", "futures");
    out.max_total_margin  = get_num(root, "max_total_margin", 0.0);
    out.max_open_positions = (int)get_num(root, "max_open_positions",
                                          (double)out.max_open_positions);

    if (out.account_mode != "futures" && out.account_mode != "portfolio_margin") {
        err = "account_mode 只能是 \"futures\" 或 \"portfolio_margin\"，收到: " + out.account_mode;
        return false;
    }
    // 统一账户只有主网。配置里两个都写了就以 account_mode 为准并告警，
    // 而不是拿主网的 Key 去打测试网域名然后报一堆看不懂的鉴权错
    if (out.account_mode == "portfolio_margin" && out.testnet) {
        out.testnet = false;
        out.warnings.push_back("统一账户没有测试网，testnet=true 已被忽略（按主网连接）");
    }
    out.alert_webhook     = get_str(root, "alert_webhook", "");
    out.state_path        = get_str(root, "state_path", "ccbot_state.json");
    out.log_path          = get_str(root, "log_path", "");

    if (out.api_key.empty() || out.api_secret.empty()) {
        err = "配置文件缺少 api_key / api_secret";
        return false;
    }

    // ── 旧的 "bots" 数组（网格DCA）：本版起不再支持 ──────────────────────────
    // 必须【显式报出来】而不是静默忽略。静默忽略的后果是：升级后用户的配置文件
    // 原样放着，进程正常启动、日志一切正常，而那些 DCA 品种其实一个都没在跑——
    // 使用者要等到某天去交易所对账才发现。这种沉默比启动失败危险得多
    {
        simdjson::dom::array old_bots;
        if (root["bots"].get(old_bots) == simdjson::SUCCESS) {
            size_t n = 0;
            for (auto e : old_bots) { (void)e; ++n; }
            if (n > 0) {
                out.warnings.push_back(
                    "配置里有 " + std::to_string(n) + " 个 \"bots\"（网格DCA）条目，"
                    "但网格DCA 已在本版整体移除，这些条目【完全不会运行】。"
                    "趋势策略请配在 \"sar_bots\" 里");
            }
        }
    }

    // ── 趋势策略（海龟 / 抛物线SAR / 纯裸K 三选一）──────────────────────────
    static const std::set<std::string> trend_keys = {
        "symbol", "budget_usdt", "leverage", "interval",
        "strategy",
        // ① 海龟
        "donchian_period", "atr_period", "atr_mult",
        // ② 抛物线SAR
        "af_start", "af_step", "af_max",
        // ③ 纯裸K
        "bare_entry", "once_per_bar", "swing_bars",
        // 共享
        "reverse", "max_consecutive_reverses", "cooldown_bars",
        "signal_max_age_sec", "use_disaster_stop", "disaster_stop_buf_dist_pct",
        "size_mode", "risk_usdt",
        // 兼容旧键：仍然识别，但会迁移并显式告警（见下）
        "mode", "allow_reverse", "reverse_needs_signal",
        "disaster_stop_buffer_pct",
    };
    simdjson::dom::array tarr;
    if (root["sar_bots"].get(tarr) == simdjson::SUCCESS) {
        for (auto elem : tarr) {
            simdjson::dom::object so;
            if (elem.get(so) != simdjson::SUCCESS) continue;

            TrendConfig c;
            c.symbol = get_str(so, "symbol", "");
            if (c.symbol.empty()) continue;

            for (auto field : so) {
                std::string k(field.key);
                if (!trend_keys.count(k))
                    out.warnings.push_back(c.symbol + " sar_bots 配置里有无法识别的键 \"" +
                                           k + "\"（拼写错误?），该项被忽略、"
                                           "对应参数使用默认值");
            }

            c.budget_usdt = get_num(so, "budget_usdt", c.budget_usdt);
            c.leverage    = (int)get_num(so, "leverage", c.leverage);
            c.interval    = get_str(so, "interval", c.interval);
            c.signal_max_age_sec = (int)get_num(so, "signal_max_age_sec",
                                                c.signal_max_age_sec);
            // 交易所侧灾难止损：SAR 唯一的进程外保护，强烈建议开
            c.use_disaster_stop = get_bool(so, "use_disaster_stop", c.use_disaster_stop);
            // ⚠ 换过口径：旧键 disaster_stop_buffer_pct 是【占价格】的百分比，
            //   新键 disaster_stop_buf_dist_pct 是【占止损距离】的百分比。
            //   两者都是"一个百分数"，认错了不会报错、只会静默挂出一张几乎
            //   必被交易所抢先触发的单，所以必须换键名而不是改解释。
            //   也【不做数值换算】：旧口径要折成新口径得知道该 bot 当时的
            //   止损距离（运行期 ATR），配置文件里没有这个信息。
            c.disaster_stop_buffer_pct = get_num(so, "disaster_stop_buf_dist_pct",
                                                 c.disaster_stop_buffer_pct);
            simdjson::dom::element probe;
            const bool has_new = (so["disaster_stop_buf_dist_pct"].get(probe)
                                  == simdjson::SUCCESS);
            const bool has_old = (so["disaster_stop_buffer_pct"].get(probe)
                                  == simdjson::SUCCESS);
            if (!has_new && has_old) {
                // 只写了旧键：用新默认值，并且每次启动都说一次。headless 不回写
                // 配置，所以这条告警会一直在——正是想要的，直到人去改配置文件
                out.warnings.push_back(
                    c.symbol + ": disaster_stop_buffer_pct 已废弃（它是占价格的百分比）。"
                    "现在用 disaster_stop_buf_dist_pct，含义是占【止损距离】的百分比，"
                    "本次按默认值 " + std::to_string((int)c.disaster_stop_buffer_pct) +
                    " 运行。两种口径无法换算，请自行确认一次"
                    "（默认参数下 20 与旧的 1 大致等价）");
            }
            if (c.use_disaster_stop && c.disaster_stop_buffer_pct <= 0) {
                // 缓冲为 0 会让交易所抢在本地之前触发，把正常止损变成"外部平仓
                // → 停 bot"。这是一个静默改变行为的配置错误，必须拦
                err = c.symbol + ": disaster_stop_buf_dist_pct 必须大于 0"
                      "（挂在止损线上会让交易所抢先触发，正常出场会变成需要人工介入的事件）";
                return false;
            }

            // ── 选策略 ──────────────────────────────────────────────────────
            // 新键 strategy，旧键 mode 仍然认（带迁移告警）
            std::string strat = get_str(so, "strategy", "");
            if (strat.empty()) {
                const std::string md = get_str(so, "mode", "");
                if (md == "bar" || md == "bar_pattern") {
                    strat = "bare_k";
                    // ⚠ 这不是单纯改名：裸K 的【入场规则本身换了】。
                    //   老规则是"阳线就做多"，新默认是"收盘价突破上一根最高价"。
                    //   后者严格得多——下跌趋势里的一根阳线只是噪音，而突破前根
                    //   高点是真正的动能信号。同一份配置换上来之后开仓频率会
                    //   明显下降。静默迁移的话用户会以为策略"突然不работ了"
                    out.warnings.push_back(
                        c.symbol + "：旧键 mode=bar 已迁移为 strategy=bare_k。"
                        "⚠ 入场规则【同时变了】——从\"阳线做多\"改为\"收盘价突破"
                        "上一根最高价\"，开仓会明显变少（这是有意收紧）。"
                        "想回到盘中即时入场请显式配 bare_entry=immediate");
                } else if (!md.empty()) {
                    strat = "turtle";
                    out.warnings.push_back(
                        c.symbol + "：旧键 mode=" + md + " 已迁移为 strategy=turtle，行为不变");
                } else {
                    strat = "turtle";
                }
            }
            if      (strat == "psar" || strat == "parabolic_sar") c.rule.strategy = trend::Strategy::ParabolicSar;
            else if (strat == "bare_k" || strat == "bare" || strat == "bark") c.rule.strategy = trend::Strategy::BareK;
            else if (strat == "turtle" || strat == "donchian")    c.rule.strategy = trend::Strategy::Turtle;
            else {
                err = c.symbol + "：strategy 只能是 turtle / psar / bare_k，收到 \"" + strat + "\"";
                return false;
            }

            // ── 反手模式 ────────────────────────────────────────────────────
            // 新键 reverse=immediate|none；旧的两个 bool 仍然认（带迁移告警）
            const std::string rv = get_str(so, "reverse", "");
            if (!rv.empty()) {
                if      (rv == "immediate") c.rule.reverse = trend::ReverseMode::Immediate;
                else if (rv == "none")      c.rule.reverse = trend::ReverseMode::None;
                else { err = c.symbol + "：reverse 只能是 immediate / none"; return false; }
            } else if (so["allow_reverse"].error() == simdjson::SUCCESS ||
                       so["reverse_needs_signal"].error() == simdjson::SUCCESS) {
                const bool old_allow = get_bool(so, "allow_reverse", true);
                const bool old_needs = get_bool(so, "reverse_needs_signal", true);
                if (old_allow && !old_needs) {
                    c.rule.reverse = trend::ReverseMode::Immediate;
                    out.warnings.push_back(c.symbol + "：旧的反手配置已迁移为 reverse=immediate，行为不变");
                } else {
                    // ⚠ 行为变了。老的"等反向信号才反手"这一档【已经没有了】：
                    //   映射到 none（平掉回到正常入场流程）是取保护性更强的那边，
                    //   但原来会在反向信号成立时立刻反手，现在要等正常入场信号
                    c.rule.reverse = trend::ReverseMode::None;
                    out.warnings.push_back(
                        c.symbol + "：旧的反手配置（等反向信号才反手）已迁移为 reverse=none。"
                        "⚠ 行为【有变化】——原来反向信号成立时会立刻反手，现在是平掉之后"
                        "走正常入场流程。想要立刻反手请显式配 reverse=immediate");
                }
            }
            c.rule.max_consecutive_reverses =
                (int)get_num(so, "max_consecutive_reverses", c.rule.max_consecutive_reverses);
            c.rule.cooldown_bars = (int)get_num(so, "cooldown_bars", c.rule.cooldown_bars);

            // ── 仓位模式 ────────────────────────────────────────────────────
            // notional=固定名义（默认）；risk=固定单笔风险（数量由真实止损线反推）
            const std::string sm = get_str(so, "size_mode", "notional");
            c.size_mode = (sm == "risk" || sm == "risk_based")
                          ? TrendConfig::SizeMode::RiskBased
                          : TrendConfig::SizeMode::Notional;
            c.risk_usdt = get_num(so, "risk_usdt", c.risk_usdt);
            if (c.size_mode == TrendConfig::SizeMode::RiskBased && c.risk_usdt <= 0) {
                err = c.symbol + " 趋势策略: size_mode=risk 时必须设置 risk_usdt（单笔愿亏金额）";
                return false;
            }

            // ── 各策略自己的参数 ────────────────────────────────────────────
            if (c.rule.strategy == trend::Strategy::Turtle) {
                c.rule.donchian_period = (int)get_num(so, "donchian_period", c.rule.donchian_period);
                c.rule.atr_period      = (int)get_num(so, "atr_period", c.rule.atr_period);
                c.rule.atr_mult        = get_num(so, "atr_mult", c.rule.atr_mult);
                if (c.rule.donchian_period < 2) { err = c.symbol + " 海龟: donchian_period 至少为2"; return false; }
                if (c.rule.atr_period < 2)      { err = c.symbol + " 海龟: atr_period 至少为2";      return false; }
                if (c.rule.atr_mult <= 0) {
                    err = c.symbol + " 海龟: atr_mult 必须大于0（它是止损距离的倍数）";
                    return false;
                }
            } else if (c.rule.strategy == trend::Strategy::ParabolicSar) {
                c.rule.af_start = get_num(so, "af_start", c.rule.af_start);
                c.rule.af_step  = get_num(so, "af_step",  c.rule.af_step);
                c.rule.af_max   = get_num(so, "af_max",   c.rule.af_max);
                if (!(c.rule.af_start > 0) || !(c.rule.af_step > 0) || !(c.rule.af_max > 0)) {
                    err = c.symbol + " 抛物线SAR: af_start / af_step / af_max 都必须大于0";
                    return false;
                }
                if (c.rule.af_start > c.rule.af_max) {
                    err = c.symbol + " 抛物线SAR: af_start 不能大于 af_max（起点就封顶了，加速因子永远不会长）";
                    return false;
                }
                // PSAR 是【无条件翻转】的系统，触及即反手就是它的定义
                c.rule.reverse = trend::ReverseMode::Immediate;
                // 震荡市里 PSAR 会一直翻下去没有刹车，这是它最著名的弱点。
                // 不设上限时明确说一次——不拦，但不能让它悄悄生效
                if (c.rule.max_consecutive_reverses <= 0)
                    out.warnings.push_back(
                        c.symbol + " 抛物线SAR：未设 max_consecutive_reverses（连续反手上限）。"
                        "PSAR 触及即翻转，震荡市里会一直翻下去没有刹车——"
                        "每次往返吃掉约 2k×ATR 的名义再加两笔手续费。建议设 2~3");
            } else {   // BareK
                const std::string be = get_str(so, "bare_entry", "break_prev");
                if      (be == "immediate")  c.rule.bare_entry = trend::BareEntry::Immediate;
                else if (be == "break_prev" || be == "break_prev_bar")
                                             c.rule.bare_entry = trend::BareEntry::BreakPrevBar;
                else { err = c.symbol + " 纯裸K: bare_entry 只能是 immediate / break_prev"; return false; }
                c.rule.once_per_bar = get_bool(so, "once_per_bar", c.rule.once_per_bar);
                c.rule.swing_bars   = (int)get_num(so, "swing_bars", c.rule.swing_bars);
                if (c.rule.swing_bars < 1) { err = c.symbol + " 纯裸K: swing_bars 至少为1"; return false; }
                // 盘中即时 + 立即反手 + 无护栏 = 一根K线内来回开平，纯烧手续费
                if (c.rule.bare_entry == trend::BareEntry::Immediate &&
                    c.rule.reverse == trend::ReverseMode::Immediate && !c.rule.once_per_bar)
                    out.warnings.push_back(
                        c.symbol + " 纯裸K：盘中即时入场 + 立即反手 + 未开每根一次护栏。"
                        "新K线刚开盘时价格≈开盘价，微小波动会让多空条件反复翻转，"
                        "一根K线内可能来回开平数次，每次两笔手续费。"
                        "建议开 once_per_bar");
            }

            out.sar_bots.push_back(c);
        }
    }

    // v4.7.1 之前这里还要拦"同一品种被 bots 与 sar_bots 同时接管"（两个引擎各下
    // 各的单、互相平掉对方的仓）。网格DCA 移除后只剩一套策略，这类冲突不存在了。

    if (out.sar_bots.empty()) {
        err = "配置文件里 sar_bots 为空（或每一项都缺少 symbol），至少要配一个";
        return false;
    }

    return true;
}

} // namespace ccbot
