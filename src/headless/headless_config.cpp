#include "headless/headless_config.h"
#include "core/app_logic.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>
#include <set>
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
                    "趋势策略请配在 \"trend_bots\" 里");
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
        "signal_max_age_sec", "use_disaster_stop", "disaster_stop_buffer_pct",
        "size_mode", "risk_usdt",
        // 兼容旧键：仍然识别，但会迁移并显式告警（见下）
        "mode", "allow_reverse", "reverse_needs_signal",
    };
    // ── 键名：trend_bots（v5.9.9 起）与 sar_bots（旧）──────────────────────
    // 引擎跑三个策略，而旧键叫 sar_bots —— 配海龟的人会觉得自己配错了地方。
    // 但旧键是【对外契约】：改名会让在跑的部署"进程正常启动、一个品种都不跑"，
    // 所以旧键继续有效，只是提示一句。
    // ⚠ 两个键同时出现必须报错，不能合并也不能任选一个：合并会让同一品种被
    //   配两次，任选一个会让另一半配置【静默不运行】—— 正是上面那段要防的事
    simdjson::dom::array tarr;
    simdjson::dom::array tarr_new, tarr_old;
    const bool has_new = (root["trend_bots"].get(tarr_new) == simdjson::SUCCESS);
    const bool has_old = (root["sar_bots"].get(tarr_old) == simdjson::SUCCESS);
    if (has_new && has_old) {
        err = "配置里同时有 \"trend_bots\" 和 \"sar_bots\"，不知道该跑哪一份。"
              "\"sar_bots\" 是旧键名，请把它的内容并进 \"trend_bots\" 后删掉它";
        return false;
    }
    if (has_old)
        out.warnings.push_back("\"sar_bots\" 是旧键名，仍然有效；引擎跑的是三个趋势策略"
                               "而不只是 SAR，建议改名为 \"trend_bots\"");
    bool have_arr = false;
    if (has_new)      { tarr = tarr_new; have_arr = true; }
    else if (has_old) { tarr = tarr_old; have_arr = true; }
    if (have_arr) {
        for (auto elem : tarr) {
            simdjson::dom::object so;
            if (elem.get(so) != simdjson::SUCCESS) continue;

            TrendConfig c;
            // 品种名规范化与 GUI 共用一套规则（app::normalize_symbol）。
            // ⚠ v5.9.9 之前 headless 完全不处理：手写成 "BTC" 或 "btcusdt" 的条目
            //   永远拉不到 K 线、永远"等信号"，而进程照常运行 —— 静默失效。
            //   GUI 那边从 v4.1.1 起就会补全并告警，两边不对称
            {
                const auto fx = app::normalize_symbol(get_str(so, "symbol", ""));
                c.symbol = fx.symbol;
                if (fx.suffixed)
                    out.warnings.push_back("品种 \"" + get_str(so, "symbol", "") +
                                           "\" 缺少报价币后缀，已按 " + c.symbol +
                                           " 处理（币安合约的代码形如 BTCUSDT）");
            }
            if (c.symbol.empty()) continue;

            for (auto field : so) {
                std::string k(field.key);
                if (!trend_keys.count(k))
                    out.warnings.push_back(c.symbol + " 的策略配置里有无法识别的键 \"" +
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
                // 规则与文案都在 app::migrate_reverse_mode，GUI 用的是同一个。
                // ⚠ v5.9.9 之前这里自己写了一份，而且和 GUI 不一致：allow_reverse=false
                //   时也报"⚠ 行为有变化——原来反向信号成立时会立刻反手"，可那个配置
                //   原来根本不反手；同一种迁移 GUI 说"几乎不变"、这里说"有变化"
                const auto m = app::migrate_reverse_mode(
                    get_bool(so, "allow_reverse", true),
                    get_bool(so, "reverse_needs_signal", true));
                c.rule.reverse = m.mode;
                if (m.effect == app::MigrationEffect::Changed)
                    out.warnings.push_back(c.symbol + "：" + m.note);
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

            out.trend_bots.push_back(c);
        }
    }

    // v4.7.1 之前这里还要拦"同一品种被 bots 与 sar_bots 同时接管"（两个引擎各下
    // 各的单、互相平掉对方的仓）。网格DCA 移除后只剩一套策略，这类冲突不存在了。

    if (out.trend_bots.empty()) {
        err = "配置文件里 trend_bots 为空（或每一项都缺少 symbol），至少要配一个";
        return false;
    }

    return true;
}

} // namespace ccbot
