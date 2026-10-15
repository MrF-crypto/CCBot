#include "core/journal.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace ccbot {

namespace {

namespace fs = std::filesystem;

constexpr int64_t kDayMs = 24LL * 3600 * 1000;

std::string num(double v, int prec = 6) {
    if (!std::isfinite(v)) return {};
    std::ostringstream o;
    o << std::setprecision(prec) << v;
    return o.str();
}

int64_t ms_of(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

std::string seg_name(char c) {
    switch (c) {
    case smart::kListLong:   return "做多名单";
    case smart::kListShort:  return "做空名单";
    case smart::kSegStrong:  return "强势";
    case smart::kSegWeak:    return "弱势";
    case smart::kSegMid:     return "中间";
    case smart::kSegHot:     return "过热";
    case smart::kSegBare:    return "裸K埋伏";
    default:                 return {};
    }
}

std::string trigger_name(char c) {
    switch (c) {
    case 'I': return "点火";
    default:  return {};
    }
}

std::string side_name(trend::Pos p) {
    return p == trend::Pos::Short ? "空" : "多";
}

int rank_in(const std::vector<std::string>& v, const std::string& s) {
    auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? 0 : (int)(it - v.begin()) + 1;
}

} // namespace

Journal::Journal(std::string dir, int keep_days) : dir_(std::move(dir)), keep_days_(keep_days) {}

std::string Journal::csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    return out + "\"";
}

std::string Journal::utc_time(int64_t ms) {
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::ostringstream o;
    o << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return o.str();
}

std::string Journal::utc_date(int64_t ms) {
    const std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::ostringstream o;
    o << std::put_time(&tmv, "%Y%m%d");
    return o.str();
}

void Journal::append(const std::string& file, const std::string& header, const std::string& lines) {
    // 写失败就丢掉：数据记录层不能反过来影响交易
    try {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        const fs::path p = fs::path(dir_) / file;
        const bool fresh = !fs::exists(p, ec);
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (!f) return;
        if (fresh) f << "\xEF\xBB\xBF" << header << "\n";   // BOM：Excel 直接打开不乱码
        f << lines;
    } catch (...) {}
}

void Journal::prune_locked(int64_t now_ms) {
    const std::string today = utc_date(now_ms);
    if (pruned_day_ == today) return;
    pruned_day_ = today;
    const std::string cutoff = utc_date(now_ms - (int64_t)keep_days_ * kDayMs);
    try {
        std::error_code ec;
        if (!fs::exists(dir_, ec)) return;
        for (const auto& e : fs::directory_iterator(dir_, ec)) {
            const std::string name = e.path().filename().string();
            std::string date;
            if (name.rfind("scan-", 0) == 0)        date = name.substr(5, 8);
            else if (name.rfind("filter-", 0) == 0) date = name.substr(7, 8);
            else if (name.rfind("ambush-", 0) == 0) date = name.substr(7, 8);
            else continue;
            if (date.size() == 8 && date < cutoff) fs::remove(e.path(), ec);
        }
    } catch (...) {}
}

void Journal::trade(const TrendTrade& t) {
    const int64_t close_ms = ms_of(t.close_time);
    const int64_t open_ms  = t.open_time.time_since_epoch().count() ? ms_of(t.open_time) : 0;
    const double  fee = (std::fabs(t.entry_price) + std::fabs(t.exit_price)) *
                        std::fabs(t.qty) * TrendEngine::kEstFeeRate;
    std::ostringstream l;
    l << utc_time(close_ms) << ","
      << (open_ms ? utc_time(open_ms) : std::string()) << ","
      << (open_ms ? num((close_ms - open_ms) / 3600000.0, 4) : std::string()) << ","
      << csv_field(t.symbol) << "," << side_name(t.side) << ","
      << trend::strategy_name(t.strategy) << "," << csv_field(t.interval) << ","
      << (t.is_auto ? "自动" : "手动") << "," << seg_name(t.seg) << ","
      << (t.rank > 0 ? std::to_string(t.rank) : std::string()) << ","
      << (t.score > 0 ? num(t.score, 4) : std::string()) << ","
      << num(t.entry_price, 10) << "," << num(t.exit_price, 10) << "," << num(t.qty, 10) << ","
      << num(t.pnl, 8) << "," << num(fee, 6) << "," << num(t.pnl - fee, 8) << ","
      << num(t.entry_dist_atr, 4) << ","
      << num(t.mfe_usd, 8) << "," << num(t.mae_usd, 8) << ","
      << num(t.mfe_pct, 4) << "," << num(t.mae_pct, 4) << ","
      << num(t.mfe_atr, 4) << "," << num(t.mae_atr, 4) << ","
      << num(t.mfe_r, 4)   << "," << num(t.mae_r, 4)   << ","
      << trigger_name(t.trigger) << "," << num(t.entry_vol_ratio, 4) << ","
      << num(t.entry_box_atr, 4) << ","
      << csv_field(t.reason) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    append("trades.csv",
           "平仓时间UTC,开仓时间UTC,持仓小时,品种,方向,策略,周期,手动自动,开仓时所在段,段内名次,"
           "分数,开仓价,平仓价,数量,盈亏U,估算手续费U,净盈亏U,入场离通道边沿ATR,"
           "浮盈亏最高U,浮盈亏最低U,浮盈亏最高%,浮盈亏最低%,浮盈亏最高ATR,浮盈亏最低ATR,"
           "浮盈亏最高R,浮盈亏最低R,触发方式,入场量比,入场箱体ATR,原因",
           l.str());
}

void Journal::filter(const FilterEvent& e) {
    const int64_t ms = ms_of(e.time);
    std::ostringstream l;
    l << utc_time(ms) << "," << csv_field(e.symbol) << "," << (e.is_long ? "向上" : "向下") << ","
      << (e.is_auto ? "自动" : "手动") << "," << csv_field(e.category) << ","
      << num(e.price, 10) << "," << csv_field(e.detail) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    prune_locked(ms);
    append("filter-" + utc_date(ms) + ".csv", "时间UTC,品种,突破方向,手动自动,卡在,价格,说明", l.str());
}

void Journal::pool(int64_t ms, const std::string& symbol, const std::string& event,
                   char seg, int rank, double score, const std::string& reason) {
    std::ostringstream l;
    l << utc_time(ms) << "," << csv_field(symbol) << "," << csv_field(event) << ","
      << seg_name(seg) << "," << (rank > 0 ? std::to_string(rank) : std::string()) << ","
      << (score > 0 ? num(score, 4) : std::string()) << "," << csv_field(reason) << "\n";
    std::lock_guard<std::mutex> lk(mtx_);
    append("pool.csv", "时间UTC,品种,事件,段,段内名次,分数,原因", l.str());
}

void Journal::scan(int64_t ms, const smart::Ranking& rk, const smart::Segmented& sg,
                   const std::set<std::string>& with_bot) {
    const std::string t = utc_time(ms);
    std::ostringstream l;
    for (const auto& kv : rk.rows) {
        const auto& s = kv.second;
        const auto& f = s.f;
        char seg = 0;
        int  rank = 0;
        if (auto it = sg.seg.find(kv.first); it != sg.seg.end()) {
            // 混合模式：所在段，以及在该段名单里的名次（没进名单 = 空）
            seg = it->second;
            const auto& list = seg == smart::kSegStrong ? sg.strong
                             : seg == smart::kSegWeak   ? sg.weak
                             : seg == smart::kSegHot    ? sg.hot
                                                        : sg.mid;
            rank = rank_in(list, kv.first);
        } else if (int r = rank_in(rk.longs, kv.first); r > 0) {
            seg = smart::kListLong;  rank = r;    // 突破模式
        } else if (int r2 = rank_in(rk.shorts, kv.first); r2 > 0) {
            seg = smart::kListShort; rank = r2;
        }
        l << t << "," << csv_field(kv.first) << "," << seg_name(seg) << ","
          << (rank > 0 ? std::to_string(rank) : std::string()) << ","
          << num(s.long_score, 4) << "," << num(s.short_score, 4) << ","
          << num(s.squeeze_score, 4) << "," << num(s.rs_pct, 4) << ","
          << num(f.rs, 4) << "," << num(f.er24, 4) << "," << num(f.box12_atr, 4) << ","
          << num(f.atr_ratio, 4) << "," << num(f.box48_atr, 4) << "," << num(f.vol_lift, 4) << ","
          << num(f.pos12, 4) << "," << num(f.atr_pct, 4) << "," << num(f.max_up24, 4) << ","
          << num(f.max_dn24, 4) << "," << num(f.close, 10) << ","
          << (with_bot.count(kv.first) ? "是" : "") << "\n";
    }
    std::lock_guard<std::mutex> lk(mtx_);
    prune_locked(ms);
    append("scan-" + utc_date(ms) + ".csv",
           "时间UTC,品种,段,段内名次,做多分,做空分,蓄势分,强弱百分位,相对强弱,效率比24,"
           "箱体12/ATR,ATR14/ATR100,箱体48/ATR,量能抬升,箱体内位置,ATR%,7天最大单日涨%,"
           "7天最大单日跌%,收盘价,已有bot",
           l.str());
}

void Journal::ambush(int64_t ms, const std::string& event, const std::vector<smart::Ambush>& pool,
                     const std::set<std::string>& with_bot) {
    const std::string t = utc_time(ms);
    std::ostringstream l;
    for (const auto& a : pool) {
        const auto& f = a.f;
        l << t << "," << csv_field(event) << "," << csv_field(a.symbol) << "," << num(a.score, 4) << ","
          << num(f.kappa, 4) << "," << num(f.pos, 4) << "," << num(f.atr_pct, 4) << ","
          << (f.atr_ratio > 0 ? num(f.atr_ratio, 4) : std::string()) << ","
          << (f.higher_low ? "是" : "") << "," << num(f.up_vol_share, 4) << ","
          << num(f.box_hi, 10) << "," << num(f.box_lo, 10) << "," << num(f.close, 10) << ","
          << (with_bot.count(a.symbol) ? "是" : "") << "\n";
    }
    std::lock_guard<std::mutex> lk(mtx_);
    prune_locked(ms);
    append("ambush-" + utc_date(ms) + ".csv",
           "时间UTC,事件,品种,分数,κ,箱体内位置,1hATR%,ATR压缩比,低点抬高,上涨量占比,"
           "箱体上沿,箱体下沿,收盘价,已有bot",
           l.str());
}

} // namespace ccbot
