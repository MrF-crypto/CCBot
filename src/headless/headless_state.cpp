#include "headless/headless_state.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>
#include <chrono>

namespace ccbot {

namespace {

std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n";  break;
        case '\r': break;
        default:   o << c;
        }
    }
    return o.str();
}

int64_t tp_to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point ms_to_tp(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

double get_num(simdjson::dom::element val, double def) {
    double d;
    if (val.get(d) == simdjson::SUCCESS) return d;
    int64_t i;
    if (val.get(i) == simdjson::SUCCESS) return (double)i;
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

void save_headless_state(const std::string& path, const std::vector<CcgBot>& bots) {
    std::ostringstream ss;
    ss << "[";
    bool first_bot = true;
    for (const auto& b : bots) {
        if (!first_bot) ss << ",";
        first_bot = false;
        ss << "{"
           << "\"symbol\":\""      << json_escape(b.cfg.symbol) << "\","
           << "\"direction\":"     << (int)b.cfg.direction << ","
           << "\"state\":"         << (int)b.state << ","
           << "\"avg_price\":"     << b.avg_price << ","
           << "\"total_qty\":"     << b.total_qty << ","
           << "\"total_cost\":"    << b.total_cost << ","
           << "\"current_price\":" << b.current_price << ","
           << "\"last_entry_price\":" << b.last_entry_price << ","
           << "\"dca_extreme\":"   << b.dca_extreme << ","
           << "\"interval_hit\":"  << (b.interval_hit ? "true" : "false") << ","
           << "\"tp_reached\":"    << (b.tp_reached ? "true" : "false") << ","
           << "\"tp_extreme\":"    << b.tp_extreme << ","
           << "\"realized_pnl\":"  << b.realized_pnl << ","
           << "\"cycle_count\":"   << b.cycle_count << ","
           << "\"cooldown_until_ms\":" << tp_to_ms(b.cooldown_until) << ","
           << "\"entries\":[";
        bool first_e = true;
        for (const auto& e : b.entries) {
            if (!first_e) ss << ",";
            first_e = false;
            ss << "{"
               << "\"level\":"     << e.level << ","
               << "\"price\":"     << e.price << ","
               << "\"qty\":"       << e.qty << ","
               << "\"cost_usdt\":" << e.cost_usdt << ","
               << "\"order_id\":\"" << json_escape(e.order_id) << "\","
               << "\"time_ms\":"   << tp_to_ms(e.time)
               << "}";
        }
        ss << "]}";
    }
    ss << "]";

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << ss.str();
}

std::vector<CcgBot> load_headless_state(const std::string& path, const std::vector<CcgConfig>& cfgs) {
    std::vector<CcgBot> out;

    std::ifstream f(path, std::ios::binary);
    if (!f) return out;   // 没有落盘文件很正常（第一次启动），直接返回空

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return out;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto padded = simdjson::padded_string(text);
    if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return out;
    simdjson::dom::array arr;
    if (doc.get(arr) != simdjson::SUCCESS) return out;

    for (auto elem : arr) {
        simdjson::dom::object o;
        if (elem.get(o) != simdjson::SUCCESS) continue;

        std::string symbol = get_str(o, "symbol", "");
        auto dir = (CcgConfig::Direction)(int)get_num(o, "direction", 0.0);

        // 拿配置文件里当前这一版的策略参数（可能跟落盘时不一样，比如用户改过配置）
        const CcgConfig* cfg = nullptr;
        for (const auto& c : cfgs) {
            if (c.symbol == symbol && c.direction == dir) { cfg = &c; break; }
        }
        if (!cfg) continue;   // 配置文件里已经删掉这个品种/方向了，丢弃落盘状态

        CcgBot bot;
        bot.cfg              = *cfg;
        bot.state             = (CcgBot::State)(int)get_num(o, "state", (double)(int)CcgBot::State::Running);
        bot.avg_price         = get_num(o, "avg_price", 0.0);
        bot.total_qty         = get_num(o, "total_qty", 0.0);
        bot.total_cost        = get_num(o, "total_cost", 0.0);
        bot.current_price     = get_num(o, "current_price", 0.0);
        bot.last_entry_price  = get_num(o, "last_entry_price", 0.0);
        bot.dca_extreme       = get_num(o, "dca_extreme", 0.0);
        bot.interval_hit      = get_bool(o, "interval_hit", false);
        bot.tp_reached        = get_bool(o, "tp_reached", false);
        bot.tp_extreme        = get_num(o, "tp_extreme", 0.0);
        bot.realized_pnl      = get_num(o, "realized_pnl", 0.0);
        bot.cycle_count       = (int)get_num(o, "cycle_count", 0.0);
        bot.cooldown_until    = ms_to_tp((int64_t)get_num(o, "cooldown_until_ms", 0.0));

        simdjson::dom::array entries;
        if (o["entries"].get(entries) == simdjson::SUCCESS) {
            for (auto ee : entries) {
                simdjson::dom::object eo;
                if (ee.get(eo) != simdjson::SUCCESS) continue;
                CcgEntry e;
                e.level     = (int)get_num(eo, "level", 0.0);
                e.price     = get_num(eo, "price", 0.0);
                e.qty       = get_num(eo, "qty", 0.0);
                e.cost_usdt = get_num(eo, "cost_usdt", 0.0);
                e.order_id  = get_str(eo, "order_id", "");
                e.time      = ms_to_tp((int64_t)get_num(eo, "time_ms", 0.0));
                bot.entries.push_back(e);
            }
        }

        out.push_back(bot);
    }

    return out;
}

} // namespace ccbot
