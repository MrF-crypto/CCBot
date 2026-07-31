#include "headless/headless_config.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>

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
    out.max_total_margin  = get_num(root, "max_total_margin", 0.0);
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

    for (auto elem : bots) {
        simdjson::dom::object bo;
        if (elem.get(bo) != simdjson::SUCCESS) continue;

        CcgConfig c;
        c.symbol = get_str(bo, "symbol", "");
        if (c.symbol.empty()) continue;

        c.strat_type    = parse_strat(get_str(bo, "strat_type", "martingale"));
        c.direction     = parse_dir(get_str(bo, "direction", "long"));
        c.budget_usdt   = get_num(bo, "budget_usdt", 3000.0);
        c.leverage      = (int)get_num(bo, "leverage", 3);
        c.max_entries   = (int)get_num(bo, "max_entries", 6);
        c.interval_pct  = get_num(bo, "interval_pct", 8.0);
        c.trail_entry   = get_num(bo, "trail_entry", 1.0);
        c.tp_pct        = get_num(bo, "tp_pct", 5.0);
        c.trail_tp      = get_num(bo, "trail_tp", 2.0);
        c.auto_restart  = get_bool(bo, "auto_restart", true);
        c.cooldown_secs = (int)get_num(bo, "cooldown_secs", 60.0);
        c.stop_loss_pct = get_num(bo, "stop_loss_pct", 0.0);

        c.entry_mode      = parse_entry_mode(get_str(bo, "entry_mode", "immediate"));
        c.kline_interval  = get_str(bo, "kline_interval", "1h");
        c.boll_period     = (int)get_num(bo, "boll_period", 20.0);
        c.boll_mult       = get_num(bo, "boll_mult", 2.0);
        c.use_rsi_filter  = get_bool(bo, "use_rsi_filter", true);
        c.rsi_period      = (int)get_num(bo, "rsi_period", 14.0);
        c.rsi_threshold   = get_num(bo, "rsi_threshold", 30.0);
        c.rsi_confirm_mode = parse_rsi_mode(get_str(bo, "rsi_confirm_mode", "snapshot"));
        c.rsi_oversold_th  = get_num(bo, "rsi_oversold_th", 25.0);
        c.dynamic_band_mode = get_bool(bo, "dynamic_band_mode", false);
        c.min_profit_floor  = get_num(bo, "min_profit_floor", 0.3);

        out.bots.push_back(c);
    }

    if (out.bots.empty()) {
        err = "配置文件 bots 数组为空或每一项都缺少 symbol";
        return false;
    }

    return true;
}

} // namespace ccbot
