#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// REST 轮询行情源。
//
// ── 为什么不是 WebSocket ─────────────────────────────────────────────────────
// WS 在纸面上更好：markPrice@1s 每秒一包、延迟几十毫秒。但它有一个实测中反复
// 咬人的失败模式——【连上了却没有数据】：TCP 与 TLS 握手成功、服务端还会回
// SUBSCRIBE 的确认，然后一帧行情都不来。经代理/TUN 的网络里这是常态，而且
// 连"连上即推、不需要任何 SUBSCRIBE"的 /ws/<流名> 端点也一样零数据。
//
// 为了识别这个状态，WS 那套必须额外背上：静默看门狗、强制重连、退避、升级重建、
// 订阅确认原文取证、对照探针。那是【六种机制只为回答一个问题】——"对面还活着吗"。
//
// 轮询没有这个问题，因为它没有持久连接：每一轮要么拿到数据，要么这一轮失败，
// 二者一目了然。"上次成功是什么时候"就是完整的健康状态，不需要看门狗去推断。
// 代价是延迟：一轮 1 秒，比 WS 慢一个数量级——但一个慢一点却始终可信的价格，
// 比一个可能已经冻住却看起来正常的价格有用得多。
//
// ── 为什么一轮只发一个请求 ───────────────────────────────────────────────────
// /fapi/v1/premiumIndex 不带 symbol 时返回【全市场】（权重 10），
// /fapi/v1/ticker/24hr 同理（权重 40）。所以请求数与品种数【无关】：
// 1 个品种和 50 个品种的开销完全一样。
// 逐品种查则是 N 次往返，一轮耗时随品种数线性增长，而且最后那个品种拿到的价
// 已经比第一个旧了好几秒 —— 全取让所有品种共享同一个时间戳。
//
// 权重账（上限 2400/分钟）：标记价 1 秒一轮 = 600/分钟，
// 24h 涨跌 30 秒一轮 = 80/分钟，合计约 680/分钟，留了三倍余量。
namespace ccbot {

class RestPriceFeed {
public:
    // 取数函数注入进来，不在这里直接发 HTTP。
    // 理由有两个：① 复用 TradingClient 里已经过测试的签名、限流闸门、超时与
    // 查单恢复；② 单测里塞一个假函数就能跑完整逻辑，不需要网络。
    using FetchMap = std::function<std::unordered_map<std::string, double>()>;
    using LogCb    = std::function<void(const std::string&)>;

    RestPriceFeed(FetchMap fetch_marks, FetchMap fetch_changes);
    ~RestPriceFeed();
    RestPriceFeed(const RestPriceFeed&)            = delete;
    RestPriceFeed& operator=(const RestPriceFeed&) = delete;

    // 一个品种的最新快照。字段名与旧的 WS 版保持一致，调用方不必改。
    struct Tick {
        std::string symbol;
        double  mark_price = 0;
        int64_t mark_ms    = 0;
        // 与 mark_ms 恒等。保留这个字段只为兼容界面上的"延迟"列——
        // 在 WS 版里它用来区分"WS 活着"与"REST 兜底成功"，而现在 REST 就是
        // 唯一来源，这个区分不再存在，留一个会撒谎的字段比留一个冗余的更糟
        int64_t ws_mark_ms = 0;
        double  chg_24h    = 0;
        int64_t chg_ms     = 0;
    };

    // 健康状态。只有四个数，而且每一个都是【直接观测】到的，不是推断出来的。
    struct Health {
        bool    running       = false;
        int64_t last_ok_ms    = 0;   // 上一次成功拿到标记价的时刻（0 = 还没成功过）
        int64_t age_ms        = 0;   // 距上次成功多久
        int     fail_streak   = 0;   // 连续失败轮数
        size_t  symbols       = 0;
        size_t  fresh         = 0;   // 价格在 kStaleMs 之内的品种数

        // ⚠ 判据是"这一轮有没有拿到数据"，不是"连接还在不在"。
        //   轮询没有"连接"这个中间状态，所以也就没有"看起来正常但其实已死"
        //   这种需要靠看门狗去识别的情形
        bool healthy() const {
            if (!running) return false;
            if (last_ok_ms == 0) return false;          // 一次都没成功
            if (age_ms > kStaleMs) return false;
            // ⚠ 必须看 fail_streak，不能只看"缓存里的价还新鲜"。
            //   周期 1 秒而陈旧阈值 10 秒，所以连挂好几轮之后价格仍在窗口内 ——
            //   只看新鲜度的话健康灯会绿着，而行情其实已经连挂三轮。
            //   这正是这套改写要消灭的那种"粉饰"：健康状态必须是【直接观测】，
            //   不能是"还没坏到看得出来"。
            //   单轮失败不翻脸（网络抖动很常见，翻脸会让指示灯乱闪），
            //   连续 kUnhealthyFails 轮就必须说实话
            if (fail_streak >= kUnhealthyFails) return false;
            return symbols == 0 || fresh == symbols;
        }
        std::string summary() const;
    };

    void start();
    void stop();

    void subscribe(const std::string& symbol);
    void unsubscribe(const std::string& symbol);

    Tick   get(const std::string& symbol) const;
    // 带陈旧保护：超过 kStaleMs 没刷新就返回 0。
    // ⚠ 绝不能把冻结价当现价交出去——引擎会拿它推移动止损、判触线，
    //   而行情该跑多远照样跑
    double mark_price(const std::string& symbol) const;
    bool   change_24h(const std::string& symbol, double& out_pct) const;
    // 外部（引擎自己的补拉）写回一个价。保留它是为了兼容，正常不需要用
    void   set_mark_price(const std::string& symbol, double price);

    Health health() const;
    // 价格已经陈旧的品种，供上层告警
    std::vector<std::string> stale_symbols() const;
    void on_server_msg(LogCb cb);

    // 一个品种的价格多久没刷新就算陈旧。轮询周期 1 秒，给 10 倍余量：
    // 偶尔一两轮失败不该让界面和引擎立刻失明，而连续十轮拿不到就是真出事了
    static constexpr int64_t kStaleMs = 10000;
    // 连续失败几轮就判为不健康。周期 1 秒，所以 3 轮 ≈ 3 秒。
    // 取 3 而不是 1：单轮失败在真实网络里很常见，1 就会让指示灯乱闪；
    // 取 3 而不是 10：等到价格真的陈旧（10 轮）才变色，等于"坏了三秒还装没事"
    static constexpr int     kUnhealthyFails = 3;

    // 墙上时钟毫秒。暴露成静态函数是为了让界面层的"这个价多久没刷新"用上
    // 与行情源【完全同一把尺子】——两边各自取时间会在跨午夜/校时后出现
    // 几毫秒到几百毫秒的错位，而陈旧判定就卡在这个阈值上
    static int64_t now_ms();
    static constexpr int64_t kMarkPollMs = 1000;    // 标记价轮询周期
    static constexpr int64_t kChgPollMs  = 30000;   // 24h 涨跌轮询周期

private:
    void loop();
    void say(const std::string& m) const;

    FetchMap fetch_marks_, fetch_changes_;

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Tick> cache_;   // 键 = 大写品种
    std::set<std::string>                subs_;
    LogCb                                log_cb_;

    std::atomic<bool>    running_{false};
    std::atomic<int64_t> last_ok_ms_{0};
    std::atomic<int>     fail_streak_{0};
    // 连续失败时只报一次，恢复时报一次。中间每轮都报的话，1 秒一轮就是刷屏——
    // 这正是 WS 版里那套退避+节流在解决的问题，而轮询只要一个 bool 就够
    std::atomic<bool>    fail_reported_{false};

    std::thread             th_;
    mutable std::mutex      cv_mtx_;
    std::condition_variable cv_;       // stop() 靠它立刻叫醒，不用等一个周期
};

} // namespace ccbot
