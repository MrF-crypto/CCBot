#include "core/funding_ledger.h"
#include "net/trading_client.h"
#include <simdjson.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

namespace ccbot {

void FundingLedger::apply(const std::string& symbol, double income, int64_t time_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& e = map_[symbol];
    // 分页窗口有重叠时同一条流水会被拉两次。同品种一次结算只产生一条记录，
    // 所以"时间戳不大于游标"就是重复，直接丢弃——不需要额外的 tranId 去重表
    if (time_ms <= e.cursor_ms) return;
    e.total      += income;
    e.since_open += income;
    e.cursor_ms   = time_ms;
}

void FundingLedger::reset_position(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    if (it != map_.end()) it->second.since_open = 0;
}

void FundingLedger::set_rate(const std::string& symbol, double rate,
                             int64_t next_ms, int64_t now_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& e = map_[symbol];
    e.rate    = rate;
    e.next_ms = next_ms;
    e.rate_ms = now_ms;
}

FundingLedger::Entry FundingLedger::get(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    return it == map_.end() ? Entry{} : it->second;
}

int64_t FundingLedger::cursor(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    return it == map_.end() ? 0 : it->second.cursor_ms;
}

std::vector<std::string> FundingLedger::symbols() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) out.push_back(k);
    return out;
}

bool FundingLedger::save(const std::string& path) const {
    std::ostringstream ss;
    ss << std::setprecision(12);
    ss << "{";
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bool first = true;
        for (const auto& [sym, e] : map_) {
            if (!first) ss << ",";
            first = false;
            ss << "\"" << sym << "\":{"
               << "\"total\":"      << e.total      << ","
               << "\"since_open\":" << e.since_open << ","
               << "\"cursor_ms\":"  << e.cursor_ms  << "}";
        }
    }
    ss << "}";

    // 原子写：崩在写文件中途会留下半截 JSON，重启后整本账丢失
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << ss.str();
        f.flush();
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool FundingLedger::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto padded = simdjson::padded_string(text);
    if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return false;
    simdjson::dom::object root;
    if (doc.get(root) != simdjson::SUCCESS) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    for (auto field : root) {
        simdjson::dom::object o;
        if (field.value.get(o) != simdjson::SUCCESS) continue;
        Entry e;
        double d = 0;
        int64_t i = 0;
        if (o["total"].get(d)      == simdjson::SUCCESS) e.total      = d;
        if (o["since_open"].get(d) == simdjson::SUCCESS) e.since_open = d;
        if (o["cursor_ms"].get(i)  == simdjson::SUCCESS) e.cursor_ms  = i;
        map_[std::string(field.key)] = e;
    }
    return true;
}

// ── 同步 ─────────────────────────────────────────────────────────────────────
int sync_funding_ledger(TradingClient& client, FundingLedger& ledger,
                        const std::vector<std::string>& symbols,
                        int64_t backfill_from_ms,
                        int max_windows) {
    using namespace std::chrono;
    const int64_t now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    // 币安 income 接口的单次时间跨度上限是 7 天，超了直接报错。留 1 小时余量
    const int64_t kWindow = 7LL * 24 * 3600 * 1000 - 3600LL * 1000;

    int applied = 0;
    for (const auto& sym : symbols) {
        int64_t from = ledger.cursor(sym);
        if (from > 0) {
            from += 1;                       // 游标那条已经记过
        } else if (backfill_from_ms > 0) {
            from = backfill_from_ms;         // 首次：从最早持仓时间补
        } else {
            from = now_ms - kWindow;         // 没有历史锚点就只看最近一窗
        }
        if (from >= now_ms) continue;

        // 按 7 天窗口往前推。max_windows 是硬上限，防止把 from 设成 2017 年
        // 之后发出上百个请求——超出时只是少补一段历史，不影响增量正确性
        for (int w = 0; w < max_windows && from < now_ms; ++w) {
            const int64_t to = std::min(from + kWindow, now_ms);
            auto recs = client.fetch_funding_income(from, to, sym);
            for (const auto& r : recs) {
                if (r.symbol != sym) continue;   // 保险：接口理论上已按品种过滤
                ledger.apply(r.symbol, r.income, r.time);
                ++applied;
            }
            from = to + 1;
        }
    }
    return applied;
}

} // namespace ccbot
