#include "headless/headless_config.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>
#include <set>

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

    // ── SAR 趋势跟随策略 ────────────────────────────────────────────────────
    static const std::set<std::string> sar_keys = {
        "symbol", "budget_usdt", "leverage", "interval",
        "donchian_period", "atr_period", "atr_mult",
        "allow_reverse", "reverse_needs_signal",
        "max_consecutive_reverses", "cooldown_bars", "signal_max_age_sec",
        "use_disaster_stop", "disaster_stop_buffer_pct",
        "size_mode", "risk_usdt", "pyramid_max_adds", "pyramid_step_atr",
        "mode", "swing_bars",
    };
    simdjson::dom::array sarr;
    if (root["sar_bots"].get(sarr) == simdjson::SUCCESS) {
        for (auto elem : sarr) {
            simdjson::dom::object so;
            if (elem.get(so) != simdjson::SUCCESS) continue;

            SarConfig c;
            c.symbol = get_str(so, "symbol", "");
            if (c.symbol.empty()) continue;

            for (auto field : so) {
                std::string k(field.key);
                if (!sar_keys.count(k))
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
            c.disaster_stop_buffer_pct = get_num(so, "disaster_stop_buffer_pct",
                                                 c.disaster_stop_buffer_pct);
            if (c.use_disaster_stop && c.disaster_stop_buffer_pct <= 0) {
                // 缓冲为 0 会让交易所抢在本地之前触发，把正常止损变成"外部平仓
                // → 停 bot"。这是一个静默改变行为的配置错误，必须拦
                err = c.symbol + ": disaster_stop_buffer_pct 必须大于 0"
                      "（挂在止损线上会让交易所抢先触发，正常出场会变成需要人工介入的事件）";
                return false;
            }

            c.rule.donchian_period = (int)get_num(so, "donchian_period",
                                                  c.rule.donchian_period);
            c.rule.atr_period      = (int)get_num(so, "atr_period", c.rule.atr_period);
            c.rule.atr_mult        = get_num(so, "atr_mult", c.rule.atr_mult);
            c.rule.allow_reverse   = get_bool(so, "allow_reverse", c.rule.allow_reverse);
            c.rule.reverse_needs_signal =
                get_bool(so, "reverse_needs_signal", c.rule.reverse_needs_signal);
            c.rule.max_consecutive_reverses =
                (int)get_num(so, "max_consecutive_reverses",
                             c.rule.max_consecutive_reverses);
            c.rule.cooldown_bars = (int)get_num(so, "cooldown_bars",
                                                c.rule.cooldown_bars);

            // 仓位模式：notional=固定名义（默认）；risk=按 ATR 等风险
            const std::string sm = get_str(so, "size_mode", "notional");
            c.size_mode = (sm == "risk" || sm == "risk_based")
                          ? SarConfig::SizeMode::RiskBased
                          : SarConfig::SizeMode::Notional;
            c.risk_usdt = get_num(so, "risk_usdt", c.risk_usdt);
            c.rule.pyramid_max_adds = (int)get_num(so, "pyramid_max_adds",
                                                   c.rule.pyramid_max_adds);
            c.rule.pyramid_step_atr = get_num(so, "pyramid_step_atr",
                                              c.rule.pyramid_step_atr);

            // 入场/止损算法：donchian（默认，海龟）或 bar（裸K线）
            const std::string md = get_str(so, "mode", "donchian");
            c.rule.mode = (md == "bar" || md == "bar_pattern")
                          ? sar::Mode::BarPattern : sar::Mode::Donchian;
            c.rule.swing_bars = (int)get_num(so, "swing_bars", c.rule.swing_bars);
            if (c.rule.mode == sar::Mode::BarPattern && c.rule.swing_bars < 1) {
                err = c.symbol + " sar: swing_bars 至少为1";
                return false;
            }

            if (c.size_mode == SarConfig::SizeMode::RiskBased && c.risk_usdt <= 0) {
                err = c.symbol + " sar: size_mode=risk 时必须设置 risk_usdt（单次愿亏金额）";
                return false;
            }
            if (c.rule.pyramid_max_adds > 0 && c.rule.pyramid_step_atr <= 0) {
                err = c.symbol + " sar: pyramid_step_atr 必须大于0，否则会在同一价位无限加仓";
                return false;
            }

            if (c.budget_usdt <= 0) { err = c.symbol + " sar: budget_usdt 必须大于0"; return false; }
            if (c.leverage    <= 0) { err = c.symbol + " sar: leverage 必须大于0";    return false; }
            if (c.rule.donchian_period < 2) {
                err = c.symbol + " sar: donchian_period 至少为2"; return false;
            }
            if (c.rule.atr_period < 2) {
                err = c.symbol + " sar: atr_period 至少为2"; return false;
            }
            if (c.rule.atr_mult <= 0) {
                err = c.symbol + " sar: atr_mult 必须大于0（它是止损距离的倍数）";
                return false;
            }
            // 无条件反手在震荡市是绞肉机，配置里关掉信号闸时明确告警一次
            if (c.rule.allow_reverse && !c.rule.reverse_needs_signal)
                out.warnings.push_back(c.symbol + " sar: reverse_needs_signal=false "
                                       "（无条件反手）——震荡市里每次止损都会立刻反向"
                                       "开仓，连续绞杀的损耗很快，确认这是你要的");

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
