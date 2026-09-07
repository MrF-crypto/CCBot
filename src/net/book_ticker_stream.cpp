#include "net/book_ticker_stream.h"
#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <thread>

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

    // 连接状态此前【完全不可见】：既没日志也没界面展示。
    // 后果是行情不来时无从判断是"没连上/订阅没生效/数据没来"三者中的哪一种，
    // 而三者的修法完全不同。更糟的是标记价有 REST 兜底、日线走 REST，
    // WS 死了也照常有价——唯独 24h 涨幅没有兜底，于是只有它会暴露问题，
    // 却又被误读成"这一条数据有问题"
    auto say = [this](const std::string& m) {
        LogCb cb;
        { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
        if (cb) cb(m);
    };

    ws_->setOnMessageCallback([this, say](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            connected_.store(true);
            size_t n = 0;
            { std::lock_guard<std::mutex> lk(mtx_); n = streams_.size(); }
            say("行情WS已连接，正在订阅 " + std::to_string(n) + " 条流（"
                + std::to_string(n / 2) + " 个品种 × 2）");
            on_open();
            break;
        }
        case ix::WebSocketMessageType::Close:
            connected_.store(false);
            say("行情WS连接已断开（会自动重连）");
            break;
        case ix::WebSocketMessageType::Error:
            connected_.store(false);
            say("行情WS连接错误: " + msg->errorInfo.reason
                + "（HTTP " + std::to_string(msg->errorInfo.http_status) + "）");
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
    // 每品种 2 条流 ⇒ 100 个品种到顶。超了要么整条 SUBSCRIBE 被拒、
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
    return { s + "@markPrice@1s",   // 标记价：引擎首选价、强平距离、界面显示
             s + "@ticker",          // 24h 滚动涨幅：高位拦截
             s + "@bookTicker" };    // 中间价：markPrice 收不到时的降级价格源
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
    // 分批发送。47 个品种 × 2 条流 = 94 个流名塞进一条 SUBSCRIBE，实盘表现为
    // 【连接成功、服务端不报错、数据永远不来】——而同一套机制在 47 条流
    // （v4.0.8 只订 bookTicker）时是正常收数据的。唯一的变量就是数量，
    // 所以按批发，一批 kSubBatch 条。
    //
    // 批之间必须留间隔：币安合约 WS 限【每秒 10 条入站消息】，超了直接断连。
    // 这里跑在 WS 回调线程上，短暂 sleep 不影响下单路径（那在另一个线程池）。
    const size_t kSubBatch = 20;
    const char* method = sub ? "SUBSCRIBE" : "UNSUBSCRIBE";
    size_t sent_batches = 0;
    for (size_t i = 0; i < streams.size(); i += kSubBatch) {
        const size_t end = std::min(i + kSubBatch, streams.size());
        std::ostringstream params;
        for (size_t k = i; k < end; ++k) {
            if (k > i) params << ',';
            params << '"' << streams[k] << '"';
        }
        std::string msg = std::string("{\"method\":\"") + method
                        + "\",\"params\":[" + params.str()
                        + "],\"id\":" + std::to_string(req_id_++) + "}";
        auto info = ws_->send(msg);
        // send 的返回值此前【被丢弃】：发送失败时既没有日志也没有重试，
        // 表现为"订阅了但没数据"，与订阅被拒完全无法区分
        if (!info.success) {
            LogCb cb;
            { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
            if (cb) cb("行情WS订阅消息发送失败（第 " + std::to_string(sent_batches + 1)
                       + " 批，" + std::to_string(end - i) + " 条流）");
        }
        ++sent_batches;
        // 每批之间隔 150ms：10 条/秒的限制下，20 条一批也远远够用
        if (end < streams.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
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

    // 首包提示（只发一次）。有了它，"WS已连接"之后到底有没有数据就一目了然：
    //   有"已连接"没"首包"  → 订阅没生效
    //   连"已连接"都没有     → 压根没连上
    //   两条都有             → 数据在流，问题在别处
    if (!first_data_seen_.exchange(true)) {
        LogCb cb;
        { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
        if (cb) cb("行情WS收到首个数据包，订阅生效");
    }
    last_data_ms_.store(now_ms(), std::memory_order_relaxed);

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
    } else if (ev == "bookTicker") {
        // {"e":"bookTicker","s":"BTCUSDT","b":"买一","B":"量","a":"卖一","A":"量"}
        std::string_view b_sv, a_sv;
        data["b"].get(b_sv);
        data["a"].get(a_sv);
        const double b = safe_stod(b_sv), a = safe_stod(a_sv);
        // 只有一边有效时中间价会是真实价格的一半——那会让引擎以为价格瞬间腰斩，
        // 直接触发深层补仓。两边都必须有效才写
        if (b <= 0 || a <= 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto& c = cache_[symbol];
        c.symbol = symbol;
        c.bid = b; c.ask = a;
        c.mid = (b + a) / 2.0;
        c.mid_ms = now_ms();
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

double BookTickerStream::mid_price(const std::string& symbol) const {
    auto t = get(symbol);
    if (t.mid <= 0) return 0.0;
    // 与标记价同一把尺子。bookTicker 是逐笔推送，10 秒没来就是这条流也断了
    if (now_ms() - t.mid_ms > kStaleMs) return 0.0;
    return t.mid;
}

double BookTickerStream::live_price(const std::string& symbol, PxSrc& out_src) const {
    // 先标记价：它与强平价、未实现盈亏同体系，是"正确"的那个。
    // 拿不到才退到中间价——某些链路只放行 bookTicker，那时中间价是
    // 唯一的实时行情，比退到 REST（47 品种串行、每个约 10 秒一次）好两个数量级
    if (double p = mark_price(symbol); p > 0) { out_src = PxSrc::Mark; return p; }
    if (double p = mid_price(symbol);  p > 0) { out_src = PxSrc::Mid;  return p; }
    out_src = PxSrc::None;
    return 0.0;
}

const char* BookTickerStream::px_src_name(PxSrc s) {
    switch (s) {
    case PxSrc::Mark: return "标记价(WS)";
    case PxSrc::Mid:  return "中间价(WS bookTicker)";
    default:          return "REST兜底";
    }
}

std::string BookTickerStream::silence_check(int64_t quiet_ms) {
    if (!running_.load()) return {};
    if (!connected_.load()) return {};           // 没连上是另一回事，Close/Error 已有日志
    const int64_t last = last_data_ms_.load(std::memory_order_relaxed);
    size_t n = 0;
    { std::lock_guard<std::mutex> lk(mtx_); n = streams_.size(); }
    if (n == 0) return {};                        // 没订阅任何流，静默是正常的

    if (last == 0) {
        // 连上之后一个包都没来过——最凶险的一种：三个"正常"信号加起来仍是完全静默
        if (silence_warned_.exchange(true)) return {};
        return "⚠ 行情WS已连接且已发出 " + std::to_string(n) +
               " 条流的订阅，但至今【一个数据包都没收到】。"
               "策略会退化到 REST 兜底（更慢、更耗权重），24h 涨幅等推送专属数据可能缺失。";
    }
    const int64_t age = now_ms() - last;
    if (age > quiet_ms) {
        if (silence_warned_.exchange(true)) return {};
        return "⚠ 行情WS已 " + std::to_string(age / 1000) + " 秒没有收到任何数据包（连接仍显示已建立）";
    }
    silence_warned_.store(false);                 // 恢复了就重新武装，下次静默还会报
    return {};
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
