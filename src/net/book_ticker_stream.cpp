#include "net/book_ticker_stream.h"
#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <string_view>
#include <utility>

namespace ccbot {

// ── 工具 ──────────────────────────────────────────────────────────────────────
int64_t BookTickerStream::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string BookTickerStream::to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return s;
}

std::string BookTickerStream::to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(::toupper(c)); });
    return s;
}

static double safe_stod(std::string_view sv) {
    try { return std::stod(std::string(sv)); } catch (...) { return 0.0; }
}

// ── Conn ──────────────────────────────────────────────────────────────────────
// 构造/析构放在 cpp：头文件里 ix::WebSocket 只是前置声明，unique_ptr 的析构
// 需要完整类型
BookTickerStream::Conn::Conn() : ws(std::make_unique<ix::WebSocket>()) {}
BookTickerStream::Conn::~Conn() = default;

// ── 健康快照的一行结论 ────────────────────────────────────────────────────────
std::string BookTickerStream::Health::summary() const {
    std::ostringstream o;
    o << (connected ? "已连接" : "未连接");
    if (conns > 1) o << '(' << conns_up << '/' << conns << "条)";
    else if (connected && uptime_ms >= 0) o << '(' << (uptime_ms / 1000) << "s)";
    if (silence_ms >= 0) {
        o << " 收包" << std::fixed << std::setprecision(1)
          << (static_cast<double>(silence_ms) / 1000.0) << "s前";
    } else {
        o << " 从未收包";
    }
    o << " 品种" << symbols
      << "(新鲜" << fresh << "/陈旧" << stale << "/无数据" << never << ')'
      << " 流" << streams;
    // 涨幅流单独报。它和标记价同连接，但币安可以只丢掉其中一条订阅，
    // 那时 markPrice 全绿而高位拦截已经悄悄失效
    if (chg_stale > 0 || chg_never > 0)
        o << " 涨幅异常" << (chg_stale + chg_never);
    if (connects > static_cast<int>(conns)) o << " 重连" << (connects - static_cast<int>(conns)) << "次";
    if (watchdog_kicks > 0) o << " 看门狗" << watchdog_kicks << "次";
    if (gaps > 0)           o << " 丢包" << gaps << "处";
    if (over_cap)           o << " ⚠超流上限";
    return o.str();
}

// ── 纯函数判定 ────────────────────────────────────────────────────────────────
// 为什么抽出来：见头文件。三个判断的误判都不报错，必须用断言钉住。
bool BookTickerStream::should_kick(int64_t now, int64_t last_msg_ms,
                                   int64_t conn_since_ms, bool connected,
                                   size_t stream_n) {
    // 没连上时重连是库的事，掀它没有意义（也没有东西可掀）
    if (!connected) return false;
    // 这条连接上一条流都没有：本来就不该有数据，不是故障。
    // 少了这条，空闲连接（品种全退订后留下的空壳）会被无限重连
    if (stream_n == 0) return false;
    // 状态不完整（刚构造/刚被掀过），等下一轮
    if (conn_since_ms <= 0) return false;
    // 刚连上还没收到第一包时，从【连接建立】起算而不是从 0 起算——
    // 否则首次连接的瞬间就会被判成静默 (now - 0 必然巨大)
    const int64_t base = (last_msg_ms > conn_since_ms) ? last_msg_ms : conn_since_ms;
    return now - base > kSilenceMs;
}

bool BookTickerStream::should_warn_no_data(int64_t now, int64_t last_msg_ms,
                                           int64_t conn_since_ms, bool connected,
                                           size_t stream_n) {
    if (!connected || stream_n == 0) return false;
    if (conn_since_ms <= 0) return false;
    // 本次连接内收到过数据 ⇒ 订阅生效了，没什么要提示的。
    // 用 >= 而不是 >：包和 Open 落在同一毫秒是可能的
    if (last_msg_ms >= conn_since_ms) return false;
    return now - conn_since_ms > kSubGraceMs;
}

size_t BookTickerStream::pick_conn(const std::vector<size_t>& counts,
                                   size_t need, size_t cap) {
    if (need == 0) return counts.size();
    // 首次适配。刻意不做"最空优先"：品种通常是一次性批量加进来的，
    // 首次适配会把它们紧密排在前面的连接上，连接数最少；
    // 而均摊会让每条连接都半满，白白多开连接（每条连接都是一次握手、
    // 一份心跳、一个重连时的 IP 频次配额）
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] + need <= cap) return i;
    return counts.size();   // 都装不下 → 要新建一条
}

// ── 构造 / 析构 ───────────────────────────────────────────────────────────────
// 两个构造函数而不是一个带默认实参的，理由见头文件（嵌套类的默认成员初始化器
// 不能出现在外层类体内的默认实参位置）
BookTickerStream::BookTickerStream(bool testnet)
    : testnet_(testnet), feeds_(Feeds{}) {}

BookTickerStream::BookTickerStream(bool testnet, Feeds feeds)
    : testnet_(testnet), feeds_(feeds) {}

BookTickerStream::~BookTickerStream() { stop(); }

void BookTickerStream::on_server_msg(LogCb cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    srv_cb_ = std::move(cb);
}

// 取出回调再调用，不在持锁状态下回调：调用方的日志函数可能反过来读这个对象
void BookTickerStream::say(const std::string& m) const {
    LogCb cb;
    { std::lock_guard<std::mutex> lk(mtx_); cb = srv_cb_; }
    if (cb) cb(m);
}

// ── 连接装配 ──────────────────────────────────────────────────────────────────
void BookTickerStream::wire(Conn* c) {
    // 连接到 combined stream 端点，SUBSCRIBE 消息动态添加流
    const std::string url = testnet_
        ? "wss://stream.binancefuture.com/stream"
        : "wss://fstream.binance.com/stream";
    c->ws->setUrl(url);
    c->ws->setPingInterval(20);
    c->ws->enableAutomaticReconnection();
    c->ws->setMaxWaitBetweenReconnectionRetries(3000);

    // 连接状态此前【完全不可见】：既没日志也没界面展示。
    // 后果是行情不来时无从判断是"没连上/订阅没生效/数据没来"三者中的哪一种，
    // 而三者的修法完全不同。更糟的是标记价有 REST 兜底、日线走 REST，
    // WS 死了也照常有价——唯独 24h 涨幅没有兜底，于是只有它会暴露问题，
    // 却又被误读成"这一条数据有问题"
    const std::string tag = "行情WS#" + std::to_string(c->id);
    c->ws->setOnMessageCallback([this, c, tag](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            const int64_t t = now_ms();
            c->connected.store(true);
            c->conn_since_ms.store(t);
            // ⚠ 每次连接都要重置这两个 latch，理由见头文件 Conn
            c->data_seen.store(false);
            c->nodata_warned.store(false);
            c->ctrl_msgs.store(0);
            const int n_conn = ++c->connects;
            // ⚠ 这里【不能】清 kicks_since_ok。
            //   它原本写的是"连上了就算自愈成功"，但那个前提在真实故障里不成立：
            //   代理/TUN 对任何 fake-IP 的 TCP 握手都会成功，于是每次 close()
            //   之后都能重新 Open、计数每轮归零，kKickEscalate 那条"重建连接"
            //   的升级路径【永远不会触发】——而它恰恰就是为"能连上但没数据"
            //   写的。实测日志里连掀 11 次、每次都重连成功、一次都没升级。
            //   真正的自愈标志是【收到数据包】，所以改到 on_message 里清
            connected_.store(true);       // 粗粒度标志；pump 每轮会校正

            size_t n_streams = 0;
            { std::lock_guard<std::mutex> lk(mtx_); n_streams = c->streams.size(); }
            say(tag + (n_conn > 1 ? " 已重连（第" + std::to_string(n_conn) + "次连接）"
                                  : std::string(" 已连接"))
                + "，正在订阅 " + std::to_string(n_streams) + " 条流");
            on_open(c);
            break;
        }
        case ix::WebSocketMessageType::Close:
            c->connected.store(false);
            say(tag + " 连接已断开（会自动重连）");
            break;
        case ix::WebSocketMessageType::Error:
            c->connected.store(false);
            say(tag + " 连接错误: " + msg->errorInfo.reason
                + "（HTTP " + std::to_string(msg->errorInfo.http_status) + "）");
            break;
        case ix::WebSocketMessageType::Message:
            on_message(msg->str, c);
            break;
        default: break;
        }
    });
}

// ── 启动 / 停止 ───────────────────────────────────────────────────────────────
void BookTickerStream::start() {
    if (running_.load()) return;
    running_.store(true);

    // 典型调用顺序是 start() 再逐个 subscribe()，此时还没有任何连接——
    // 连接由 subscribe 按需创建并启动。这里只负责把【已存在的】连接拉起来
    std::vector<Conn*> to_start;
    { std::lock_guard<std::mutex> lk(mtx_); for (auto& c : conns_) to_start.push_back(c.get()); }
    for (Conn* c : to_start) c->ws->start();

    pump_ = std::thread([this] { pump_loop(); });
}

void BookTickerStream::stop() {
    if (!running_.load()) return;
    running_.store(false);
    connected_.store(false);
    cv_.notify_all();                       // 让 pump 立刻醒，不用等一个周期
    if (pump_.joinable()) pump_.join();     // 必须先收 pump：它会碰各连接的 ws

    std::vector<Conn*> all;
    { std::lock_guard<std::mutex> lk(mtx_); for (auto& c : conns_) all.push_back(c.get()); }
    for (Conn* c : all) c->ws->stop();
}

// ── 看门狗 + 出站控制消息合并发送 ─────────────────────────────────────────────
// 两件事都是低频轮询，共用一个线程；各开一个只是多一个要 join 的东西。
void BookTickerStream::pump_loop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(cv_mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(kFlushMs),
                         [this] { return !running_.load(); });
        }
        if (!running_.load()) break;

        // ── ① 合并发送待办的订阅/退订，按连接分组 ─────────────────────────────
        std::vector<std::pair<Conn*, std::vector<std::string>>> out_sub, out_unsub;
        struct Snap { Conn* c; size_t n; };
        std::vector<Snap> snaps;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            std::unordered_map<int, std::vector<std::string>> by_del, by_add;
            // 退订要先分组【再】清 stream_conn_：归属信息是找到"该往哪条连接
            // 发 UNSUBSCRIBE"的唯一线索，清早了这条退订就发不出去，
            // 币安那边的订阅会一直留着，重连时又被全量重订顶回来
            for (const auto& s : want_unsub_) {
                auto it = stream_conn_.find(s);
                if (it != stream_conn_.end()) by_del[it->second].push_back(s);
            }
            for (const auto& s : want_unsub_) stream_conn_.erase(s);
            want_unsub_.clear();

            for (const auto& s : want_sub_) {
                auto it = stream_conn_.find(s);
                if (it != stream_conn_.end()) by_add[it->second].push_back(s);
            }
            want_sub_.clear();

            for (auto& kv : by_del)
                if (kv.first >= 0 && static_cast<size_t>(kv.first) < conns_.size())
                    out_unsub.emplace_back(conns_[static_cast<size_t>(kv.first)].get(),
                                           std::move(kv.second));
            for (auto& kv : by_add)
                if (kv.first >= 0 && static_cast<size_t>(kv.first) < conns_.size())
                    out_sub.emplace_back(conns_[static_cast<size_t>(kv.first)].get(),
                                         std::move(kv.second));

            for (auto& c : conns_) snaps.push_back({c.get(), c->streams.size()});
        }
        // 未连接的连接直接跳过：streams 已经记下了，它 Open 时 resubscribe_all
        // 会全量重订，留着反而会在重连后重复发一遍
        for (auto& kv : out_unsub) if (kv.first->connected.load()) send_subs(kv.first, kv.second, false);
        for (auto& kv : out_sub)   if (kv.first->connected.load()) send_subs(kv.first, kv.second, true);

        // ── ② 每条连接各自的静默看门狗 ────────────────────────────────────────
        // 必须逐条判：一条断了另一条好着是常态，用全局时间线的话
        // 好连接会被坏连接的静默拖着一起重连
        const int64_t now = now_ms();
        bool any_up = false;
        for (const auto& s : snaps) {
            Conn* c = s.c;
            const bool    conn = c->connected.load();
            const int64_t lm   = c->last_msg_ms.load();
            const int64_t cs   = c->conn_since_ms.load();
            if (conn) any_up = true;

            if (should_warn_no_data(now, lm, cs, conn, s.n)
                && !c->nodata_warned.exchange(true)) {
                // 分两种成因说，因为修法完全相反。判据是有没有收到过控制帧
                // （订阅确认）：币安对每条 SUBSCRIBE 必回 {"result":null,"id":N}
                const int ctrl = c->ctrl_msgs.load();
                const std::string head =
                    "⚠ 行情WS#" + std::to_string(c->id) + " 已连接 "
                    + std::to_string((now - cs) / 1000) + " 秒却一个行情包都没收到 —— ";
                if (ctrl > 0) {
                    say(head + "但服务端回了 " + std::to_string(ctrl) +
                        " 条控制消息，说明【连到币安了】，问题出在订阅上："
                        "多半是某条流名非法（币安会因为一条非法流名拒掉整条 "
                        "SUBSCRIBE，于是这条连接上的流一条都订不上）。"
                        "若上面有「服务端消息」，那就是原因");
                } else {
                    say(head + "连订阅确认都没回（币安对每条 SUBSCRIBE 必回 "
                        "{\"result\":null}）。这不是订阅问题——请求根本没到币安。"
                        "查代理/VPN 的分流规则是否覆盖 fstream.binance.com，"
                        "以及 DNS 是否把它解析到了 198.18/15 这类 fake-IP："
                        "那种地址上 TCP 握手必然成功，所以「连上了」说明不了任何事");
                }
            }

            if (should_kick(now, lm, cs, conn, s.n)) {
                const int64_t base = (lm > cs) ? lm : cs;
                force_reconnect(c, "连接看起来正常但已 "
                                   + std::to_string((now - base) / 1000) + " 秒");
            } else if (!conn && c->kicks_since_ok.load() > 0
                       && now - c->last_kick_ms.load() > kSilenceMs) {
                // 掀过一次但连接一直没回来。这种状态下 connected 恒为 false，
                // should_kick 永远不会再成立，只能靠这条分支升级处置
                force_reconnect(c, "强制重连后 "
                                   + std::to_string((now - c->last_kick_ms.load()) / 1000)
                                   + " 秒仍未恢复，");
            }
        }
        connected_.store(any_up);
    }
}

void BookTickerStream::force_reconnect(Conn* c, const std::string& why) {
    if (!running_.load()) return;

    const int kicks = ++wd_kicks_;
    const int since = ++c->kicks_since_ok;
    c->connected.store(false);
    c->last_kick_ms.store(now_ms());
    // 清掉上次连接的时间线，免得下一轮拿旧值再判一次
    c->last_msg_ms.store(0);
    c->conn_since_ms.store(0);

    const std::string tag = "行情WS#" + std::to_string(c->id);
    say("⚠ " + tag + " " + why + "没有任何数据包，判定为半开连接，强制重连（累计 "
        + std::to_string(kicks) + " 次）");

    // ⚠ 绝不能持 mtx_ 调 ws 的方法：close()/stop() 会等 WS 线程走完，
    //   而 WS 线程可能正在 on_message 里等 mtx_ —— 直接死锁。
    //   这里没有持锁（say 自己取放锁），保持这样
    if (since < kKickEscalate) {
        // 首选 close()：让库按 enableAutomaticReconnection 自己重拨，
        // 不动 WS 线程，代价最小
        c->ws->close();
    } else {
        // 连续 kKickEscalate 次 close() 都没换来一次成功连接，说明库没有替我们
        // 重拨。整条重建。把自愈全押在一个我们无法在单测里验证的库行为上是不行的，
        // 所以留了这条升级路径
        say(tag + " 连续 " + std::to_string(since) + " 次强制重连未恢复，改为重建连接");
        c->ws->stop();
        if (running_.load()) c->ws->start();
        // ⚠ 这里【不能】把 kicks_since_ok 清零。它只该由"成功连上"来清（见 Open
        //   分支）。在这里清的话，pump 里那条"掀过但连接一直没回来"的分支
        //   （条件含 kicks_since_ok > 0）就再也不会成立——重建失败一次之后
        //   彻底不再重试，等于把自愈只做了一半
    }
}

// ── 重连后重新订阅该连接上的全部流 ────────────────────────────────────────────
void BookTickerStream::on_open(Conn* c) {
    resubscribe_all(c);
}

void BookTickerStream::resubscribe_all(Conn* c) {
    std::vector<std::string> to_sub;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        to_sub.assign(c->streams.begin(), c->streams.end());
    }
    if (to_sub.empty()) return;
    if (to_sub.size() > kStreamCap) {
        say("⚠ 行情WS#" + std::to_string(c->id) + " 流数量 "
            + std::to_string(to_sub.size()) + " 超过币安合约单连接上限 "
            + std::to_string(kStreamCap) + "，超出部分不会有行情");
    }
    send_subs(c, to_sub, true);
}

// ── 订阅 / 取消 ───────────────────────────────────────────────────────────────
// 逐品种订阅而不是全市场的 !markPrice@arr@1s / !ticker@arr：后者每秒推送
// 【全部】约五百个合约，盯十个品种的场景下 99% 的带宽是白扔的。
// （反过来，将来做全市场扫描时那两条合并流才是对的——总数与品种数无关。）
std::vector<std::string> BookTickerStream::streams_of(const std::string& symbol) const {
    const std::string s = to_lower(symbol);
    std::vector<std::string> v;
    v.reserve(feeds_.per_symbol());
    v.push_back(s + "@markPrice@1s");          // 标记价：引擎决策、强平距离、界面
    v.push_back(s + "@ticker");                // 24h 滚动涨幅：高位拦截
    if (feeds_.agg_trade)   v.push_back(s + "@aggTrade");    // 成交价：短线信号
    if (feeds_.book_ticker) v.push_back(s + "@bookTicker");  // 买卖一：滑点估计
    return v;
}

void BookTickerStream::subscribe(const std::string& symbol) {
    const std::string up = to_upper(symbol);
    const auto names = streams_of(symbol);

    std::vector<Conn*> to_start;
    int  new_conn_id = -1;
    size_t n_streams = 0, n_syms = 0, n_conns = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        subs_.insert(up);

        std::vector<std::string> fresh;
        for (const auto& s : names)
            if (!streams_.count(s)) fresh.push_back(s);

        if (!fresh.empty()) {
            // ⚠ 按品种【整体】分片：这个品种的所有流必须落在同一条连接上。
            //   分开的话，一个品种的标记价可能在死连接上、成交价在活连接上，
            //   两者之间任何对齐（"这笔成交离强平多远"）都失去意义，
            //   而且 Health 也无法回答"这个品种到底能不能交易"
            std::vector<size_t> counts;
            counts.reserve(conns_.size());
            for (const auto& c : conns_) counts.push_back(c->streams.size());

            size_t idx = pick_conn(counts, fresh.size(), kStreamCap);
            if (idx >= conns_.size()) {
                auto nc = std::make_unique<Conn>();
                nc->id = static_cast<int>(conns_.size());
                wire(nc.get());
                new_conn_id = nc->id;
                idx = conns_.size();
                conns_.push_back(std::move(nc));
                // 已经在跑就立刻拉起；还没 start() 的话由 start() 统一拉
                if (running_.load()) to_start.push_back(conns_[idx].get());
            }
            Conn* c = conns_[idx].get();
            for (const auto& s : fresh) {
                streams_.insert(s);
                c->streams.insert(s);
                stream_conn_[s] = c->id;
                want_sub_.insert(s);
                want_unsub_.erase(s);   // 刚退订又加回来：订阅胜出
            }
        }
        n_streams = streams_.size();
        n_syms    = subs_.size();
        n_conns   = conns_.size();
    }

    // 建连接要在锁外：ws->start() 会拉起一个线程，那个线程可能立刻回调进
    // on_message 去抢 mtx_
    for (Conn* c : to_start) c->ws->start();

    // 新开连接是个值得说一声的事件：它解释了容量是怎么长上去的，
    // 也提醒币安对【单 IP 的建连频次】另有限制（每 5 分钟 300 次），
    // 连接数多 + 重连频繁时会撞上
    if (new_conn_id > 0) {
        say("行情WS 新开第 " + std::to_string(n_conns) + " 条连接（单连接上限 "
            + std::to_string(kStreamCap) + " 条流，当前 " + std::to_string(n_syms)
            + " 个品种 × " + std::to_string(feeds_.per_symbol()) + " 条 = "
            + std::to_string(n_streams) + " 条）");
    }
    // 不在这里直接发订阅。发送统一交给 pump 合并：币安合约 WS 限制每秒最多
    // 10 条入站消息，超了直接断连——而 send_subs 的合并只管"一条消息里多条流"，
    // 界面上连续添加 15 个品种就是 15 条消息在几十毫秒内发出去，照样触线
}

void BookTickerStream::unsubscribe(const std::string& symbol) {
    const std::string up = to_upper(symbol);
    std::lock_guard<std::mutex> lk(mtx_);
    subs_.erase(up);
    for (const auto& s : streams_of(symbol)) {
        if (streams_.erase(s) == 0) continue;
        auto it = stream_conn_.find(s);
        if (it != stream_conn_.end()) {
            const size_t ci = static_cast<size_t>(it->second);
            if (ci < conns_.size()) conns_[ci]->streams.erase(s);
            // ⚠ 保留 stream_conn_[s]：pump 要靠它知道该往哪条连接发 UNSUBSCRIBE。
            //   由 pump 在发出之后清掉
        }
        want_unsub_.insert(s);
        want_sub_.erase(s);     // 还没发出去就撤了：不必发那一对
    }
    cache_.erase(up);
}

void BookTickerStream::send_subs(Conn* c, const std::vector<std::string>& streams, bool sub) {
    if (streams.empty()) return;
    // 分块发送。单条消息塞满 200 条流时整条被拒就全军覆没，分块至少只损失一块；
    // 块数很少（200/100=2），不会触到每秒 10 条的入站限制
    for (size_t i = 0; i < streams.size(); i += kChunk) {
        const size_t end = std::min(i + kChunk, streams.size());
        std::ostringstream params;
        for (size_t j = i; j < end; ++j) {
            if (j > i) params << ',';
            params << '"' << streams[j] << '"';
        }
        const std::string msg =
            std::string("{\"method\":\"") + (sub ? "SUBSCRIBE" : "UNSUBSCRIBE")
            + "\",\"params\":[" + params.str()
            + "],\"id\":" + std::to_string(req_id_++) + "}";
        c->ws->send(msg);
    }
}

// ── 消息解析 ──────────────────────────────────────────────────────────────────
void BookTickerStream::on_message(const std::string& json, Conn* c) {
    if (json.find("\"stream\"") == std::string::npos) {
        // 不是数据包。{"result":null,"id":N} 是正常的订阅确认，安静丢掉；
        // 其余（{"code":2,"msg":"Invalid request..."} 之类）必须让人看见——
        // 这里此前一律静默返回，某条流没订上时界面只是空白，无从查起
        if (json.find("\"result\":null") == std::string::npos) {
            say(std::string("行情WS") + (c ? "#" + std::to_string(c->id) : "")
                + " 服务端消息: " + json.substr(0, 300));
        }
        // ⚠ 控制消息【不算】数据活性。订阅被拒时服务端照样回消息，
        //   若把它计入 last_msg_ms，看门狗会认为"有来往所以连接是好的"，
        //   于是一条订阅全被拒的连接可以永远存活下去
        //
        // 但要【计数】：它是"币安在不在应答"的唯一证据。订阅确认本身不打日志
        //   （正常情况每次重连都刷一条纯噪音），所以没有这个计数的话，
        //   "收到了确认但没行情"和"一个字节都没回来"在日志上完全一样
        if (c) c->ctrl_msgs.fetch_add(1);
        return;
    }
    if (json.size() < 20) return;

    if (c) {
        // 本次连接的首包提示。有了它，"已连接"之后到底有没有数据就一目了然：
        //   有"已连接"没"首包"  → 订阅没生效
        //   连"已连接"都没有     → 压根没连上
        //   两条都有             → 数据在流，问题在别处
        // 每次连接都提示一次（不是进程级一次），因为重连是最容易丢订阅的时刻
        if (!c->data_seen.exchange(true)) {
            say("行情WS#" + std::to_string(c->id) + " 收到首个数据包，订阅生效");
            // 真正的"自愈成功"是数据回来了，不是 TCP 连上了。升级计数在这里清，
            // 不在 Open 分支清——理由见 wire() 里那段注释
            c->kicks_since_ok.store(0);
        }
        c->last_msg_ms.store(now_ms());
    }

    // parser 复用。它内部持有解析用的大块缓冲，每条消息构造一个就是每条消息
    // 一次大分配。开了 aggTrade 之后这条路径每秒上千包，必须先干净。
    // thread_local 而不是成员：on_message 平时只在各自的 WS 线程跑（多连接就是
    // 多个线程），成员 parser 不是线程安全的；thread_local 天然一线程一份
    static thread_local simdjson::dom::parser parser;
    static thread_local std::string pad;
    pad.assign(json);
    pad.append(simdjson::SIMDJSON_PADDING, '\0');

    simdjson::dom::element doc;
    // realloc_if_needed=false：padding 已经由上面的 append 保证，
    // 让 simdjson 省掉它内部那次拷贝
    if (parser.parse(pad.data(), json.size(), false).get(doc) != simdjson::SUCCESS) return;

    // combined stream 格式: {"stream":"btcusdt@markPrice@1s","data":{...}}
    simdjson::dom::element data;
    if (doc["data"].get(data) != simdjson::SUCCESS) return;

    std::string_view ev;
    if (data["e"].get(ev) != simdjson::SUCCESS) return;

    std::string_view sym_sv;
    if (data["s"].get(sym_sv) != simdjson::SUCCESS) return;
    // ⚠ 必须在这里就拷成 std::string：sym_sv 指向 pad，而 pad 是 thread_local，
    //   下一条消息进来就会被覆盖
    std::string symbol(sym_sv);

    if (ev == "markPriceUpdate") {
        // {"e":"markPriceUpdate","s":"BTCUSDT","p":"<标记价>","i":"<指数价>",...}
        std::string_view p_sv;
        if (data["p"].get(p_sv) != simdjson::SUCCESS) return;
        const double mp = safe_stod(p_sv);
        if (mp <= 0) return;
        const int64_t t = now_ms();
        std::lock_guard<std::mutex> lk(mtx_);
        // 各条流写同一个缓存条目，各自【只更新自己那几个字段】，
        // 不构造全新 Tick 覆盖——否则一条流每来一包就把另外几条的数据抹成 0
        auto& e = cache_[symbol];
        e.symbol     = symbol;
        e.mark_price = mp;
        e.mark_ms    = t;
        e.ws_mark_ms = t;   // 只有流来的包会动这个，REST 写回不动（见头文件）

    } else if (ev == "24hrTicker") {
        // {"e":"24hrTicker","s":"BTCUSDT","P":"<24h涨幅%>","c":"<最新成交价>",...}
        // 只取 P。这条流的 c（最新成交价）刻意不用——要成交价就订 @aggTrade，
        // 那才是每笔推送；@ticker 是每秒一次的汇总，拿它当成交价是慢一拍的
        std::string_view P_sv;
        if (data["P"].get(P_sv) != simdjson::SUCCESS) return;
        // 涨幅可以是负数也可以恰好是 0，不能像价格那样用 ">0" 判合法。
        // safe_stod 失败返回 0，而真实的 0 涨幅同样是 0——两者无法区分，
        // 所以直接检查原始字符串
        if (P_sv.empty()) return;
        const double pct = safe_stod(P_sv);
        const int64_t t  = now_ms();
        std::lock_guard<std::mutex> lk(mtx_);
        // 只碰自己的两个字段：不能让每秒一次的涨幅去刷新标记价的收包时间，
        // 否则 markPrice 断流时陈旧保护会被这条流一直"续命"
        auto& e = cache_[symbol];
        e.symbol  = symbol;
        e.chg_24h = pct;
        e.chg_ms  = t;

    } else if (ev == "aggTrade") {
        // {"e":"aggTrade","s":"BTCUSDT","a":<聚合成交ID>,"p":"<价>","q":"<量>",...}
        int64_t aid = 0;
        if (data["a"].get(aid) != simdjson::SUCCESS) return;
        std::string_view p_sv, q_sv;
        if (data["p"].get(p_sv) != simdjson::SUCCESS) return;
        if (data["q"].get(q_sv) != simdjson::SUCCESS) return;
        const double px = safe_stod(p_sv);
        if (px <= 0) return;
        const double qty = safe_stod(q_sv);
        const int64_t t  = now_ms();

        std::lock_guard<std::mutex> lk(mtx_);
        auto& e = cache_[symbol];
        // 聚合成交ID 是【逐一递增】的，所以这里能做两件别处做不到的事：
        //   ① aid <= 已有值 ⇒ 乱序或重放的旧包。直接丢弃，绝不能让它把
        //      更新的价格盖回去——那是"价格突然跳回去"这类幽灵bug的来源
        //   ② aid > 已有值+1 ⇒ 中间的消息【丢了】。这是唯一能证明
        //      "WebSocket 悄悄漏了包"的证据，别的地方查不出来
        if (e.agg_id != 0) {
            if (aid <= e.agg_id) return;
            if (aid > e.agg_id + 1) { ++e.agg_gaps; ++gaps_; }
        }
        e.symbol   = symbol;
        e.last_px  = px;
        e.last_qty = qty;
        e.last_ms  = t;
        e.agg_id   = aid;

    } else if (ev == "bookTicker") {
        // {"e":"bookTicker","u":<updateId>,"s":"BTCUSDT",
        //  "b":"<买一价>","B":"<买一量>","a":"<卖一价>","A":"<卖一量>"}
        // ⚠ 这里的 "a" 是【卖一价】（字符串），而 aggTrade 的 "a" 是成交ID（数字）。
        //   同名不同义，靠 e 区分，写错了会静默拿到垃圾
        int64_t uid = 0;
        if (data["u"].get(uid) != simdjson::SUCCESS) return;
        std::string_view b_sv, B_sv, a_sv, A_sv;
        if (data["b"].get(b_sv) != simdjson::SUCCESS) return;
        if (data["a"].get(a_sv) != simdjson::SUCCESS) return;
        const double bid = safe_stod(b_sv);
        const double ask = safe_stod(a_sv);
        if (bid <= 0 || ask <= 0) return;
        // 买一 >= 卖一 = 交叉盘口。bookTicker 是一个【一致的快照】，
        // 交叉就说明这包是坏的，收下它会让滑点估算得出负数
        if (bid >= ask) return;
        const double bq = (data["B"].get(B_sv) == simdjson::SUCCESS) ? safe_stod(B_sv) : 0.0;
        const double aq = (data["A"].get(A_sv) == simdjson::SUCCESS) ? safe_stod(A_sv) : 0.0;
        const int64_t t = now_ms();

        std::lock_guard<std::mutex> lk(mtx_);
        auto& e = cache_[symbol];
        // updateId 单调递增但【不是逐一递增】（一次跳多少不定），所以只能检出
        // 倒退，检不出丢包——和 agg_id 的语义不同，不要把两者的计数混着读
        if (e.book_id != 0 && uid <= e.book_id) {
            if (uid < e.book_id) ++e.book_back;
            return;
        }
        e.symbol  = symbol;
        e.bid     = bid;
        e.ask     = ask;
        e.bid_qty = bq;
        e.ask_qty = aq;
        e.book_ms = t;
        e.book_id = uid;
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
    // ⚠ 不碰 ws_mark_ms：这是 REST 兜底成功的证据，不是 WS 活着的证据
}

bool BookTickerStream::change_24h(const std::string& symbol, double& out_pct) const {
    auto t = get(symbol);
    // chg_ms==0 表示这条流一次都没到过。涨幅本身可以合法地为 0 或负数，
    // 所以不能用数值判有无，只能看有没有收过包
    if (t.chg_ms == 0) return false;
    if (now_ms() - t.chg_ms > kChgStaleMs) return false;
    out_pct = t.chg_24h;
    return true;
}

bool BookTickerStream::last_trade(const std::string& symbol, int64_t max_age_ms,
                                  double& out_px) const {
    auto t = get(symbol);
    if (t.last_px <= 0 || t.last_ms == 0) return false;
    // max_age_ms < 0 = 不限年龄（"有多旧都给我"，界面显示这类用途）。
    // 阈值由调用方给的理由见头文件：冷门币几分钟不成交是正常的，
    // 而短线拿 5 秒前的成交价去追涨是错的，两者差三个数量级
    if (max_age_ms >= 0 && now_ms() - t.last_ms > max_age_ms) return false;
    out_px = t.last_px;
    return true;
}

bool BookTickerStream::best_bid_ask(const std::string& symbol, int64_t max_age_ms,
                                    double& out_bid, double& out_ask) const {
    auto t = get(symbol);
    if (t.bid <= 0 || t.ask <= 0 || t.book_ms == 0) return false;
    if (max_age_ms >= 0 && now_ms() - t.book_ms > max_age_ms) return false;
    out_bid = t.bid;
    out_ask = t.ask;
    return true;
}

// ── 健康快照 ──────────────────────────────────────────────────────────────────
BookTickerStream::Health BookTickerStream::health() const {
    Health h;
    h.watchdog_kicks = wd_kicks_.load();
    const int64_t now = now_ms();

    std::lock_guard<std::mutex> lk(mtx_);
    h.conns   = conns_.size();
    h.streams = streams_.size();
    h.symbols = subs_.size();
    h.gaps    = gaps_;

    int64_t newest_msg = 0, oldest_up = 0;
    for (const auto& c : conns_) {
        h.connects += c->connects.load();
        if (c->streams.size() > kStreamCap) h.over_cap = true;
        if (c->connected.load()) {
            ++h.conns_up;
            const int64_t cs = c->conn_since_ms.load();
            if (cs > 0 && (oldest_up == 0 || cs < oldest_up)) oldest_up = cs;
        }
        const int64_t lm = c->last_msg_ms.load();
        if (lm > newest_msg) newest_msg = lm;
    }
    h.connected  = h.conns_up > 0;
    h.silence_ms = (newest_msg > 0) ? now - newest_msg : -1;
    h.uptime_ms  = (oldest_up  > 0) ? now - oldest_up  : -1;

    for (const auto& s : subs_) {
        auto it = cache_.find(s);
        if (it == cache_.end()) { ++h.never; ++h.chg_never; continue; }
        const Tick& t = it->second;
        // 看 ws_mark_ms 而不是 mark_ms：后者会被 REST 兜底刷新，
        // 那样 WS 死了 Health 还是全绿，看门狗和这个快照就同时白做了
        if (t.ws_mark_ms == 0)                        ++h.never;
        else if (now - t.ws_mark_ms > kStaleMs)       ++h.stale;
        else                                          ++h.fresh;
        // 涨幅流单独数。它和标记价同在一条连接上，但币安可以只丢掉其中一条
        // 订阅——那时 markPrice 全绿，而高位拦截的唯一数据来源已经悄悄失效
        if (t.chg_ms == 0)                            ++h.chg_never;
        else if (now - t.chg_ms > kChgStaleMs)        ++h.chg_stale;
    }
    return h;
}

std::vector<std::string> BookTickerStream::stale_symbols() const {
    std::vector<std::string> out;
    const int64_t now = now_ms();
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& s : subs_) {
        auto it = cache_.find(s);
        if (it == cache_.end() || it->second.ws_mark_ms == 0) continue;  // 从未收到，另算
        if (now - it->second.ws_mark_ms > kStaleMs) out.push_back(s);
    }
    return out;   // subs_ 是 std::set，天然有序
}

std::vector<size_t> BookTickerStream::conn_loads_for_test() const {
    std::vector<size_t> v;
    std::lock_guard<std::mutex> lk(mtx_);
    v.reserve(conns_.size());
    for (const auto& c : conns_) v.push_back(c->streams.size());
    return v;
}

} // namespace ccbot
