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
    // 标记价单独一条流。用逐品种的 @markPrice@1s 而不是全市场的
    // !markPrice@arr@1s：后者每秒推送【全部】约五百个合约，盯十个品种的场景下
    // 99% 的带宽是白扔的。逐品种与 bookTicker 的订阅模式也一致。
    // 一条连接最多 1024 个流，每品种两条流对实际用量来说远远够
    std::string s_mark = to_lower(symbol) + "@markPrice@1s";
    bool need_book, need_mark;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        need_book = streams_.insert(s_book).second;
        need_mark = streams_.insert(s_mark).second;
    }
    if (connected_.load()) {
        if (need_book) send_sub(s_book, true);
        if (need_mark) send_sub(s_mark, true);
    }
}

void BookTickerStream::unsubscribe(const std::string& symbol) {
    std::string s_book = to_lower(symbol) + "@bookTicker";
    std::string s_mark = to_lower(symbol) + "@markPrice@1s";
    bool had_book, had_mark;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        had_book = streams_.erase(s_book) > 0;
        had_mark = streams_.erase(s_mark) > 0;
        cache_.erase(symbol);
    }
    if (connected_.load()) {
        if (had_book) send_sub(s_book, false);
        if (had_mark) send_sub(s_mark, false);
    }
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
            // 先取回已有条目：标记价来自另一条流，这里【不能】用全新 Tick 覆盖，
            // 否则 bookTicker 每来一包就把标记价抹成 0（bookTicker 是逐笔、
            // markPrice 是每秒一次，抹掉的概率接近 100%）
            t = cache_[symbol];
            t.symbol  = symbol;
            t.bid     = safe_stod(b_sv);
            t.bid_qty = safe_stod(B_sv);
            t.ask     = safe_stod(a_sv);
            t.ask_qty = safe_stod(A_sv);
            t.recv_ms = now_ms();
            t.valid   = (t.bid > 0 && t.ask > 0);
            // 中间价（字段名 last_price 是历史遗留，它不是最新成交价）
            t.last_price = t.valid ? (t.bid + t.ask) / 2.0 : 0.0;
            cache_[symbol] = t;
            cb = tick_cb_;
        }
        if (cb && t.valid) cb(t);
    } else if (ev == "markPriceUpdate") {
        // {"e":"markPriceUpdate","s":"BTCUSDT","p":"<标记价>","i":"<指数价>",...}
        std::string_view p_sv;
        if (data["p"].get(p_sv) != simdjson::SUCCESS) return;
        const double mp = safe_stod(p_sv);
        if (mp <= 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        // 同理：只更新标记价两个字段，不碰 bid/ask/valid/recv_ms。
        // 尤其不能动 valid 和 recv_ms —— 陈旧判定靠它们，
        // 让每秒一次的标记价去刷新"行情新鲜度"会掩盖 bookTicker 已经断流
        auto& c = cache_[symbol];
        c.symbol     = symbol;
        c.mark_price = mp;
        c.mark_ms    = now_ms();
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
    if (!t.valid) return 0.0;
    // 陈旧保护：WS 半开/静默断流时 cache 里是冻结价（valid 永远为 true）——
    // 超过10秒没有新包就视为无效，调用方会走 REST 标记价兜底。
    // 否则引擎会拿"僵尸价格"做补仓/止盈/止损判定，实际行情暴跌时完全失明
    if (now_ms() - t.recv_ms > 10000) return 0.0;
    return (t.bid + t.ask) / 2.0;
}

} // namespace ccbot
