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

void BookTickerStream::on_server_msg(LogCb cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    srv_cb_ = std::move(cb);
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
    std::vector<std::string> to_sub;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        to_sub.assign(streams_.begin(), streams_.end());
    }
    if (to_sub.empty()) return;

    // 币安合约 WS 单连接最多 200 条流（1024 是【现货】的数字，别搞混）。
    // 每品种 3 条流 ⇒ 约 66 个品种到顶。超了要么整条 SUBSCRIBE 被拒、
    // 要么连接被断，而两种情况此前都是静默的——所以在这里就说清楚
    if (to_sub.size() > 200) {
        LogCb cb;
        { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
        if (cb) cb("⚠ 行情WS流数量 " + std::to_string(to_sub.size()) +
                   " 超过币安合约单连接上限 200（每品种3条流≈66个品种），"
                   "超出部分不会有行情");
    }
    send_subs(to_sub, true);
}

// ── 订阅 / 取消 ───────────────────────────────────────────────────────────────
// 每个品种两条流。逐品种订阅而不是全市场的 !markPrice@arr@1s / !ticker@arr：
// 后者每秒推送【全部】约五百个合约，盯十个品种的场景下 99% 的带宽是白扔的。
// （反过来，将来做全市场扫描时那两条合并流才是对的——总数与品种数无关。）
std::vector<std::string> BookTickerStream::streams_of(const std::string& symbol) {
    const std::string s = to_lower(symbol);
    return { s + "@markPrice@1s",   // 标记价：引擎决策、强平距离、界面显示
             s + "@ticker" };        // 24h 滚动涨幅：高位拦截
}

void BookTickerStream::subscribe(const std::string& symbol) {
    std::vector<std::string> fresh;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& s : streams_of(symbol))
            if (streams_.insert(s).second) fresh.push_back(s);
    }
    // 合并成一条 SUBSCRIBE。逐条发的话币安合约 WS 的
    // 【每秒 10 条入站消息】限制很容易触线，超了直接断连
    if (connected_.load() && !fresh.empty()) send_subs(fresh, true);
}

void BookTickerStream::unsubscribe(const std::string& symbol) {
    std::vector<std::string> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& s : streams_of(symbol))
            if (streams_.erase(s) > 0) gone.push_back(s);
        cache_.erase(symbol);
    }
    if (connected_.load() && !gone.empty()) send_subs(gone, false);
}

void BookTickerStream::send_subs(const std::vector<std::string>& streams, bool sub) {
    if (streams.empty()) return;
    std::ostringstream params;
    bool first = true;
    for (const auto& s : streams) {
        if (!first) params << ',';
        params << '"' << s << '"';
        first = false;
    }
    std::string msg = std::string("{\"method\":\"") + (sub ? "SUBSCRIBE" : "UNSUBSCRIBE")
                    + "\",\"params\":[" + params.str()
                    + "],\"id\":" + std::to_string(req_id_++) + "}";
    ws_->send(msg);
}

// ── 消息解析 ──────────────────────────────────────────────────────────────────
void BookTickerStream::on_message(const std::string& json) {
    if (json.find("\"stream\"") == std::string::npos) {
        // 不是数据包。{"result":null,"id":N} 是正常的订阅确认，安静丢掉；
        // 其余（{"code":2,"msg":"Invalid request..."} 之类）必须让人看见——
        // 这里此前一律静默返回，某条流没订上时界面只是空白，无从查起
        if (json.find("\"result\":null") == std::string::npos) {
            LogCb cb;
            { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
            if (cb) cb("行情WS服务端消息: " + json.substr(0, 300));
        }
        return;
    }
    if (json.size() < 20) return;

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(json);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return;

    // combined stream 格式: {"stream":"btcusdt@markPrice@1s","data":{...}}
    simdjson::dom::element data;
    if (doc["data"].get(data) != simdjson::SUCCESS) return;

    std::string_view ev;
    if (data["e"].get(ev) != simdjson::SUCCESS) return;

    std::string_view sym_sv;
    if (data["s"].get(sym_sv) != simdjson::SUCCESS) return;
    std::string symbol(sym_sv);

    if (ev == "markPriceUpdate") {
        // {"e":"markPriceUpdate","s":"BTCUSDT","p":"<标记价>","i":"<指数价>",...}
        std::string_view p_sv;
        if (data["p"].get(p_sv) != simdjson::SUCCESS) return;
        const double mp = safe_stod(p_sv);
        if (mp <= 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        // 两条流写同一个缓存条目，各自【只更新自己那几个字段】，
        // 不构造全新 Tick 覆盖——否则一条流每来一包就把另一条的数据抹成 0
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
        // 同样只碰自己的两个字段：不能让每秒一次的涨幅去刷新标记价的收包时间，
        // 否则 markPrice 断流时陈旧保护会被这条流一直"续命"
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

double BookTickerStream::mark_price(const std::string& symbol) const {
    auto t = get(symbol);
    if (t.mark_price <= 0) return 0.0;
    // 陈旧保护：WS 半开/静默断流时缓存里躺着一个冻结价，看起来完全正常，
    // 引擎会拿着僵尸价继续补仓/止盈/止损，实际行情暴跌时完全失明。
    // markPrice@1s 每秒一包，10 秒没来就是这条流断了，调用方会转 REST 兜底
    if (now_ms() - t.mark_ms > kStaleMs) return 0.0;
    return t.mark_price;
}

void BookTickerStream::set_mark_price(const std::string& symbol, double price) {
    if (price <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto& c = cache_[symbol];
    c.symbol     = symbol;
    c.mark_price = price;
    c.mark_ms    = now_ms();   // 与流来的包同等对待，陈旧保护照常生效
}

bool BookTickerStream::change_24h(const std::string& symbol, double& out_pct) const {
    auto t = get(symbol);
    // chg_ms==0 表示这条流一次都没到过。涨幅本身可以合法地为 0 或负数，
    // 所以不能用数值判有无，只能看有没有收过包
    if (t.chg_ms == 0) return false;
    // 容忍度给到 60 秒：24h 涨幅本身是慢变量，几十秒的陈旧不影响
    // "最近涨得多急"这个判断，没必要跟标记价用同一个 10 秒阈值
    if (now_ms() - t.chg_ms > 60000) return false;
    out_pct = t.chg_24h;
    return true;
}

} // namespace ccbot
