#pragma once
#include "core/smart_mode.h"
#include "core/trend_engine.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

// 数据记录层：把实盘里"事后要拿来分析"的东西写成 CSV，用来校准智能模式与过滤规则。
// 只写不读、不参与任何交易决策；写失败就丢掉这一行，绝不影响交易。
//
// 目录 <数据目录>/journal 下四类文件（UTF-8 带 BOM，Excel 直接打开不乱码）：
//   trades.csv            每笔平仓：开仓时所在的段/分数、入场离通道边沿多少 ATR、持仓时长……
//   pool.csv              候选池事件：加入 / 移出 / 换方向，带原因
//   scan-YYYYMMDD.csv     每轮扫描的完整名单快照（全部合格币的指标、分数、所在段、收盘价）
//                         ——事后拿后续快照里的价格对照，看高分币是不是真的走得更好
//   filter-YYYYMMDD.csv   被过滤掉的突破（每 bot 每根K线最多一条）——事后看它后来怎么走
//   ambush-YYYYMMDD.csv   智能裸K 的埋伏池快照（建池 / 每小时刷新）——进池的币后来有多少真点火了
// 按日的几类保留 keep_days 天（默认 60），每天第一次写时清理更早的。时间一律 UTC
namespace ccbot {

class Journal {
public:
    explicit Journal(std::string dir, int keep_days = 60);

    void trade (const TrendTrade& t);
    void filter(const FilterEvent& e);
    void pool  (int64_t ms, const std::string& symbol, const std::string& event,
                char seg, int rank, double score, const std::string& reason);
    void scan  (int64_t ms, const smart::Ranking& rk, const smart::Segmented& sg,
                const std::set<std::string>& with_bot);
    // 智能裸K：埋伏池快照（每天建池一次、每小时刷新一次）。event = 建池 / 刷新
    void ambush(int64_t ms, const std::string& event, const std::vector<smart::Ambush>& pool,
                const std::set<std::string>& with_bot);

    const std::string& dir() const { return dir_; }

    // 工具（测试也用）
    static std::string csv_field(const std::string& s);   // 含逗号/引号/换行时加引号
    static std::string utc_time(int64_t ms);              // "2026-10-10 08:00:00"
    static std::string utc_date(int64_t ms);              // "20261010"

private:
    void append(const std::string& file, const std::string& header, const std::string& line);
    void prune_locked(int64_t now_ms);

    std::string dir_;
    int         keep_days_;
    std::mutex  mtx_;
    std::string pruned_day_;   // 今天清理过没有
};

} // namespace ccbot
