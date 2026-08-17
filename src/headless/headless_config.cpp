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

CcgConfig::StratType parse_strat(const std::string& s) {
    if (s == "flat")       return CcgConfig::StratType::Flat;
    if (s == "martingale") return CcgConfig::StratType::Martingale;
    if (s == "mart_plus")  return CcgConfig::StratType::MartPlus;
    if (s == "triple")     return CcgConfig::StratType::Triple;
    if (s == "square")     return CcgConfig::StratType::Square;
    if (s == "fibonacci")  return CcgConfig::StratType::Fibonacci;
    if (s == "lucas")      return CcgConfig::StratType::Lucas;
    if (s == "linear")     return CcgConfig::StratType::Linear;
    return CcgConfig::StratType::Martingale;
}

CcgConfig::Direction parse_dir(const std::string& s) {
    if (s == "short") return CcgConfig::Direction::Short;
    if (s == "both")  return CcgConfig::Direction::Both;
    return CcgConfig::Direction::Long;
}

CcgConfig::EntryMode parse_entry_mode(const std::string& s) {
    return (s == "indicator") ? CcgConfig::EntryMode::Indicator : CcgConfig::EntryMode::Immediate;
}

CcgConfig::RsiConfirmMode parse_rsi_mode(const std::string& s) {
    return (s == "cross") ? CcgConfig::RsiConfirmMode::CrossFromOversold
                           : CcgConfig::RsiConfirmMode::Snapshot;
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

    simdjson::dom::array bots;
    if (root["bots"].get(bots) != simdjson::SUCCESS) {
        err = "配置文件缺少 bots 数组";
        return false;
    }

    // 已知的 bot 配置键：拼错键名会静默落回默认值（比如 stop_loss_pct 拼错 = 没有止损），
    // 所以未知键必须显式告警
    static const std::set<std::string> known_keys = {
        "symbol", "direction", "strat_type", "budget_usdt", "leverage", "max_entries",
        "interval_pct", "trail_entry", "tp_pct", "trail_tp", "auto_restart",
        "cooldown_secs", "stop_loss_pct", "use_disaster_stop", "disaster_stop_pct",
        "entry_mode", "kline_interval",
        "boll_period", "boll_mult", "use_rsi_filter", "rsi_period", "rsi_threshold",
        "rsi_confirm_mode", "rsi_oversold_th", "dynamic_band_mode", "min_profit_floor",
        "mtf_ladder", "mtf_tier_layers", "mtf_k", "mtf_min_gap_pct",
        "use_trend_filter", "trend_interval", "trend_ema_period", "sr_radar", "sr_interval",
        "smart_gates", "use_htf_filter", "htf_interval", "htf_pos_max", "use_sr_gate",
        "sr_min_confluence", "sr_headroom_ratio", "use_sr_exit", "use_structural_stop",
        "sr_independent_conf", "sr_lower_half_only",
    };

    for (auto elem : bots) {
        simdjson::dom::object bo;
        if (elem.get(bo) != simdjson::SUCCESS) continue;

        CcgConfig c;
        c.symbol = get_str(bo, "symbol", "");
        if (c.symbol.empty()) continue;

        for (auto field : bo) {
            std::string k(field.key);
            if (!known_keys.count(k))
                out.warnings.push_back(c.symbol + " 配置里有无法识别的键 \"" + k +
                                       "\"（拼写错误?），该项被忽略、对应参数使用默认值");
        }

        std::string dir_s = get_str(bo, "direction", "long");
        if (dir_s == "both") {
            // 引擎内部 Both 会走纯空头分支，headless 又没有 GUI 那样的拆分逻辑——
            // 用户想要对冲、实际得到裸空单，必须拒绝启动
            err = c.symbol + " 配置了 direction=both：headless 不支持双向，"
                  "请拆成两个 bot 分别配置 long 和 short（注意需要币安双向持仓模式）";
            return false;
        }

        c.strat_type    = parse_strat(get_str(bo, "strat_type", "linear"));
        c.direction     = parse_dir(dir_s);
        c.budget_usdt   = get_num(bo, "budget_usdt", 3000.0);
        c.leverage      = (int)get_num(bo, "leverage", 3);
        c.max_entries   = (int)get_num(bo, "max_entries", 7);
        c.interval_pct  = get_num(bo, "interval_pct", 8.0);
        c.trail_entry   = get_num(bo, "trail_entry", 1.0);
        c.tp_pct        = get_num(bo, "tp_pct", 5.0);
        c.trail_tp      = get_num(bo, "trail_tp", 2.0);
        c.auto_restart  = get_bool(bo, "auto_restart", true);
        c.cooldown_secs = (int)get_num(bo, "cooldown_secs", 300.0);
        c.stop_loss_pct = get_num(bo, "stop_loss_pct", 0.0);
        c.use_disaster_stop = get_bool(bo, "use_disaster_stop", false);
        c.disaster_stop_pct = get_num(bo, "disaster_stop_pct", 30.0);
        // 配了比例却没打开开关是最容易犯的错——它会静默地什么都不做，
        // 而使用者以为仓位已经有进程外保护了
        if (!c.use_disaster_stop && bo["disaster_stop_pct"].error() == simdjson::SUCCESS)
            out.warnings.push_back(c.symbol + " 配了 disaster_stop_pct 但 use_disaster_stop 不是 true，"
                                              "交易所侧灾难止损单【未启用】");

        c.entry_mode      = parse_entry_mode(get_str(bo, "entry_mode", "indicator"));
        c.kline_interval  = get_str(bo, "kline_interval", "1h");
        c.boll_period     = (int)get_num(bo, "boll_period", 20.0);
        c.boll_mult       = get_num(bo, "boll_mult", 2.0);
        c.use_rsi_filter  = get_bool(bo, "use_rsi_filter", true);
        c.rsi_period      = (int)get_num(bo, "rsi_period", 14.0);
        c.rsi_threshold   = get_num(bo, "rsi_threshold", 30.0);
        c.rsi_confirm_mode = parse_rsi_mode(get_str(bo, "rsi_confirm_mode", "cross"));
        c.rsi_oversold_th  = get_num(bo, "rsi_oversold_th", 25.0);
        c.dynamic_band_mode = get_bool(bo, "dynamic_band_mode", true);
        c.min_profit_floor  = get_num(bo, "min_profit_floor", 3.5);
        c.mtf_ladder        = get_bool(bo, "mtf_ladder", false);
        c.mtf_tier_layers   = get_str(bo, "mtf_tier_layers", "");
        c.mtf_k             = get_num(bo, "mtf_k", 0.5);
        c.mtf_min_gap_pct   = get_num(bo, "mtf_min_gap_pct", 2.0);
        c.use_trend_filter  = get_bool(bo, "use_trend_filter", true);
        c.trend_interval    = get_str(bo, "trend_interval", "4h");
        c.trend_ema_period  = (int)get_num(bo, "trend_ema_period", 200.0);
        c.sr_radar          = get_bool(bo, "sr_radar", true);
        c.sr_interval       = get_str(bo, "sr_interval", "4h");
        c.smart_gates         = get_bool(bo, "smart_gates", true);
        c.use_htf_filter      = get_bool(bo, "use_htf_filter", true);
        c.htf_interval        = get_str(bo, "htf_interval", "1d");
        c.htf_pos_max         = get_num(bo, "htf_pos_max", 0.60);
        c.use_sr_gate         = get_bool(bo, "use_sr_gate", true);
        c.sr_min_confluence   = (int)get_num(bo, "sr_min_confluence", 2.0);
        c.sr_independent_conf = get_bool(bo, "sr_independent_conf", true);
        c.sr_lower_half_only  = get_bool(bo, "sr_lower_half_only", false);
        c.sr_headroom_ratio   = get_num(bo, "sr_headroom_ratio", 3.0);
        c.use_sr_exit         = get_bool(bo, "use_sr_exit", false);
        c.use_structural_stop = get_bool(bo, "use_structural_stop", false);

        out.bots.push_back(c);
    }

    if (out.bots.empty()) {
        err = "配置文件 bots 数组为空或每一项都缺少 symbol";
        return false;
    }

    return true;
}

} // namespace ccbot
