#pragma once
#include "core/smart_mode.h"
#include "core/trend_engine.h"

#include <cstdint>
#include <functional>
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
        char   list  = 0;
        int    rank  = 0;
        double score = 0;
        double atr_pct = 0, box12_atr = 0, vol_lift = 0, rs = 0;
        bool   has_bot = false;
    };
    struct Report {
        bool        ok = false;
        int64_t     at_ms = 0;
        int         universe = 0, passed_gate = 0, eligible = 0;
        int         added = 0, removed = 0, monitored = 0, holding = 0;
        std::string summary;          // 一行
        std::vector<Row> longs, shorts;
    };
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

    // 启动时调用：读池状态与当日盈亏
    void load();

private:
    void apply_engine_gates_locked();
    void save_locked() const;
    static bool same_params(const TrendConfig& a, const TrendConfig& b);

    std::shared_ptr<TradingClient> client_;
    std::shared_ptr<TrendEngine>   engine_;
    std::string                    state_path_;
    LogFn                          log_;

    mutable std::mutex mtx_;
    smart::Config      cfg_;
    smart::Pool        pool_;
    Report             last_;
    int64_t            next_due_ms_ = 0;   // 0 = 立刻
    std::mutex         run_mtx_;           // run_once 不重入
};

} // namespace ccbot
