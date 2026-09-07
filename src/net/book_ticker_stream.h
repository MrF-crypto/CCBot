#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <set>
#include <mutex>
#include <atomic>
#include <memory>

namespace ix { class WebSocket; }

namespace ccbot {

// 订阅 Binance USDT-M Futures bookTicker 流
// 提供实时买一/卖一价格，延迟 < 50ms
class BookTickerStream {
public:
    struct Tick {
        std::string symbol;
        double bid        = 0;
        double bid_qty    = 0;
        double ask        = 0;
        double ask_qty    = 0;
        // 买一卖一中间价。名字叫 last_price 是历史遗留——它【不是】最新成交价，
        // 真实成交价会在买卖价之间来回跳，中间价平滑得多，所以界面一直用这个
        double last_price = 0;
        // 标记价（markPrice@1s）。这是币安用来算【强平价、未实现盈亏、强平触发】
        // 的价，和成交价/中间价是两套体系：它带指数成分和资金费基差，抗单交易所插针。
        // 想回答"我离强平还有多远"只能用它，用中间价算出来的距离是错的。
        // 更新频率 1s（bookTicker 是逐笔），所以单独记收包时间，不与 recv_ms 混用
        double  mark_price = 0;
        int64_t mark_ms    = 0;
        // 24 小时滚动涨幅%（<symbol>@ticker 的 P 字段，就是币安界面上那个"24h涨幅"）。
        // 高位拦截用它，而不是"今日涨幅"——后者每天 UTC 0 点归零，
        // 而 UTC 0 点是北京时间早 8 点，是真实交易时段，闸门会在那里瞎掉
        double  chg_24h    = 0;
        int64_t chg_ms     = 0;
        int64_t recv_ms   = 0;   // 本地收包时间戳(ms)
        bool    valid     = false;
    };

    using TickCb = std::function<void(const Tick&)>;

    explicit BookTickerStream(bool testnet = false);
    ~BookTickerStream();

    BookTickerStream(const BookTickerStream&)            = delete;
    BookTickerStream& operator=(const BookTickerStream&) = delete;

    void start();
    void stop();
    bool is_connected() const { return connected_.load(); }

    // 订阅/取消（大写 symbol，如 "BTCUSDT"）
    void subscribe  (const std::string& symbol);
    void unsubscribe(const std::string& symbol);

    // 线程安全读最新 tick；未收到时 valid=false
    Tick   get      (const std::string& symbol) const;
    double mid_price(const std::string& symbol) const;  // (bid+ask)/2, 0=无效
    // 标记价，带与 mid_price 相同的陈旧保护（超时返回 0）。
    // 引擎决策用这个：强平、未实现盈亏、强平触发币安全部按标记价算，
    // 用中间价决策等于拿一套体系的价格去撞另一套体系的强平线
    double mark_price(const std::string& symbol) const;
    // 24 小时滚动涨幅%；无数据或过期返回 false
    bool   change_24h(const std::string& symbol, double& out_pct) const;

    // REST 取到的标记价写回缓存。
    // 存在的理由：markPrice 流不可用时（未订阅成功/刚启动/断流），调用方会转 REST
    // 兜底把价格喂给引擎——但界面读的是这个缓存，不写回的话引擎有价、界面空着。
    // v4.0.9 就是这么漏的：策略照常跑，"标记价"那一列却一直显示 "--"
    void   set_mark_price(const std::string& symbol, double price);

    void on_tick(TickCb cb);   // 每条消息回调（运行在 WS 线程）

    // 服务端返回的【非数据消息】回调（运行在 WS 线程）。
    // 这类消息没有 "stream" 字段，此前被消息入口第一行直接丢弃——包括
    // {"code":2,"msg":"Invalid request..."} 这种订阅失败。后果是某条流没订上时
    // 界面只是一片空白，没有任何线索指向"订阅被拒"。v4.0.9 的标记价就撞上了这个
    using LogCb = std::function<void(const std::string&)>;
    void on_server_msg(LogCb cb);

    static int64_t now_ms();

    // ── 测试注入点 ────────────────────────────────────────────────────────────
    // 报文解析与陈旧判定是这个类里唯一会【静默出错】的部分：解析错了价格是垃圾，
    // 陈旧判定错了引擎会拿着冻结价继续决策——两者都不会报错。而建连接/重连/订阅
    // 出错是显性故障（完全没有价格），不需要单测。
    // 所以只把这两处开出去，不为了可测性去动传输层结构（与订单路径测试同一取向）。
    void on_message_for_test(const std::string& json) { on_message(json); }
    // 把某个品种的收包时间往前推 age_ms 毫秒，用来构造"WebSocket 半开、
    // 缓存里是冻结价"这个最凶险的场景——真等 10 秒会让测试慢得没人愿意跑
    void age_cache_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it != cache_.end()) it->second.recv_ms = now_ms() - age_ms;
    }

private:
    void on_open   ();
    void on_message(const std::string& json);
    // 批量订阅/退订。合并成一条消息而不是一条流一条消息：币安合约 WS 限制
    // 【每秒最多 10 条入站消息】，超了直接断连。每品种 3 条流，逐条发的话
    // 连续加 4 个品种就会触线
    void send_subs (const std::vector<std::string>& streams, bool sub);
    void resubscribe_all();

    static std::string to_lower(std::string s);

    bool testnet_;
    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> running_  {false};
    std::atomic<bool> connected_{false};
    std::atomic<int>  req_id_   {1};

    mutable std::mutex                    mtx_;
    std::set<std::string>                 streams_;   // lowercase "btcusdt@bookTicker"
    std::unordered_map<std::string, Tick> cache_;     // UPPER symbol → Tick

    TickCb tick_cb_;
    LogCb  srv_cb_;
};

} // namespace ccbot
