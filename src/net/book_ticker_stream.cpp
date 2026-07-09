#include "net/book_ticker_stream.h"
#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>
#include <chrono>
#include <algorithm>
#include <sstream>

namespace ccbot {

// ── 工具 ──────────────────────────────────────────────────────────────────────
int64_t BookTickerStream::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string BookTickerStream::to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static double safe_stod(std::string_view sv) {
    try { return std::stod(std::string(sv)); } catch (...) { return 0.0; }
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
BookTickerStream::BookTickerStream(bool testnet)
    : testnet_(testnet), ws_(std::make_unique<ix::WebSocket>()) {}

BookTickerStream::~BookTickerStream() { stop(); }

void BookTickerStream::on_tick(TickCb cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    tick_cb_ = std::move(cb);
}

// ── 启动 / 停止 ───────────────────────────────────────────────────────────────
void BookTickerStream::start() {
    if (running_.load()) return;
    running_.store(true);

    // 连接到 combined stream 端点，SUBSCRIBE 消息动态添加流
    std::string url = testnet_
        ? "wss://stream.binancefuture.com/stream"
        : "wss://fstream.binance.com/stream";

    ws_->setUrl(url);
    ws_->setPingInterval(20);
    ws_->enableAutomaticReconnection();
    ws_->setMaxWaitBetweenReconnectionRetries(3000);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            connected_.store(true);
            on_open();
            break;
        case ix::WebSocketMessageType::Close:
        case ix::WebSocketMessageType::Error:
            connected_.store(false);
            break;
        case ix::WebSocketMessageType::Message:
            on_message(msg->str);
            break;
        default: break;
        }
    });

    ws_->start();
}

void BookTickerStream::stop() {
    if (!running_.load()) return;
    running_.store(false);
    connected_.store(false);
    ws_->stop();
}

// ── 重连后重新订阅所有已注册流 ────────────────────────────────────────────────
void BookTickerStream::on_open() {
    resubscribe_all();
}

void BookTickerStream::resubscribe_all() {
    std::set<std::string> to_sub;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        to_sub = streams_;
    }
    if (to_sub.empty()) return;

    // 批量 SUBSCRIBE（一条消息）
    std::ostringstream params;
    bool first = true;
    for (const auto& s : to_sub) {
        if (!first) params << ',';
        params << '"' << s << '"';
        first = false;
    }
    std::string msg = "{\"method\":\"SUBSCRIBE\",\"params\":[" + params.str()
                    + "],\"id\":" + std::to_string(req_id_++) + "}";
    ws_->send(msg);
}

// ── 订阅 / 取消 ───────────────────────────────────────────────────────────────
// 每个品种只订阅 bookTicker 一个流（买一/卖一）。最新成交价直接取买一卖一
// 中间价，不再单独订阅 ticker/aggTrade 流——减少连接数，界面也更稳定。
void BookTickerStream::subscribe(const std::string& symbol) {
    std::string s_book = to_lower(symbol) + "@bookTicker";
    bool need_book;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        need_book = streams_.insert(s_book).second;
    }
    if (connected_.load() && need_book) send_sub(s_book, true);
}

void BookTickerStream::unsubscribe(const std::string& symbol) {
    std::string s_book = to_lower(symbol) + "@bookTicker";
    bool had_book;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        had_book = streams_.erase(s_book) > 0;
        cache_.erase(symbol);
    }
    if (connected_.load() && had_book) send_sub(s_book, false);
}

void BookTickerStream::send_sub(const std::string& stream, bool sub) {
    std::string method = sub ? "SUBSCRIBE" : "UNSUBSCRIBE";
    std::string msg = "{\"method\":\"" + method + "\","
                      "\"params\":[\"" + stream + "\"],"
                      "\"id\":" + std::to_string(req_id_++) + "}";
    ws_->send(msg);
}

// ── 消息解析 ──────────────────────────────────────────────────────────────────
void BookTickerStream::on_message(const std::string& json) {
    // 过滤订阅响应（{"result":null,"id":1}）
    if (json.size() < 20 || json.find("\"stream\"") == std::string::npos) return;

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(json);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return;

    // combined stream 格式: {"stream":"btcusdt@bookTicker","data":{...}}
    simdjson::dom::element data;
    if (doc["data"].get(data) != simdjson::SUCCESS) return;

    std::string_view ev;
    if (data["e"].get(ev) != simdjson::SUCCESS) return;

    std::string_view sym_sv;
    if (data["s"].get(sym_sv) != simdjson::SUCCESS) return;
    std::string symbol(sym_sv);

    if (ev == "bookTicker") {
        std::string_view b_sv, B_sv, a_sv, A_sv;
        data["b"].get(b_sv);
        data["B"].get(B_sv);
        data["a"].get(a_sv);
        data["A"].get(A_sv);

        Tick t;
        TickCb cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            t.symbol  = symbol;
            t.bid     = safe_stod(b_sv);
            t.bid_qty = safe_stod(B_sv);
            t.ask     = safe_stod(a_sv);
            t.ask_qty = safe_stod(A_sv);
            t.recv_ms = now_ms();
            t.valid   = (t.bid > 0 && t.ask > 0);
            // 最新成交价：买一卖一中间价（不再单独订阅 ticker 流）
            t.last_price = t.valid ? (t.bid + t.ask) / 2.0 : 0.0;
            cache_[symbol] = t;
            cb = tick_cb_;
        }
        if (cb && t.valid) cb(t);
    }
}

// ── 缓存读取 ──────────────────────────────────────────────────────────────────
BookTickerStream::Tick BookTickerStream::get(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cache_.find(symbol);
    return (it != cache_.end()) ? it->second : Tick{};
}

double BookTickerStream::mid_price(const std::string& symbol) const {
    auto t = get(symbol);
    return (t.valid) ? (t.bid + t.ask) / 2.0 : 0.0;
}

} // namespace ccbot
