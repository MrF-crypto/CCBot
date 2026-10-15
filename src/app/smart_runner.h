#pragma once
#include "core/journal.h"
#include "core/smart_mode.h"
#include "core/trend_engine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ccbot {

class TradingClient;

// 智能模式的执行层：拉全市场数据 → 打分 → 候选池决策 → 落到引擎上。
// GUI 与 headless 共用这一份，两边只负责"什么时候调"和"界面/日志怎么显示"。
//
// 线程：run_once 是阻塞的（几十秒的网络请求），调用方放到后台线程跑。
// 其余方法都可以在任意线程调用。

// 一次全市场扫描。pool_syms = 当前自动 bot 的品种（它们走放宽的门槛，
// 并且"这轮没拿到数据"时归为未知而不是不合格）
struct MarketScan {
    smart::ScanOutcome outcome;
    int universe = 0;        // USDT 永续总数
    int passed_gate = 0;     // 过了不需要K线的门槛
    int eligible = 0;        // 特征算得出且 ATR% 合格
    std::string error;       // outcome.ok=false 时的原因
};
MarketScan scan_market(TradingClient& c, const smart::Config& cfg,
                       const std::set<std::string>& pool_syms, int64_t now_ms, int parallel = 3);

// 智能裸K 建埋伏池：全市场过裸K 自己的硬门槛（成交额 / 上线天数 / 24h 涨跌，资金费率与拉黑
// 和海龟共用；只要标的类型是 COIN 的，黄金代币另排），过了的各拉 201 根 1h 算结构
struct BareScan {
    bool ok = false;
    int  universe = 0, passed_gate = 0, fetched = 0;
    std::map<std::string, smart::AmbushFeat> feats;
    std::string error;
};
BareScan scan_ambush(TradingClient& c, const smart::Config& cfg, int64_t now_ms, int parallel = 4);
// 给定品种各拉 201 根 1h 算结构（待命刷新用）。拉不到的不在结果里
std::map<std::string, smart::AmbushFeat> fetch_ambush(TradingClient& c,
                                                      const std::vector<std::string>& syms,
                                                      int parallel = 4);

// 自动 bot 里属于某套策略（裸K 看名单代码 K，其余都算海龟）且有持仓或在途单的个数。
// 智能模式同一时刻只跑一套：另一套还有持仓时不让切（界面置灰），运行层也不启动新策略
int smart_holdings(const std::vector<TrendBot>& bots, smart::Strategy s);

// 用成交历史算某个 UTC 日的已实现盈亏，扣估算手续费——口径与引擎的当日亏损一致
// （TrendEngine::book_day）。启动时状态文件里没有今天的数（升级后第一次运行、
// 文件丢了）就用它补，否则当日亏损限额会从 0 重新算
double sum_day_realized(const std::vector<TrendTrade>& trades, int64_t utc_day);

class SmartRunner {
public:
    using LogFn = std::function<void(const std::string& msg, bool warn)>;

    SmartRunner(std::shared_ptr<TradingClient> client, std::shared_ptr<TrendEngine> engine,
                std::string state_path, LogFn log);

    // 换配置：立刻把风控闸门落到引擎上；方向或模板变了的空仓自动 bot 下一轮换参数。
    // 启用 / 方向 / 门槛变化时，下一次 due() 立刻为真
    void set_config(const smart::Config& cfg);
    smart::Config config() const;

    // 到点没有（每小时整点后 1 分钟，等 1h K线收盘；启用或改配置后立即）
    bool due(int64_t now_ms) const;
    void force();

    struct Row {               // 名单展示用
        std::string symbol;
        char   list  = 0;      // 名单 / 分段代码（smart::kList* / kSeg*）
        int    rank  = 0;
        double score = 0;
        double atr_pct = 0, box12_atr = 0, vol_lift = 0, rs = 0, max_up24 = 0;
        bool   has_bot = false;
        // 智能裸K 的埋伏池（海龟名单不用）
        std::string status;              // 埋伏中 / 持仓中 / 今日已点火
        double dist_up_pct = 0;          // 离箱体上沿还有多少 %（负 = 已在上方）
        double kappa = 0, box_pos = 0, up_vol_share = 0;
    };
    // 一份名单：突破模式是"做多 / 做空"两份，混合模式是"强势 / 弱势 / 中间 / 过热"四份
    struct Group {
        std::string title;     // 例如 "强势（仅多）"
        char        code = 0;
        int         quota = 0;
        std::vector<Row> rows;
    };
    struct Report {
        bool        ok = false;
        int64_t     at_ms = 0;
        int         universe = 0, passed_gate = 0, eligible = 0;
        int         added = 0, removed = 0, monitored = 0, holding = 0;
        std::string summary;          // 一行
        std::vector<Group> groups;
    };
    // 把一次扫描整理成名单（--scan 预览与 run_once 共用）
    static std::vector<Group> build_groups(const smart::Ranking& rk, const smart::Segmented& sg,
                                           const smart::Config& cfg,
                                           const std::set<std::string>& with_bot);
    // 扫描 + 落地。阻塞。未启用时只做清理（见 housekeep）
    Report run_once(int64_t now_ms);
    Report last_report() const;

    // 不扫描的轻量维护，可以每分钟调一次：
    //   · 未启用时：空仓的自动 bot 移除（持仓中的等它走完）
    //   · 当日盈亏落盘（亏损限额跨重启要接得上）
    void housekeep(int64_t now_ms);

    // 品种加入/移除时通知应用层（订阅/退订价格、刷新界面）
    std::function<void(const std::string&)> on_symbol_added;
    std::function<void(const std::string&)> on_symbol_removed;

    // ── 智能裸K ───────────────────────────────────────────────────────────
    using PriceFn = std::function<double(const std::string&)>;   // 拿不到新鲜价返回 0
    // 点火检测，每拍（约 3 秒）调一次。会阻塞（要拉K线）——放后台线程，自己防重入。
    // 不是裸K 模式、未启用、埋伏池还没建好时什么都不做：
    //   · 每根 5m 收盘后 3 秒起，池里每个币拉一次 5m 判定刚收盘那根是不是点火（每根只判一次）
    //   · 点火 → 建 bot、喂 1h 快照、按点火K线最低点做初始止损立刻开多
    //   · 空仓的裸K bot（平仓了、或被风控闸门拦着没开成）删掉；币留在池里，之后还能再点火
    void bare_tick(int64_t now_ms, const PriceFn& price_of);
    // 埋伏池进 / 出：订阅 / 退订价格（界面显示离上沿多远用；退订前应用层要确认没有 bot 在用）
    std::function<void(const std::string&, bool)> on_watch;
    // 埋伏池里有它：应用层退订价格前要问（裸K bot 删了，币可能还在池里）
    bool watching(const std::string& symbol) const;
    // 埋伏池的当前状态（界面名单页用；按分数排，状态按此刻的价格与 bot 算）
    std::vector<Row> bare_rows(int64_t now_ms, const PriceFn& price_of) const;

    // 启动时调用：读池状态与当日盈亏
    void load();
    // 数据记录层：每轮扫描的名单快照、候选池的加入/移出/换方向写进 journal（可不设）
    void set_journal(std::shared_ptr<Journal> j) { journal_ = std::move(j); }
    // load 之后调用：引擎里今天的已实现还是 0（状态文件里没有今天的数）时，
    // 用成交历史补上
    void seed_day_from_history(const std::vector<TrendTrade>& trades);

    // 作废这个运行器：正在跑的那一轮扫描完成后【不落地】、不写状态文件。
    // GUI 断线重连会连引擎一起重建运行器，旧的那轮扫描若照常落地，会把结果写进
    // 已经不用的旧引擎，还和新运行器抢同一个状态文件
    void cancel();

private:
    void apply_engine_gates_locked();
    void save_locked() const;
    static bool same_params(const TrendConfig& a, const TrendConfig& b);
    Report run_bare_once(int64_t now_ms, const smart::Config& cfg);
    // 另一套策略留下的自动 bot：空仓的删掉（连同那一套的选币数据），持仓中的标「待移出」
    // （管到平仓、不再开新仓）。返回还在持仓的个数——不为 0 时这一套不启动
    int retire_foreign(smart::Strategy keep, int64_t now_ms);
    // 还有另一套的持仓：本轮不扫描、不开仓，日志只提一次（换了数量再提）
    bool blocked_by_foreign(int holding, smart::Strategy keep);
    int  foreign_noted_ = -1;   // 上次提示时的持仓数，-1 = 没在等（mtx_ 保护）
    // 埋伏池清空（离开裸K 模式时），退订价格
    void clear_seeds();
    // 建池 / 待命刷新（run_bare_once 的两个分支）
    Report build_ambush(int64_t now_ms, const smart::Config& cfg);
    Report refresh_ambush(int64_t now_ms, const smart::Config& cfg);
    // 一个币点火了：建 bot、开仓
    void ignite(const std::string& sym, const smart::Ambush& a, const smart::Ignition& g,
                const smart::Config& cfg, const PriceFn& price_of, int64_t now_ms);

    struct AmbushState {
        smart::Ambush a;
        double  add_close = 0;     // 进池时的收盘价（进池后涨太多就移出）
        int64_t ignited_ms = 0;    // 最近一次点火开仓的时刻（界面显示「今日已点火」）
    };
    std::map<std::string, AmbushState> seeds_;      // 埋伏池，mtx_ 保护
    int64_t    built_day_ = -1;                     // 建池那天的 UTC 日序号，-1 = 还没建（mtx_）
    std::mutex bare_mtx_;                           // bare_tick 不重入
    int64_t    ign_bar_ = 0;                        // 正在判定的那根 5m 的收盘时刻（bare_mtx_）
    std::set<std::string> ign_done_;                // 这根已经判过的币（bare_mtx_）
    // 上一次刷新以来的点火判定，mtx_ 保护
    struct BareStats { int judged = 0, vol_hit = 0, vetoed = 0, ignited = 0, entered = 0; };
    BareStats  bare_stats_;

    std::shared_ptr<TradingClient> client_;
    std::shared_ptr<TrendEngine>   engine_;
    std::shared_ptr<Journal>       journal_;
    std::string                    state_path_;
    LogFn                          log_;

    mutable std::mutex mtx_;
    smart::Config      cfg_;
    smart::Pool        pool_;
    Report             last_;
    int64_t            next_due_ms_ = 0;   // 0 = 立刻
    std::mutex         run_mtx_;           // run_once 不重入
    std::atomic<bool>  cancelled_{false};
    // 连续扫描失败的轮数。只有第一轮失败走告警（webhook），之后只记日志，
    // 恢复时说一声——长时间断网不至于每小时推一条同样的话
    int                fail_streak_ = 0;
};

} // namespace ccbot
