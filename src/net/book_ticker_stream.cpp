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
    // 24h 滚动涨幅。没有别的免费来源：日线 K 线只能算出"今日涨幅"（每天 UTC 0 点
    // 归零），要真正的滚动 24 小时得按小时线回看 24 根，那是每品种一次额外 REST；
    // 而 @ticker 的 P 字段就是币安官方的 24h 滚动涨幅，推送式、零请求成本
    std::string s_tick = to_lower(symbol) + "@ticker";
    bool need_book, need_mark, need_tick;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        need_book = streams_.insert(s_book).second;
        need_mark = streams_.insert(s_mark).second;
        need_tick = streams_.insert(s_tick).second;
    }
    if (connected_.load()) {
        if (need_book) send_sub(s_book, true);
        if (need_mark) send_sub(s_mark, true);
        if (need_tick) send_sub(s_tick, true);
    }
}

void BookTickerStream::unsubscribe(const std::string& symbol) {
    std::string s_book = to_lower(symbol) + "@bookTicker";
    std::string s_mark = to_lower(symbol) + "@markPrice@1s";
    std::string s_tick = to_lower(symbol) + "@ticker";
    bool had_book, had_mark, had_tick;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        had_book = streams_.erase(s_book) > 0;
        had_mark = streams_.erase(s_mark) > 0;
        had_tick = streams_.erase(s_tick) > 0;
        cache_.erase(symbol);
    }
    if (connected_.load()) {
        if (had_book) send_sub(s_book, false);
        if (had_mark) send_sub(s_mark, false);
        if (had_tick) send_sub(s_tick, false);
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
    } else if (ev == "24hrTicker") {
        // {"e":"24hrTicker","s":"BTCUSDT","P":"<24h涨幅%>","c":"<最新成交价>",...}
        // 只取 P。这条流的其它字段（最新成交价、成交量）目前无人使用——
        // 订阅它纯粹是为了拿到滚动 24h 涨幅，日线 K 线给不出这个数
        std::string_view P_sv;
        if (data["P"].get(P_sv) != simdjson::SUCCESS) return;
        // 涨幅可以是负数也可以恰好是 0，不能像价格那样用 ">0" 判合法。
        // 用"解析后字符串非空且不是纯垃圾"来判：safe_stod 失败返回 0，
        // 而真实的 0 涨幅同样是 0——两者无法区分，所以直接检查原始字符串
        if (P_sv.empty()) return;
        std::lock_guard<std::mutex> lk(mtx_);
        // 同样不碰 valid/recv_ms：陈旧判定归 bookTicker 那条流管
        auto& c = cache_[symbol];
        c.symbol  = symbol;
        c.chg_24h = safe_stod(P_sv);
        c.chg_ms  = now_ms();
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

double BookTickerStream::mark_price(const std::string& symbol) const {
    auto t = get(symbol);
    if (t.mark_price <= 0) return 0.0;
    // 陈旧保护按【标记价自己的】收包时间判，不能用 recv_ms——那是 bookTicker 的。
    // markPrice@1s 每秒一包，10 秒没来就是这条流断了，此时 bookTicker 可能还活着，
    // 拿冻结的标记价继续决策与拿冻结的中间价一样危险
    if (now_ms() - t.mark_ms > 10000) return 0.0;
    return t.mark_price;
}

bool BookTickerStream::change_24h(const std::string& symbol, double& out_pct) const {
    auto t = get(symbol);
    // chg_ms==0 表示这条流一次都没到过。涨幅本身可以合法地为 0 或负数，
    // 所以不能用数值判有无，只能看有没有收过包
    if (t.chg_ms == 0) return false;
    // @ticker 是每秒一推，容忍度给到 60 秒——它比 bookTicker 慢，
    // 且 24h 涨幅本身是慢变量，几十秒的陈旧不影响"最近涨得多急"这个判断
    if (now_ms() - t.chg_ms > 60000) return false;
    out_pct = t.chg_24h;
    return true;
}

} // namespace ccbot
