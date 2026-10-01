#include "core/fee_ledger.h"
#include "net/trading_client.h"
#include <simdjson.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ccbot {

// ── 时间段 ───────────────────────────────────────────────────────────────────
void FeeLedger::open_span(const std::string& symbol, int64_t from_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& e = map_[symbol];
    for (const auto& s : e.spans) if (s.to_ms == 0) return;
    e.spans.push_back({from_ms, 0});
}

void FeeLedger::close_span(const std::string& symbol, int64_t at_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    if (it == map_.end()) return;
    for (auto& s : it->second.spans)
        if (s.to_ms == 0) s.to_ms = std::max(at_ms, s.from_ms);
}

void FeeLedger::add_closed_span(const std::string& symbol, int64_t from_ms, int64_t to_ms) {
    if (to_ms < from_ms) return;
    std::lock_guard<std::mutex> lk(mtx_);
    map_[symbol].spans.push_back({from_ms, to_ms});
}

bool FeeLedger::reconcile_active(const std::set<std::string>& active, int64_t now_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    bool changed = false;
    for (const auto& sym : active) {
        auto& e = map_[sym];
        bool open = false;
        for (const auto& s : e.spans) if (s.to_ms == 0) { open = true; break; }
        if (!open) { e.spans.push_back({now_ms, 0}); changed = true; }
    }
    for (auto& [sym, e] : map_) {
        if (active.count(sym)) continue;
        for (auto& s : e.spans)
            if (s.to_ms == 0) { s.to_ms = std::max(now_ms, s.from_ms); changed = true; }
    }
    return changed;
}

bool FeeLedger::covers_locked(const Entry& e, int64_t t) {
    for (const auto& s : e.spans)
        if (t >= s.from_ms && (s.to_ms == 0 || t <= s.to_ms)) return true;
    return false;
}

bool FeeLedger::covers(const std::string& symbol, int64_t time_ms) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    return it != map_.end() && covers_locked(it->second, time_ms);
}

// ── 记账 ─────────────────────────────────────────────────────────────────────
bool FeeLedger::apply(const std::string& symbol, int64_t tran_id, const std::string& asset,
                      double income, int64_t time_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& e = map_[symbol];
    if (time_ms < e.cursor_ms) return false;                     // 处理过的时段
    if (time_ms == e.cursor_ms) {
        // 同一毫秒可能有好几条（一笔市价单分多次成交）。有 tranId 就按它去重；
        // 没有 tranId（理论上不会）只能退回"同一毫秒算重复"，宁可少记不重记
        if (tran_id == 0) return false;
        if (std::find(e.ids_at_cursor.begin(), e.ids_at_cursor.end(), tran_id)
                != e.ids_at_cursor.end()) return false;
    } else {
        e.cursor_ms = time_ms;
        e.ids_at_cursor.clear();
    }
    if (tran_id != 0) e.ids_at_cursor.push_back(tran_id);

    // 游标照常前进（这条已经"看过"了），但只有落在 bot 时间段里的才计入
    if (!covers_locked(e, time_ms)) return false;
    if (asset != "USDT") { ++e.skipped_asset; return false; }
    e.total += income;
    ++e.count;
    return true;
}

// ── 同步计划 ─────────────────────────────────────────────────────────────────
std::vector<FeeLedger::Job> FeeLedger::plan(int64_t now_ms) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Job> jobs;
    for (const auto& [sym, e] : map_) {
        if (e.spans.empty()) continue;
        bool    open       = false;
        int64_t earliest   = e.spans.front().from_ms;
        int64_t last_close = 0;
        for (const auto& s : e.spans) {
            earliest = std::min(earliest, s.from_ms);
            if (s.to_ms == 0) open = true; else last_close = std::max(last_close, s.to_ms);
        }
        // bot 还在：一直同步到现在。已删：同步到删除时刻之后 kGraceMs 就收手
        const int64_t need_until = open ? now_ms : last_close + kGraceMs;
        if (e.synced_to_ms >= need_until) continue;

        int64_t from = (e.synced_to_ms > 0) ? e.synced_to_ms - kOverlapMs : earliest;
        from = std::max(from, now_ms - kMaxLookbackMs);
        if (from >= now_ms) continue;
        jobs.push_back({sym, from, now_ms});
    }
    return jobs;
}

void FeeLedger::mark_synced(const std::string& symbol, int64_t to_ms) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& e = map_[symbol];
    e.synced_to_ms = std::max(e.synced_to_ms, to_ms);
}

// ── 读 ───────────────────────────────────────────────────────────────────────
double FeeLedger::total() const {
    std::lock_guard<std::mutex> lk(mtx_);
    double s = 0;
    for (const auto& [k, e] : map_) s += e.total;
    return s;
}

FeeLedger::Entry FeeLedger::get(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(symbol);
    return it == map_.end() ? Entry{} : it->second;
}

std::vector<std::string> FeeLedger::symbols() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) out.push_back(k);
    return out;
}

bool FeeLedger::backfilled() const { std::lock_guard<std::mutex> lk(mtx_); return backfilled_; }
void FeeLedger::set_backfilled(bool v) { std::lock_guard<std::mutex> lk(mtx_); backfilled_ = v; }

// ── 落盘 ─────────────────────────────────────────────────────────────────────
bool FeeLedger::save(const std::string& path) const {
    std::ostringstream ss;
    ss << std::setprecision(12);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ss << "{\"backfilled\":" << (backfilled_ ? "true" : "false") << ",\"symbols\":{";
        bool first = true;
        for (const auto& [sym, e] : map_) {
            if (!first) ss << ",";
            first = false;
            ss << "\"" << sym << "\":{"
               << "\"total\":"         << e.total         << ","
               << "\"count\":"         << e.count         << ","
               << "\"skipped_asset\":" << e.skipped_asset << ","
               << "\"cursor_ms\":"     << e.cursor_ms     << ","
               << "\"synced_to_ms\":"  << e.synced_to_ms  << ",\"ids\":[";
            for (size_t i = 0; i < e.ids_at_cursor.size(); ++i)
                ss << (i ? "," : "") << e.ids_at_cursor[i];
            ss << "],\"spans\":[";
            for (size_t i = 0; i < e.spans.size(); ++i)
                ss << (i ? "," : "") << "[" << e.spans[i].from_ms << "," << e.spans[i].to_ms << "]";
            ss << "]}";
        }
        ss << "}}";
    }
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

bool FeeLedger::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto padded = simdjson::padded_string(text);
    if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return false;
    simdjson::dom::object root;
    if (doc.get(root) != simdjson::SUCCESS) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    bool b = false;
    if (root["backfilled"].get(b) == simdjson::SUCCESS) backfilled_ = b;
    simdjson::dom::object syms;
    if (root["symbols"].get(syms) != simdjson::SUCCESS) return true;
    for (auto field : syms) {
        simdjson::dom::object o;
        if (field.value.get(o) != simdjson::SUCCESS) continue;
        Entry e;
        double d = 0; int64_t i = 0;
        if (o["total"].get(d)         == simdjson::SUCCESS) e.total         = d;
        if (o["count"].get(i)         == simdjson::SUCCESS) e.count         = (int)i;
        if (o["skipped_asset"].get(i) == simdjson::SUCCESS) e.skipped_asset = (int)i;
        if (o["cursor_ms"].get(i)     == simdjson::SUCCESS) e.cursor_ms     = i;
        if (o["synced_to_ms"].get(i)  == simdjson::SUCCESS) e.synced_to_ms  = i;
        simdjson::dom::array arr;
        if (o["ids"].get(arr) == simdjson::SUCCESS)
            for (auto v : arr) { int64_t id = 0; if (v.get(id) == simdjson::SUCCESS) e.ids_at_cursor.push_back(id); }
        if (o["spans"].get(arr) == simdjson::SUCCESS)
            for (auto v : arr) {
                simdjson::dom::array pr;
                if (v.get(pr) != simdjson::SUCCESS) continue;
                Span s; int k = 0;
                for (auto x : pr) {
                    int64_t t = 0;
                    if (x.get(t) != simdjson::SUCCESS) t = 0;
                    (k++ == 0 ? s.from_ms : s.to_ms) = t;
                }
                e.spans.push_back(s);
            }
        map_[std::string(field.key)] = e;
    }
    return true;
}

// ── 同步 ─────────────────────────────────────────────────────────────────────
namespace {
// 拉 [from, to] 这一段并按时间升序记账。一页拉满就把区间对半切开、两半分别重拉。
// 不依赖交易所返回的是"区间里最早的 N 条"还是"最新的 N 条"——
// 依赖了而且猜错的话，中间那段会【静默漏掉】
bool fetch_range(TradingClient& client, FeeLedger& ledger, const std::string& sym,
                 int64_t from, int64_t to, int page_limit, int depth, int& applied) {
    bool ok = false;
    auto recs = client.fetch_income("COMMISSION", from, to, sym, page_limit, &ok);
    if (!ok) return false;
    const bool full = (int)recs.size() >= page_limit;
    if (full && to > from && depth < 24) {
        const int64_t mid = from + (to - from) / 2;
        return fetch_range(client, ledger, sym, from, mid, page_limit, depth + 1, applied) &&
               fetch_range(client, ledger, sym, mid + 1, to, page_limit, depth + 1, applied);
    }
    std::sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) {
        return a.time != b.time ? a.time < b.time : a.tran_id < b.tran_id;
    });
    for (const auto& r : recs) {
        if (r.symbol != sym) continue;
        if (ledger.apply(sym, r.tran_id, r.asset, r.income, r.time)) ++applied;
    }
    return true;
}
}  // namespace

int sync_fee_ledger(TradingClient& client, FeeLedger& ledger, int64_t now_ms,
                    bool* all_ok, int page_limit, int max_windows) {
    if (all_ok) *all_ok = true;
    // 交易所 income 接口单次时间跨度上限 7 天，留 1 小时余量
    const int64_t kWindow = 7LL * 24 * 3600 * 1000 - 3600LL * 1000;
    int applied = 0;
    for (const auto& job : ledger.plan(now_ms)) {
        int64_t from = job.from_ms;
        bool ok = true;
        for (int w = 0; w < max_windows && from <= job.to_ms; ++w) {
            const int64_t wto = std::min(from + kWindow, job.to_ms);
            if (!fetch_range(client, ledger, job.symbol, from, wto, page_limit, 0, applied)) {
                ok = false;
                break;
            }
            ledger.mark_synced(job.symbol, wto);   // 拉完一个窗口就记一个窗口
            from = wto + 1;
        }
        if (!ok && all_ok) *all_ok = false;
    }
    return applied;
}

}  // namespace ccbot
