#include "headless/sar_state.h"

#include <simdjson.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

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

double get_num(simdjson::dom::object& o, const char* key, double def) {
    simdjson::dom::element val;
    if (o[key].get(val) != simdjson::SUCCESS) return def;
    double d;
    if (val.get(d) == simdjson::SUCCESS) return d;
    int64_t i;
    if (val.get(i) == simdjson::SUCCESS) return (double)i;
    uint64_t u;
    if (val.get(u) == simdjson::SUCCESS) return (double)u;
    return def;
}
int64_t get_i64(simdjson::dom::object& o, const char* key, int64_t def) {
    int64_t i;
    if (o[key].get(i) == simdjson::SUCCESS) return i;
    return def;
}
std::string get_str(simdjson::dom::object& o, const char* key, const std::string& def) {
    std::string_view v;
    if (o[key].get(v) == simdjson::SUCCESS) return std::string(v);
    return def;
}

} // namespace

void save_sar_state(const std::string& path, const std::vector<SarBot>& bots) {
    std::ostringstream ss;
    // ⚠ 必须设精度。ostringstream 默认 6 位有效数字，而这里存的是开仓价、
    //   止损线、持仓量。截断的后果在 SAR 上比 DCA 更直接：止损线【就是】出场价，
    //   94310.4912 存成 94310.5 等于每次重启都把出场价挪一下。
    //   max_digits10=17 是"任意 double 无损往返"的最小位数
    ss << std::setprecision(std::numeric_limits<double>::max_digits10);
    ss << "[";
    bool first = true;
    for (const auto& b : bots) {
        if (!first) ss << ",";
        first = false;
        ss << "{"
           << "\"symbol\":\""    << json_escape(b.cfg.symbol) << "\","
           << "\"state\":"       << (int)b.state << ","
           << "\"pos\":"         << (int)b.st.pos << ","
           << "\"entry_price\":" << b.st.entry_price << ","
           << "\"peak\":"        << b.st.peak << ","
           << "\"stop\":"        << b.st.stop << ","
           << "\"consec_reverses\":" << b.st.consec_reverses << ","
           << "\"cooldown_left\":"   << b.st.cooldown_left << ","
           << "\"qty\":"          << b.qty << ","
           << "\"current_price\":" << b.current_price << ","
           << "\"realized_pnl\":" << b.realized_pnl << ","
           << "\"trade_count\":"  << b.trade_count << ","
           << "\"win_count\":"    << b.win_count << ","
           << "\"start_time_ms\":" << tp_to_ms(b.start_time)
           << "}";
    }
    ss << "]";

    // 原子写盘：先写临时文件再改名。直接覆盖写的话，进程崩在写文件中途会留下
    // 半截 JSON，重启后仓位跟踪静默丢失——而丢的正是那条止损线
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << ss.str();
        f.flush();
        if (!f.good()) return;   // 磁盘满等，保留旧文件不动
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) f << ss.str();
        std::filesystem::remove(tmp, ec);
    }
}

std::vector<SarBot> load_sar_state(const std::string& path,
                                   const std::vector<SarConfig>& cfgs) {
    std::vector<SarBot> out;

    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string raw = buf.str();
    if (raw.empty()) return out;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(raw);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return out;

    for (auto elem : arr) {
        simdjson::dom::object o;
        if (elem.get(o) != simdjson::SUCCESS) continue;

        const std::string sym = get_str(o, "symbol", "");
        if (sym.empty()) continue;

        // 配置里已经删掉的品种，落盘状态直接丢弃
        const SarConfig* cfg = nullptr;
        for (const auto& c : cfgs) if (c.symbol == sym) { cfg = &c; break; }
        if (!cfg) continue;

        SarBot b;
        b.cfg   = *cfg;                       // 参数用最新配置，不用落盘的
        b.state = (SarBot::State)(int)get_num(o, "state", 0);

        const int pos = (int)get_num(o, "pos", (double)(int)sar::Pos::Flat);
        b.st.pos = (pos == (int)sar::Pos::Long)  ? sar::Pos::Long
                 : (pos == (int)sar::Pos::Short) ? sar::Pos::Short
                                                 : sar::Pos::Flat;
        b.st.entry_price     = get_num(o, "entry_price", 0);
        b.st.peak            = get_num(o, "peak", 0);
        b.st.stop            = get_num(o, "stop", 0);
        b.st.consec_reverses = (int)get_num(o, "consec_reverses", 0);
        b.st.cooldown_left   = (int)get_num(o, "cooldown_left", 0);
        b.qty           = get_num(o, "qty", 0);
        b.current_price = get_num(o, "current_price", 0);
        b.realized_pnl  = get_num(o, "realized_pnl", 0);
        b.trade_count   = (int)get_num(o, "trade_count", 0);
        b.win_count     = (int)get_num(o, "win_count", 0);
        b.start_time    = ms_to_tp(get_i64(o, "start_time_ms", 0));

        // 半截状态防御：有方向却没有开仓价或没有止损线，说明落盘写到一半或被
        // 人改过。这种状态比"空仓"危险得多——止损线为 0 时，多头的
        // `price <= stop` 永远不成立，仓位会一直裸着没人管。
        // 当成空仓丢弃，交给对账去发现交易所上那笔真实仓位并停掉 bot
        if (b.st.pos != sar::Pos::Flat &&
            (b.st.entry_price <= 0 || b.st.stop <= 0 || b.qty <= 0)) {
            b.st = sar::State{};
            b.qty = 0;
        }

        out.push_back(std::move(b));
    }
    return out;
}

} // namespace ccbot
