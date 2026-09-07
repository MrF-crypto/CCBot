#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <set>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

namespace ix { class WebSocket; }

namespace ccbot {

// Binance USDT-M Futures 行情流。每个品种订阅两条：
//   <symbol>@markPrice@1s  标记价——引擎决策、强平距离、界面显示全部用它
//   <symbol>@ticker        24 小时滚动涨幅——高位拦截用
//
// v4.0.11 起【不再订阅 bookTicker】。v4.0.9 把全系统统一到标记价之后，
// 买一/卖一/中间价在生产代码里已经零引用，那条流只剩下给"延迟"列算包龄——
// 而那个延迟测的是引擎根本不看的流：markPrice 停了、bookTicker 还在的时候，
// 延迟列显示绿色，引擎实际已经在走 REST 兜底。健康指示器指错了对象。
// 去掉之后每品种从 3 条流降到 2 条（币安合约单连接上限 200 条）。
class BookTickerStream {
public:
    struct Tick {
        std::string symbol;
        // 标记价。币安用它算强平价、未实现盈亏、强平触发——带指数成分与资金费
        // 基差，抗单交易所插针。"离强平还有多远"只有它能回答
        double  mark_price = 0;
        int64_t mark_ms    = 0;   // 标记价收包时间(ms)，陈旧判定与延迟显示都用它
        // 24 小时滚动涨幅%（@ticker 的 P 字段，即币安界面上那个"24h涨幅"）。
        // 高位拦截用它而不是"今日涨幅"——后者每天 UTC 0 点归零，
        // 而 UTC 0 点是北京时间早 8 点，是真实交易时段，闸门会在那里瞎掉
        double  chg_24h    = 0;
        int64_t chg_ms     = 0;
        // 买一/卖一与中间价。v4.0.11 曾把这条流整个删掉（当时生产代码确已零引用），
        // v4.0.15 恢复——因为实测发现某些网络路径上 markPrice/ticker/aggTrade/kline
        // 全部收不到包，而 bookTicker 与 depth 照常每秒几百包。三次复测一致，
        // 关压缩、换品种、换全市场合并流都不变，!markPrice@arr@1s 甚至被主动 RST。
        // 那种链路上 bookTicker 是【唯一可用的实时行情】，删掉它等于把引擎
        // 逼回 REST 兜底：47 个品种串行，每个品种约 10 秒才更新一次价格
        double  bid        = 0;
        double  ask        = 0;
        double  mid        = 0;   // (bid+ask)/2，两边都有效时才置
        int64_t mid_ms     = 0;
    };

    explicit BookTickerStream(bool testnet = false);
    ~BookTickerStream();

    BookTickerStream(const BookTickerStream&)            = delete;
    BookTickerStream& operator=(const BookTickerStream&) = delete;

    void start();
    void stop();
    bool is_connected() const { return connected_.load(); }

    // 行情静默自检：连上了但久无数据时返回描述文本，否则返回空串。
    // 调用方（GUI/headless 的 tick 循环）定期调一次，非空就写日志。
    //
    // 存在的理由：实盘出现过【连接成功、订阅已发、服务端不报错、数据永远不来】
    // 这一种状态——三个"正常"信号加起来仍然是完全静默，而系统内部没有任何机制
    // 会注意到，只有 24h 涨幅（唯一没有 REST 兜底的数据）会间接暴露它。
    // 这个自检把"没有消息"本身变成一条消息
    std::string silence_check(int64_t quiet_ms = 30000);

    // 订阅/取消（大写 symbol，如 "BTCUSDT"）。
    // ⚠ 删除 bot 时必须调用 unsubscribe：streams_ 不会自己收缩，重连时全量重订，
    //   反复增删品种会一路累积到币安的 200 条流上限，超出后【静默】失效
    void subscribe  (const std::string& symbol);
    void unsubscribe(const std::string& symbol);

    // 线程安全读最新快照；未收到任何数据时各字段为 0
    Tick   get(const std::string& symbol) const;

    // 标记价，带陈旧保护（超过 kStaleMs 没有新包返回 0）。
    // 引擎决策用这个：WebSocket 半开时连接还在、数据早就不来了，缓存里躺着一个
    // 冻结的价格看起来完全正常，引擎会拿着僵尸价继续补仓/止盈/止损
    double mark_price(const std::string& symbol) const;

    // 买一卖一中间价，带同样的陈旧保护。markPrice 流不可用时的降级价格源。
    double mid_price(const std::string& symbol) const;

    // 引擎用的实时价，按可用性自动降级：
    //   ① markPrice 推送   正常网络（与强平、未实现盈亏同体系）
    //   ② bookTicker 中间价 markPrice 收不到时（某些链路只放行这条流）
    //   ③ 返回 0          两条都没有，调用方走 REST 兜底
    // out_src 回填实际用了哪一档，调用方据此在【切换时】打一条日志——
    // 静默降级正是此前最难查的那类问题：策略照常跑，只是价格延迟从亚秒变成十秒
    enum class PxSrc { None, Mark, Mid };
    double live_price(const std::string& symbol, PxSrc& out_src) const;
    static const char* px_src_name(PxSrc s);

    // 24 小时滚动涨幅%；从未收到或过期返回 false。
    // 涨幅可以合法地为 0 或负数，所以不能像价格那样用返回值 0 表示"无数据"
    bool   change_24h(const std::string& symbol, double& out_pct) const;

    // REST 取到的标记价写回缓存。
    // 存在的理由：markPrice 流不可用时（未订阅成功/刚启动/断流），调用方会转 REST
    // 兜底把价格喂给引擎——但界面读的是这个缓存，不写回的话引擎有价、界面空着。
    // v4.0.9 就是这么漏的：策略照常跑，"标记价"那一列却一直显示 "--"
    void   set_mark_price(const std::string& symbol, double price);

    // 服务端返回的【非数据消息】回调（运行在 WS 线程）。
    // 这类消息没有 "stream" 字段，v4.0.9 之前被消息入口第一行直接丢弃——包括
    // {"code":2,"msg":"Invalid request..."} 这种订阅失败。后果是某条流没订上时
    // 界面只是一片空白，没有任何线索指向"订阅被拒"
    using LogCb = std::function<void(const std::string&)>;
    void on_server_msg(LogCb cb);

    static int64_t now_ms();

    // 陈旧阈值：markPrice@1s 每秒一包，10 秒没来就是这条流断了
    static constexpr int64_t kStaleMs = 10000;

    // ── 测试注入点 ────────────────────────────────────────────────────────────
    // 报文解析与陈旧判定是这个类里唯一会【静默出错】的部分：解析错了价格是垃圾，
    // 陈旧判定错了引擎会拿着冻结价继续决策——两者都不会报错。而建连接/重连/订阅
    // 出错是显性故障（完全没有价格），不需要单测。
    void on_message_for_test(const std::string& json) { on_message(json); }
    // 把标记价的收包时间往前推，用来构造"WebSocket 半开、缓存里是冻结价"
    // 这个最凶险的场景——真等 10 秒会让测试慢得没人愿意跑
    void age_mark_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it != cache_.end()) it->second.mark_ms = now_ms() - age_ms;
    }
    size_t stream_count_for_test() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return streams_.size();
    }

private:
    void on_open   ();
    void on_message(const std::string& json);
    // 批量订阅/退订。合并成一条消息而不是一条流一条消息：币安合约 WS 限制
    // 【每秒最多 10 条入站消息】，超了直接断连——逐条发的话连续加几个品种就触线
    void send_subs (const std::vector<std::string>& streams, bool sub);
    void resubscribe_all();
    // 一个品种对应的全部流名（小写）
    static std::vector<std::string> streams_of(const std::string& symbol);

    static std::string to_lower(std::string s);

    bool testnet_;
    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> running_  {false};
    std::atomic<bool> connected_{false};
    std::atomic<int>  req_id_   {1};
    std::atomic<bool> first_data_seen_{false};   // 首包只提示一次
    std::atomic<int64_t> last_data_ms_{0};       // 最后一次收到数据包的时刻
    std::atomic<bool> silence_warned_{false};    // 静默告警只报一次，恢复后重新武装


    mutable std::mutex                    mtx_;
    std::set<std::string>                 streams_;   // lowercase "btcusdt@markPrice@1s"
    std::unordered_map<std::string, Tick> cache_;     // UPPER symbol → Tick

    LogCb  srv_cb_;
};

} // namespace ccbot
