#include "backtest/bt_data.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <filesystem>

namespace ccbot::bt {

namespace {

// "2026-01-01 00:00:00" → UTC毫秒。容忍非补零（"2021/6/1 0:00"）与两种分隔符
int64_t parse_ts(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    // 依次尝试：ISO带秒 / ISO无秒 / 斜杠带秒 / 斜杠无秒
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5 &&
        std::sscanf(s.c_str(), "%d/%d/%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5)
        return 0;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31) return 0;

    // 手算UTC天数（避免 timegm/_mkgmtime 的平台差异与本地时区污染）
    // 民用历法转儒略日算法（Howard Hinnant days_from_civil）
    int y = Y - (M <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return ((days * 24 + h) * 60 + m) * 60000LL + (int64_t)sec * 1000;
}

double to_d(const std::string& s) {
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch (...) { return 0.0; }
}

// 按逗号切分（数据无引号包裹字段，简单切分即可）
void split_csv(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        size_t pos = line.find(',', start);
        if (pos == std::string::npos) { out.emplace_back(line.substr(start)); break; }
        out.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
}

constexpr int64_t kMinuteMs = 60000;
constexpr uint32_t kCacheMagic = 0x43424B31;  // "CBK1"

} // namespace

std::string normalize_symbol(const std::string& filename) {
    std::string s = std::filesystem::path(filename).stem().string();
    std::string out;
    for (char c : s) {
        if (c == '-' || c == '_') continue;             // BTC-USDT → BTCUSDT
        out += (char)std::toupper((unsigned char)c);
    }
    return out;
}

std::string ts_to_str(int64_t ms) {
    if (ms <= 0) return "-";
    // 毫秒 → 民用历法（civil_from_days 的逆运算），全程UTC不碰本地时区
    int64_t days = ms / 86400000LL;
    int64_t rem  = ms % 86400000LL;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo  = mp + (mp < 10 ? 3 : -9);
    y += (mo <= 2);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02lld:%02lld:%02lld",
                  (long long)y, mo, d, (long long)(rem / 3600000),
                  (long long)(rem / 60000 % 60), (long long)(rem / 1000 % 60));
    return buf;
}

std::string QualityReport::to_text() const {
    std::ostringstream o;
    o << "品种 " << symbol << "\n"
      << "  K线数     : " << total_bars << "\n"
      << "  时间范围  : " << ts_to_str(first_ts) << " ~ " << ts_to_str(last_ts) << " (UTC)\n"
      << "  缺口      : " << gap_count << " 段 / 共缺 " << gap_minutes << " 分钟";
    if (total_bars + gap_minutes > 0)
        o << "（完整度 " << std::fixed
          << (int)(100.0 * total_bars / (total_bars + gap_minutes) + 0.5) << "%）";
    o << "\n"
      << "  坏点剔除  : OHLC异常 " << bad_ohlc << " / 重复时间戳 " << dup_ts << "\n"
      << "  资金费点  : " << funding_pts << " 次结算\n";
    if (!largest_gaps.empty()) {
        o << "  最大缺口  :\n";
        for (const auto& [a, b] : largest_gaps)
            o << "    " << ts_to_str(a) << " → " << ts_to_str(b)
              << "  (" << (b - a) / kMinuteMs << " 分钟)\n";
    }
    return o.str();
}

bool load_csv(const std::string& path, Series& out, QualityReport& rep, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "打不开文件: " + path; return false; }

    out.symbol = normalize_symbol(path);
    rep = QualityReport{};
    rep.symbol = out.symbol;

    std::string line;
    // 第1行是版权说明（非CSV头）——不硬性假设，用"是否含 candle_begin_time"来判断
    if (!std::getline(f, line)) { err = "空文件"; return false; }
    if (line.find("candle_begin_time") == std::string::npos) {
        if (!std::getline(f, line)) { err = "只有版权行没有表头"; return false; }
    }
    // 此时 line = 表头，按列名定位下标（不写死顺序，容忍字段增减）
    std::vector<std::string> cols;
    split_csv(line, cols);
    auto idx_of = [&](const char* name) -> int {
        for (size_t i = 0; i < cols.size(); ++i) {
            std::string c = cols[i];
            c.erase(std::remove_if(c.begin(), c.end(),
                    [](unsigned char ch){ return ch=='\r'||ch=='\n'||ch==' '; }), c.end());
            if (c == name) return (int)i;
        }
        return -1;
    };
    const int i_ts = idx_of("candle_begin_time"), i_o = idx_of("open"),
              i_h = idx_of("high"), i_l = idx_of("low"), i_c = idx_of("close"),
              i_v = idx_of("volume"), i_qv = idx_of("quote_volume"),
              i_sp = idx_of("Spread"), i_fr = idx_of("fundingRate");
    if (i_ts < 0 || i_o < 0 || i_h < 0 || i_l < 0 || i_c < 0) {
        err = "表头缺少必要列（candle_begin_time/open/high/low/close）";
        return false;
    }

    std::vector<std::string> fs;
    out.bars.clear();
    out.bars.reserve(400000);
    int64_t prev_ts = 0;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        split_csv(line, fs);
        if ((int)fs.size() <= i_c) continue;

        Bar b;
        b.ts_ms = parse_ts(fs[i_ts]);
        if (b.ts_ms <= 0) continue;
        b.open  = to_d(fs[i_o]);  b.high = to_d(fs[i_h]);
        b.low   = to_d(fs[i_l]);  b.close = to_d(fs[i_c]);
        if (i_v  >= 0 && (int)fs.size() > i_v)  b.volume    = to_d(fs[i_v]);
        if (i_qv >= 0 && (int)fs.size() > i_qv) b.quote_vol = to_d(fs[i_qv]);
        if (i_sp >= 0 && (int)fs.size() > i_sp) b.spread    = std::fabs(to_d(fs[i_sp]));
        if (i_fr >= 0 && (int)fs.size() > i_fr) b.funding   = to_d(fs[i_fr]);

        // 坏点剔除：非正价格、high<low、极端离群（防单根坏点造成假爆仓/假周期）
        if (b.open <= 0 || b.high <= 0 || b.low <= 0 || b.close <= 0 ||
            b.high < b.low || b.high < b.open || b.high < b.close ||
            b.low > b.open || b.low > b.close) {
            ++rep.bad_ohlc;
            continue;
        }
        // 重复/倒序时间戳剔除（数据拼接可能产生）
        if (prev_ts > 0 && b.ts_ms <= prev_ts) { ++rep.dup_ts; continue; }

        if (b.funding != 0) ++rep.funding_pts;
        prev_ts = b.ts_ms;
        out.bars.push_back(b);
    }

    if (out.bars.empty()) { err = "没有解析出任何有效K线"; return false; }

    // 缺口统计：相邻bar间隔 > 1分钟即为缺口（不填充——填充会制造价格静止的假象，
    // 网格会在假数据上刷假周期。回放器遇缺口按虚拟时钟跳跃处理）
    std::vector<std::pair<int64_t,int64_t>> gaps;
    for (size_t i = 1; i < out.bars.size(); ++i) {
        int64_t d = out.bars[i].ts_ms - out.bars[i-1].ts_ms;
        if (d > kMinuteMs) {
            ++rep.gap_count;
            rep.gap_minutes += (size_t)(d / kMinuteMs - 1);
            gaps.emplace_back(out.bars[i-1].ts_ms, out.bars[i].ts_ms);
        }
    }
    std::sort(gaps.begin(), gaps.end(),
              [](auto& a, auto& b){ return (a.second-a.first) > (b.second-b.first); });
    if (gaps.size() > 5) gaps.resize(5);
    rep.largest_gaps = std::move(gaps);
    rep.total_bars = out.bars.size();
    rep.first_ts   = out.bars.front().ts_ms;
    rep.last_ts    = out.bars.back().ts_ms;
    return true;
}

bool load_csv_multi(const std::vector<std::string>& paths, Series& out,
                    QualityReport& rep, std::string& err) {
    out.bars.clear();
    rep = QualityReport{};
    if (paths.empty()) { err = "没有输入文件"; return false; }

    for (const auto& p : paths) {
        Series part; QualityReport prep; std::string perr;
        if (!load_csv(p, part, prep, perr)) { err = p + ": " + perr; return false; }
        if (out.symbol.empty()) out.symbol = part.symbol;
        rep.bad_ohlc += prep.bad_ohlc;
        rep.dup_ts   += prep.dup_ts;
        out.bars.insert(out.bars.end(), part.bars.begin(), part.bars.end());
    }
    rep.symbol = out.symbol;

    // 跨文件可能乱序/重叠：排序后去重（同一时间戳保留先出现的）
    std::sort(out.bars.begin(), out.bars.end(),
              [](const Bar& a, const Bar& b){ return a.ts_ms < b.ts_ms; });
    size_t w = 0;
    for (size_t i = 0; i < out.bars.size(); ++i) {
        if (w > 0 && out.bars[i].ts_ms == out.bars[w-1].ts_ms) { ++rep.dup_ts; continue; }
        out.bars[w++] = out.bars[i];
    }
    out.bars.resize(w);
    if (out.bars.empty()) { err = "合并后没有数据"; return false; }

    // 合并后重新统计缺口与资金费点
    std::vector<std::pair<int64_t,int64_t>> gaps;
    for (size_t i = 0; i < out.bars.size(); ++i) {
        if (out.bars[i].funding != 0) ++rep.funding_pts;
        if (i == 0) continue;
        int64_t d = out.bars[i].ts_ms - out.bars[i-1].ts_ms;
        if (d > kMinuteMs) {
            ++rep.gap_count;
            rep.gap_minutes += (size_t)(d / kMinuteMs - 1);
            gaps.emplace_back(out.bars[i-1].ts_ms, out.bars[i].ts_ms);
        }
    }
    std::sort(gaps.begin(), gaps.end(),
              [](auto& a, auto& b){ return (a.second-a.first) > (b.second-b.first); });
    if (gaps.size() > 5) gaps.resize(5);
    rep.largest_gaps = std::move(gaps);
    rep.total_bars = out.bars.size();
    rep.first_ts   = out.bars.front().ts_ms;
    rep.last_ts    = out.bars.back().ts_ms;
    return true;
}

bool save_cache(const std::string& path, const Series& s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    uint32_t magic = kCacheMagic;
    uint32_t nsym  = (uint32_t)s.symbol.size();
    uint64_t n     = s.bars.size();
    f.write((const char*)&magic, 4);
    f.write((const char*)&nsym, 4);
    f.write(s.symbol.data(), nsym);
    f.write((const char*)&n, 8);
    f.write((const char*)s.bars.data(), (std::streamsize)(n * sizeof(Bar)));
    return f.good();
}

bool load_cache(const std::string& path, Series& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0, nsym = 0;
    uint64_t n = 0;
    f.read((char*)&magic, 4);
    if (magic != kCacheMagic) return false;
    f.read((char*)&nsym, 4);
    if (nsym > 64) return false;
    out.symbol.resize(nsym);
    f.read(out.symbol.data(), nsym);
    f.read((char*)&n, 8);
    if (!f || n > 50000000ULL) return false;
    out.bars.resize((size_t)n);
    f.read((char*)out.bars.data(), (std::streamsize)(n * sizeof(Bar)));
    return f.good() || f.eof();
}

int64_t interval_ms(const std::string& s) {
    if (s.size() < 2) return 0;
    int64_t num = 0;
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) num = num * 10 + (s[i++] - '0');
    if (num <= 0) return 0;
    char unit = s[i];
    switch (unit) {
    case 'm': return num * 60000LL;
    case 'h': return num * 3600000LL;
    case 'd': return num * 86400000LL;
    case 'w': return num * 604800000LL;
    default:  return 0;
    }
}

std::vector<Bar> aggregate(const std::vector<Bar>& src, int64_t period_ms) {
    std::vector<Bar> out;
    if (src.empty() || period_ms < kMinuteMs) return out;
    out.reserve(src.size() / (size_t)(period_ms / kMinuteMs) + 2);

    int64_t cur_start = -1;
    Bar acc{};
    for (const auto& b : src) {
        int64_t bucket = b.ts_ms - (b.ts_ms % period_ms);   // UTC自然边界对齐
        if (bucket != cur_start) {
            if (cur_start >= 0) out.push_back(acc);
            cur_start   = bucket;
            acc         = b;
            acc.ts_ms   = bucket;
        } else {
            acc.high      = std::max(acc.high, b.high);
            acc.low       = std::min(acc.low,  b.low);
            acc.close     = b.close;
            acc.volume   += b.volume;
            acc.quote_vol+= b.quote_vol;
            acc.spread    = b.spread;
            if (b.funding != 0) acc.funding = b.funding;
        }
    }
    if (cur_start >= 0) out.push_back(acc);
    return out;
}

} // namespace ccbot::bt
