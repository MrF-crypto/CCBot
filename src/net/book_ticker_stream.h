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
        double last_price = 0;   // 最新成交价（取买一/卖一中间价，界面展示用）
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

    void on_tick(TickCb cb);   // 每条消息回调（运行在 WS 线程）

    static int64_t now_ms();

private:
    void on_open   ();
    void on_message(const std::string& json);
    void send_sub  (const std::string& stream, bool sub);
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
};

} // namespace ccbot
