#pragma once
#include "core/trend_engine.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// 智能模式：扫描 USDT 永续全市场 → 打分排序 → 候选池 → 自动建海龟 bot 监控 →
// 每轮扫描刷新（淘汰、补位）。
//
// 这里只有【纯逻辑】：门槛判定、特征计算、打分排名、候选池的增删决策、配置的
// JSON 往返。不碰网络、不碰引擎——拉数据在 net/market_scan，把决策落到引擎上
// 在应用层（GUI / headless 共用 app 层的同一个函数）。这样整套规则不开窗口、
// 不连交易所就能测。
//
// ── 规则总览（与用户逐条确认过）─────────────────────────────────────────────
//   · 方向由勾选决定：勾做多扫"强势 + 靠近上沿"，勾做空扫"弱势 + 靠近下沿"，
//     两份名单分开排。两个都勾时 bot 是【多空】：向上突破做多、向下突破做空，
//     不看它从哪份名单进来——名单只负责选币
//   · 持仓中的 bot【绝不】因为排名下降被移除，一定等这笔走完
//   · 一笔走完后 bot 保留、继续等下一次突破；只有空仓时满足淘汰条件才移除
//   · 进名单看前 N，出名单要掉出 1.5N 且连续 2 轮，加入不满 24 小时不淘汰
//     （硬门槛不合格除外）——不对称的门槛，排名在边界附近浮动时不会来回进出
//   · 这一轮【没拿到数据】的币不做任何处理。"没拿到"不等于"不合格"——这是本项目
//     踩过六次的缺陷族，智能模式一旦把它当成不合格，网络抖一下就会删掉一批 bot
namespace ccbot::smart {

struct Bar {
    int64_t open_ms = 0;
    double  open = 0, high = 0, low = 0, close = 0, volume = 0;
};

struct Config {
    bool enabled   = false;
    bool dir_long  = true;
    bool dir_short = true;

    // ── 硬门槛 ──
    double min_quote_vol   = 5e6;    // 24h 成交额（U）
    int    min_age_days    = 14;     // 上线天数
    double atr_pct_min     = 0.8;    // 1h ATR% 区间
    double atr_pct_max     = 6.0;
    double max_abs_chg24   = 50;     // 24h 涨跌幅绝对值上限（%）：已经走完一大段的不追
    double max_abs_funding = 0.1;    // 资金费率绝对值上限（% / 8h）：太拥挤或被操纵
    double max_tick_pct    = 0.1;    // 最小价格变动 ÷ 价格（%）：精度太粗止损和 ATR 都失真
    std::set<std::string> blacklist; // 用户拉黑

    // ── 名单 ──
    int    long_n    = 50;
    int    short_n   = 50;
    double min_score = 50;           // 排名是相对的，这道线防"矮子里拔将军"

    // ── 淘汰 ──
    double drop_rank_factor = 1.5;   // 掉出 1.5N
    int    drop_after_scans = 2;     // 且连续 2 轮
    int    min_residence_h  = 24;    // 加入不满这么久不淘汰（硬门槛除外）

    // ── 风控（落到引擎的账户级闸门上）──
    int    max_positions    = 20;    // 同时持仓上限（全部 bot）
    double daily_loss_limit = 0;     // 当日亏损限额（U，0=关）
    double float_loss_limit = 0;     // 持仓浮亏限额（U，0=关）
    int    prio_reserve     = 2;     // 名额剩这么多以内时……
    double prio_min_score   = 70;    // ……只接受这个分以上的自动 bot
    bool   pause_new        = false; // 应急：暂停全部 bot 开新仓（不删 bot、不动已有仓位）

    // ── bot 模板（海龟）。symbol 留空，方向/反手/硬止损由 make_bot_config 强制 ──
    TrendConfig tmpl = default_template();

    static TrendConfig default_template();
};

// 自动 bot 的方向：两个都勾 = 多空，只勾一个 = 仅那个方向
trend::Direction bot_direction(const Config& cfg);
// 按模板生成某个品种的 bot 配置
TrendConfig make_bot_config(const Config& cfg, const std::string& symbol);

// ── 市场数据（net 层拉好后喂进来）────────────────────────────────────────────
struct SymbolMeta {
    std::string symbol, base, quote, contract_type, status;
    int64_t onboard_ms  = 0;
    int64_t delivery_ms = 0;     // 永续是 2100 年；公告下架后会被改到近期
    double  tick = 0;            // 最小价格变动
};
struct Ticker24 {
    double quote_vol = 0;        // 24h 成交额（U）
    double chg_pct   = 0;        // 24h 涨跌幅（%）
    double last      = 0;
};

// 内置排除：BTC、ETH、稳定币
bool builtin_excluded(const std::string& base);

// 不需要K线的硬门槛。返回空串 = 通过，否则是原因。
// relax = 已经在池里的币：成交额按门槛的 80% 判（滞回），免得在门槛边上来回进出。
// t / funding 为 nullptr = 这轮没拿到，不据此判不合格（由调用方归为"未知"）
std::string pre_gate(const SymbolMeta& m, const Ticker24* t, const double* funding_pct,
                     const Config& cfg, int64_t now_ms, bool relax);
// 下架判定单独拿出来：持仓中的 bot 碰到它要告警
bool delisting(const SymbolMeta& m, int64_t now_ms);

// ── 特征 ──────────────────────────────────────────────────────────────────
struct Features {
    bool        ok = false;
    std::string why;            // !ok 时的原因
    double atr_pct   = 0;       // 1h ATR14 ÷ 价格（%）
    double box12_atr = 0;       // 最近 12 根箱体 ÷ ATR14
    double box48_atr = 0;       // 最近 48 根箱体 ÷ ATR14（横盘时长）
    double atr_ratio = 0;       // ATR14 ÷ ATR100（波动率压缩）
    double vol_lift  = 0;       // 最近 6 根均量 ÷ 最近 168 根均量（量能抬升）
    double pos12     = 0;       // 收盘价在 12 根箱体里的位置，0 = 下沿，1 = 上沿
    double rs        = 0;       // (7 天涨幅 − BTC 7 天涨幅) ÷ 7 天波动率
    double er24      = 0;       // 24 根效率比，带方向
};
// bars / btc：已收盘的 1h K线，时间正序，不含未收盘那根。至少 kMinBars 根
inline constexpr int kMinBars = 170;
Features compute_features(const std::vector<Bar>& bars, const std::vector<Bar>& btc);
// ATR% 区间（要K线，所以和 pre_gate 分开）。relax 时区间各放宽 20%
std::string atr_gate(const Features& f, const Config& cfg, bool relax);

// ── 打分排名 ──────────────────────────────────────────────────────────────
// 每个指标先在【该方向所有合格的币】里换算成百分位（0~100），再加权求和。
// 用百分位而不是原始值：各指标量纲完全不同，直接相加会被数值大的那个主导
struct Weights {
    double rs = 30, er = 15;                       // 强弱（趋势延续）
    double box12 = 10, atr_ratio = 10, box48 = 10; // 蓄势（横盘爆拉）
    double vol = 15, pos = 10;                     // 苏醒
};
struct Scored {
    std::string symbol;
    double long_score = 0, short_score = 0;
    Features f;
};
struct Ranking {
    std::vector<std::string> longs, shorts;   // 分数降序，只含 ≥ min_score 的
    std::map<std::string, Scored> rows;       // 全部合格的币
    int rank_long (const std::string& s) const;   // 1 起，0 = 不在
    int rank_short(const std::string& s) const;
};
Ranking rank(const std::map<std::string, Features>& eligible, const Config& cfg,
             const Weights& w = Weights{});

// ── 候选池 ────────────────────────────────────────────────────────────────
struct BotView {
    std::string bot_id, symbol;
    bool is_auto = false;
    bool has_pos = false;
    bool stopped = false;
};

struct ScanOutcome {
    bool ok = false;                           // 整轮可信。false 时什么都不做
    Ranking ranking;
    std::map<std::string, std::string> hard_fail;   // 明确不合格 → 原因
    std::set<std::string> unknown;             // 这轮没拿到数据（绝不当成不合格）
    std::set<std::string> delisting;           // 即将下架
};

struct Plan {
    struct Add    { std::string symbol; char list = 0; int rank = 0; double score = 0; };
    struct Remove { std::string bot_id, symbol, why; };
    struct Meta   { std::string bot_id; double score = 0; int rank = 0; char list = 0;
                    bool leaving = false; };   // leaving = 持仓中但已不合格，走完就移除
    std::vector<Add>         add;
    std::vector<Remove>      remove;
    std::vector<Meta>        meta;
    std::vector<std::string> alerts;           // 要醒目告警的（持仓中的币即将下架）
};

class Pool {
public:
    struct Entry {
        int64_t added_ms   = 0;
        int     miss_scans = 0;    // 连续几轮不在名单（1.5N 以内）
    };

    // 根据这一轮扫描与当前 bot，给出增删决策。会更新内部的连续未达标计数。
    Plan plan(const ScanOutcome& scan, const std::vector<BotView>& bots,
              const Config& cfg, int64_t now_ms);
    // 应用层真的加上 / 删掉之后回报（加失败的不记，免得留下幽灵条目）
    void on_added  (const std::string& symbol, int64_t now_ms);
    void on_removed(const std::string& symbol);

    const std::map<std::string, Entry>& entries() const { return entries_; }
    std::string to_json() const;
    bool        from_json(std::string_view s);

private:
    std::map<std::string, Entry> entries_;
};

// ── 配置的 JSON 往返（GUI 存独立文件，headless 读主配置里的 "smart_mode" 段）──
std::string config_to_json(const Config& cfg);
// 缺的键保持 cfg 里原有的值（调用方先放默认值）。err 非空 = 有非法值，cfg 不动
bool config_from_json(std::string_view s, Config& cfg, std::string* err);

} // namespace ccbot::smart
