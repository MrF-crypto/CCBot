#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>

namespace ix { class WebSocket; }

namespace ccbot {

// Binance USDT-M Futures 行情流。
//
// ── 四条流，四种用途 ──────────────────────────────────────────────────────────
// 默认只订阅前两条（Core），与 v4.7.1 完全一致；后两条是给短线策略准备的容量，
// 默认关闭，理由见 Feeds。
//
//   <sym>@markPrice@1s  标记价  引擎决策、强平距离、界面显示。严格每秒一包
//   <sym>@ticker        涨幅    24 小时滚动涨幅，高位拦截用。每秒一包
//   <sym>@aggTrade      成交价  每笔成交。短线信号该看它，不是标记价
//   <sym>@bookTicker    买卖一  下单前估滑点用。盘口一变就推
//
// 三个"价"是三个【口径】，不是三个精度不同的同一个数：
//   标记价 = 带指数成分与资金费基差，币安用它算强平与未实现盈亏。
//            "离强平还有多远"只有它能回答，抗单交易所插针
//   成交价 = 市场上真实换手的价格。短线的信号与盈亏兑现看的是这个
//   买卖一 = 你实际能成交的价格。滑点只能靠它算
// 混用会出事：拿标记价当成交价，回测赚的钱实盘会被点差吃掉；
// 拿成交价算强平距离，插针时会误判。
//
// v4.0.11 曾删掉 bookTicker，那次的教训不是"这条流没用"，而是
// 【用途没定义清楚】：当时它在生产代码里零引用，只剩给"延迟"列算包龄，
// 而那个延迟测的是引擎根本不看的流——markPrice 停了、bookTicker 还在的时候，
// 延迟列显示绿色，引擎实际已经在走 REST 兜底。所以这次每条流都先写清
// 角色、读者、陈旧规则，再接线。
//
// ── 自愈与可观测 ──────────────────────────────────────────────────────────────
// 此前这个类只做到了"不出错"，没做到"会恢复"。WebSocket 半开时（TCP 还在、
// Open 状态还在、数据早就不来了）三件事同时发生：
//   ① mark_price() 的陈旧保护返回 0 —— 安全，引擎不会用冻结价
//   ② 调用方转 REST 兜底拿到价格 —— 可用，策略继续跑
//   ③ 于是 headless 的 stall 计数被清零 —— 【不告警】
// 三件事叠起来的结果是【永久降级且完全不可见】：延迟从毫秒掉到秒级、
// REST 权重被持续消耗、24h 涨幅没有任何兜底直接失效，而所有监控都显示健康，
// 并且没有任何东西会把这条死连接掀掉重连。
// 现在有：静默看门狗（主动重连）、Health 快照（把链路真实状态摊开）、
// 出站控制消息合并限流、序号断层检测（丢包的唯一证据）。
class BookTickerStream {
public:
    // 要订阅哪些流。
    //
    // 后两条默认【关闭】，这是有意的：开了就是每品种 4 条流，而币安单连接
    // 上限 200 条，能装的品种数直接砍半；目前它们还没有消费者（趋势SAR 用的是
    // 标记价）。等短线策略真的要用时再开，届时分片会自动多开连接顶上。
    struct Feeds {
        bool agg_trade   = false;   // 每笔成交：决策价 + 成交量 + 断层检测
        bool book_ticker = false;   // 买一/卖一：执行价与滑点估计
        size_t per_symbol() const {
            return 2 + (agg_trade ? 1u : 0u) + (book_ticker ? 1u : 0u);
        }
    };

    struct Tick {
        std::string symbol;

        // ── 标记价（@markPrice@1s，始终订阅）────────────────────────────────
        // 币安用它算强平价、未实现盈亏、强平触发——带指数成分与资金费基差，
        // 抗单交易所插针。"离强平还有多远"只有它能回答
        double  mark_price = 0;
        int64_t mark_ms    = 0;   // 收包时间(ms)。陈旧判定用它，REST 兜底也会刷新它
        // 最后一次【来自 WS 流】的标记价时间。与 mark_ms 的区别只在 REST 兜底：
        // set_mark_price 会刷新 mark_ms（引擎的陈旧保护必须对 REST 来的价也生效），
        // 但不会碰这个。Health 统计新鲜度只看它——否则 WS 死了、REST 兜底成功时
        // Health 依旧全绿，看门狗和健康快照就同时白做了
        int64_t ws_mark_ms = 0;

        // ── 24 小时滚动涨幅%（@ticker 的 P 字段，始终订阅）──────────────────
        // 高位拦截用它而不是"今日涨幅"——后者每天 UTC 0 点归零，
        // 而 UTC 0 点是北京时间早 8 点，是真实交易时段，闸门会在那里瞎掉
        double  chg_24h    = 0;
        int64_t chg_ms     = 0;

        // ── 最新成交价（@aggTrade，需开启 Feeds::agg_trade）─────────────────
        // ⚠ 没有固定陈旧阈值可用：冷门币几分钟不成交是完全正常的，不是断流。
        //   "这条流断了吗"只能靠 markPrice@1s 回答（它保证每秒一包）。
        //   所以 last_trade() 要求调用方自己给容忍度
        double  last_px    = 0;
        double  last_qty   = 0;
        int64_t last_ms    = 0;
        // 聚合成交ID。币安保证它【逐一递增】，所以断层就是丢包的铁证——
        // 这是唯一能证明"WebSocket 悄悄漏了消息"的东西，别的地方查不出来
        int64_t agg_id     = 0;
        int64_t agg_gaps   = 0;   // 累计断层次数

        // ── 买一/卖一（@bookTicker，需开启 Feeds::book_ticker）──────────────
        double  bid = 0, ask = 0;
        double  bid_qty = 0, ask_qty = 0;
        int64_t book_ms = 0;
        // 盘口 updateId。⚠ 它单调递增但【不是逐一递增】（一次跳多少不定），
        //   所以只能检出"倒退"（乱序/旧包），检不出丢包。
        //   和 agg_id 的语义不同，别把两者的断层数混在一起解读
        int64_t book_id   = 0;
        int64_t book_back = 0;   // 累计倒退次数
    };

    // 行情链路的健康快照。
    //
    // 一次取全而不是拆成一堆 getter：调用方要的是【一致】的图像。分多次读会拼出
    // 自相矛盾的状态——比如读到 connected=true，再读 silence_ms 时已经断了，
    // 于是日志里出现"连接正常，但 300 秒没收到包"这种无法解释的组合。
    struct Health {
        bool    connected      = false;  // 至少有一条连接是通的
        size_t  conns          = 0;      // 连接数
        size_t  conns_up       = 0;      // 其中已连通的
        int     connects       = 0;      // 各连接累计连接成功次数之和
        int     watchdog_kicks = 0;      // 静默看门狗强制重连的次数
        int64_t silence_ms     = -1;     // 距最后一个数据包多少毫秒；从未收到 = -1
        int64_t uptime_ms      = -1;     // 最早那条连接已持续多久；全断 = -1
        size_t  streams        = 0;
        size_t  symbols        = 0;
        size_t  fresh          = 0;      // 标记价新鲜的品种数
        size_t  stale          = 0;      // 标记价收过包但已超过 kStaleMs
        size_t  never          = 0;      // 订阅了但一个标记价都没收到
        // 涨幅流单独统计。它和标记价在同一条连接上，但币安可以只丢掉其中一条
        // 订阅——那时 markPrice 全绿，而 24h 涨幅（高位拦截的唯一来源）已经
        // 悄悄失效。不单独数的话这种劣化完全看不见
        size_t  chg_stale      = 0;      // 收过包但超过 kChgStaleMs
        size_t  chg_never      = 0;
        int64_t gaps           = 0;      // aggTrade 序号断层累计（= 确证的丢包）
        bool    over_cap       = false;  // 仍有流装不下（分片也没能安置）

        // 链路是否健康。判定标准是【引擎还能不能拿到新鲜价格】，而不是
        // "连接对象还在不在"——后者在半开时一样返回 true，正是要防的假阳性。
        //
        // 这个判定是【瞬时】的，调用方必须自己去抖（连续 N 次不健康才告警）：
        // 单个品种偶发丢一两包很正常，为此叫人是在训练人忽略告警。
        //
        // 断层数不参与判定：丢一两条消息是可恢复的，不该等同于链路不可用。
        // 它在 summary() 里报出来，由人去看趋势
        bool healthy() const {
            if (!connected || over_cap) return false;
            if (symbols == 0) return true;   // 没订阅任何品种，无从判断，不算故障
            return fresh == symbols && chg_stale == 0 && chg_never == 0;
        }
        // 一行结论，日志与 webhook 正文直接用
        std::string summary() const;
    };

    // ⚠ 刻意【不】写成 `BookTickerStream(bool, Feeds feeds = {})` 这一个带默认实参的
    //   构造函数。Feeds 是嵌套类，而它的默认成员初始化器在外层类的类体还没结束时
    //   不可用——默认实参正好处在那个位置。MSVC 放过了，GCC 与 Clang 都拒绝：
    //     GCC:   could not convert '<brace-enclosed initializer list>()' to 'Feeds'
    //     Clang: default member initializer for 'agg_trade' needed within definition
    //            of enclosing class ... outside of member functions
    //   拆成两个构造函数之后，Feeds{} 落在 .cpp 的函数体里，这条规则就不适用了
    explicit BookTickerStream(bool testnet = false);
    BookTickerStream(bool testnet, Feeds feeds);
    ~BookTickerStream();

    BookTickerStream(const BookTickerStream&)            = delete;
    BookTickerStream& operator=(const BookTickerStream&) = delete;

    void start();
    void stop();
    // 至少有一条连接通着。⚠ 它在半开时照样返回 true，判断链路可用性请用
    // health().healthy()
    bool is_connected() const { return connected_.load(); }

    // 订阅/取消（大写 symbol，如 "BTCUSDT"）。
    // ⚠ 删除 bot 时必须调用 unsubscribe：streams_ 不会自己收缩，重连时全量重订，
    //   反复增删品种会一路累积到上限
    void subscribe  (const std::string& symbol);
    void unsubscribe(const std::string& symbol);

    // 线程安全读最新快照；未收到任何数据时各字段为 0
    Tick   get(const std::string& symbol) const;

    // 标记价，带陈旧保护（超过 kStaleMs 没有新包返回 0）。
    // 引擎决策用这个：WebSocket 半开时连接还在、数据早就不来了，缓存里躺着一个
    // 冻结的价格看起来完全正常，引擎会拿着僵尸价继续补仓/止盈/止损
    double mark_price(const std::string& symbol) const;

    // 24 小时滚动涨幅%；从未收到或过期返回 false。
    // 涨幅可以合法地为 0 或负数，所以不能像价格那样用返回值 0 表示"无数据"
    bool   change_24h(const std::string& symbol, double& out_pct) const;

    // 最新成交价（需开启 Feeds::agg_trade）。
    //
    // ⚠ max_age_ms 必须由调用方给，这里【没有】一个放之四海的阈值：
    //   冷门币几分钟没有成交是正常的，不是断流；而短线策略拿一笔 5 秒前的
    //   成交价去追涨是错的。两种需求差三个数量级，类库替谁都不对。
    //   判断"这条流断了吗"请用 health()，别用成交包龄——那是两回事
    bool   last_trade(const std::string& symbol, int64_t max_age_ms,
                      double& out_px) const;

    // 买一/卖一（需开启 Feeds::book_ticker）。同样要调用方给容忍度：
    // 盘口一变就推，活跃品种毫秒级、冷门品种可以几秒不动
    bool   best_bid_ask(const std::string& symbol, int64_t max_age_ms,
                        double& out_bid, double& out_ask) const;

    // REST 取到的标记价写回缓存。
    // 存在的理由：markPrice 流不可用时（未订阅成功/刚启动/断流），调用方会转 REST
    // 兜底把价格喂给引擎——但界面读的是这个缓存，不写回的话引擎有价、界面空着。
    // v4.0.9 就是这么漏的：策略照常跑，"标记价"那一列却一直显示 "--"
    //
    // ⚠ 写回的价格【不计入 Health】：它是兜底成功的证据，不是 WS 活着的证据。
    //   把它算进 fresh 的话，链路死了 Health 依旧全绿——那就把看门狗和健康快照
    //   同时废掉了，回到"永久降级且不可见"
    void   set_mark_price(const std::string& symbol, double price);

    // 链路健康快照。调用方（界面状态栏 / headless 周期日志与告警）据此判断
    // "行情链路本身"是否可用，而不是逐个品种猜
    Health health() const;
    // 标记价收过包但已陈旧的品种，按字母序。给告警正文用——"哪几个品种断了"
    // 比"3 个品种断了"可查得多
    std::vector<std::string> stale_symbols() const;

    // 服务端返回的【非数据消息】回调（运行在 WS 线程或 pump 线程）。
    // 这类消息没有 "stream" 字段，v4.0.9 之前被消息入口第一行直接丢弃——包括
    // {"code":2,"msg":"Invalid request..."} 这种订阅失败。后果是某条流没订上时
    // 界面只是一片空白，没有任何线索指向"订阅被拒"
    using LogCb = std::function<void(const std::string&)>;
    void on_server_msg(LogCb cb);

    static int64_t now_ms();

    // 标记价陈旧阈值：markPrice@1s 每秒一包，10 秒没来就是这条流断了
    static constexpr int64_t kStaleMs = 10000;

    // 涨幅陈旧阈值。给到 60 秒是因为 24h 涨幅本身是慢变量，几十秒的陈旧不影响
    // "最近涨得多急"这个判断，没必要跟标记价用同一把尺子
    static constexpr int64_t kChgStaleMs = 60000;

    // 静默阈值：整条连接一个数据包都没有多久，就认定它已经死了。
    // 取 20 秒而不是和 kStaleMs 一样的 10 秒——两者判的不是一回事：
    //   kStaleMs   = 某【一个品种】的价格陈旧了，可能只是那条流抖了一下
    //   kSilenceMs = 这条连接上【所有】品种一个包都没有，只要它带着一个品种
    //                就每秒该有一包，这只可能是连接本身坏了，比前者严重得多
    // 但也要给重连握手与首次订阅生效留出余量，所以不取更短
    static constexpr int64_t kSilenceMs = 20000;

    // 连接建立后多久还没有任何数据包，就提示"订阅可能没生效"。
    // 单独一条阈值是因为这两种故障的【修法完全不同】：
    // 没连上要查网络，连上了没数据要查订阅（流名拼错、超上限、被拒）
    static constexpr int64_t kSubGraceMs = 10000;

    // 币安 USDT-M 合约单连接流上限（1024 是【现货】的数字，别搞混）
    static constexpr size_t  kStreamCap = 200;

    // ── 纯函数判定（可穷举测试）────────────────────────────────────────────────
    // 抽出来而不是埋在线程循环/订阅流程里，是因为这几个判断都会【静默出错】，
    // 而且两个方向的错都不报错。线程循环本身没法在单测里可靠驱动。
    //
    // 该不该掀掉这条连接重连：
    //   判太松 → 半开连接永远不恢复，回到本轮要修的那个问题
    //   判太紧 → 正常连接被反复打断，每次重连都要全量重订，反而更不稳
    //
    // kicks_since_ok = 连续掀了多少次都没换来一个数据包。据它退避：连续掀不好
    // 说明这不是"半开连接"那种能靠重连自愈的故障，而是外部原因（出口 IP 被
    // 限制、代理没把域名送到币安）。此时每 20 秒掀一次只有三个后果——日志被
    // 刷满、白耗币安的「300 连接 / 5 分钟 / IP」配额、把真正有用的告警冲散。
    // 收到数据包时 kicks_since_ok 归零（见 on_message），立刻回到 20 秒的灵敏度：
    // 退避只惩罚"一直好不了"，不惩罚偶发抖动
    static bool should_kick(int64_t now, int64_t last_msg_ms, int64_t conn_since_ms,
                            bool connected, size_t stream_n, int kicks_since_ok = 0);
    // 当前该容忍多久的静默（毫秒）。抽成纯函数是为了能穷举各档
    static int64_t silence_budget_ms(int kicks_since_ok);
    // 是否该提示"连上了但订阅没生效"
    static bool should_warn_no_data(int64_t now, int64_t last_msg_ms, int64_t conn_since_ms,
                                    bool connected, size_t stream_n);
    // 给一个新品种挑连接。counts = 各连接当前流数，need = 这个品种要占几条流，
    // cap = 单连接上限。返回装得下它的连接下标；都装不下就返回 counts.size()，
    // 表示要新建一条连接。
    // 此前没有这一步：超过 200 条流的部分【静默】收不到行情，而没有任何地方报错
    static size_t pick_conn(const std::vector<size_t>& counts, size_t need, size_t cap);

    // ── 测试注入点 ────────────────────────────────────────────────────────────
    // 报文解析与陈旧判定是这个类里会【静默出错】的部分：解析错了价格是垃圾，
    // 陈旧判定错了引擎会拿着冻结价继续决策——两者都不会报错。建连接/重连本身
    // 是显性故障（完全没有价格），不需要单测；但上面那三个【判断】要测。
    // 注入的报文会【归属到第 0 条连接】（没有就先造一条，不建 socket 不启动）。
    // 必须归属：首包提示、连接活性、看门狗时间线全挂在连接上，用 nullptr 注入
    // 等于测了一条真实运行中不存在的路径
    void on_message_for_test(const std::string& json) {
        Conn* c = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (conns_.empty()) {
                auto nc = std::make_unique<Conn>();
                nc->id = 0;
                conns_.push_back(std::move(nc));
            }
            c = conns_[0].get();
        }
        on_message(json, c);
    }
    // 把标记价的收包时间往前推，用来构造"WebSocket 半开、缓存里是冻结价"
    // 这个最凶险的场景——真等 10 秒会让测试慢得没人愿意跑。
    // 两个时间戳一起推：它们记的是【同一个事件】（收到一包流数据），
    // 只推一个会造出真实运行中不可能出现的状态，那样测出来的结论不算数
    void age_mark_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it == cache_.end()) return;
        const int64_t t = now_ms() - age_ms;
        it->second.mark_ms = t;
        if (it->second.ws_mark_ms != 0) it->second.ws_mark_ms = t;
    }
    void age_chg_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it != cache_.end() && it->second.chg_ms != 0)
            it->second.chg_ms = now_ms() - age_ms;
    }
    void age_trade_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it != cache_.end() && it->second.last_ms != 0)
            it->second.last_ms = now_ms() - age_ms;
    }
    void age_book_for_test(const std::string& symbol, int64_t age_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = cache_.find(symbol);
        if (it != cache_.end() && it->second.book_ms != 0)
            it->second.book_ms = now_ms() - age_ms;
    }
    size_t stream_count_for_test() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return streams_.size();
    }
    // 把各连接标成已连通。没有它的话单测里 conns_up 恒为 0、Health::healthy()
    // 恒为 false，那条判定除了"未连接"以外的分支一条都测不到
    void mark_connected_for_test(bool up) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (conns_.empty()) {
            auto nc = std::make_unique<Conn>();
            nc->id = 0;
            conns_.push_back(std::move(nc));
        }
        for (auto& c : conns_) {
            c->connected.store(up);
            if (up && c->conn_since_ms.load() == 0) c->conn_since_ms.store(now_ms());
        }
    }
    // 待发的增量订阅/退订条数。订阅不再即时发送而是合并后由 pump 线程发出
    // （见 subscribe 处的说明），这个钩子用来断言"确实进了队列"
    size_t pending_ctl_for_test() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return want_sub_.size() + want_unsub_.size();
    }
    // 分片结果：连接数，以及每条连接分到的流数
    size_t conn_count_for_test() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return conns_.size();
    }
    std::vector<size_t> conn_loads_for_test() const;

private:
    // 一条 WebSocket 连接及其独立的时间线。
    // 每条连接必须有【自己的】看门狗状态：一条断了另一条好着是常态，
    // 用全局时间线的话好连接会被坏连接的静默拖着一起重连
    struct Conn {
        std::unique_ptr<ix::WebSocket> ws;
        std::atomic<bool>    connected     {false};
        std::atomic<int>     connects      {0};
        std::atomic<int64_t> last_msg_ms   {0};   // 最后一个【数据包】
        std::atomic<int64_t> conn_since_ms {0};
        std::atomic<int64_t> last_kick_ms  {0};
        std::atomic<int>     kicks_since_ok{0};
        // ⚠ 每次连接都要重置这两个 latch。此前"首包提示"是进程级一次性的，
        //   于是重连后订阅被拒与订阅成功在日志里长得一模一样（都没有首包那条），
        //   而重连恰恰是最容易丢订阅的时刻
        std::atomic<bool>    data_seen     {false};
        std::atomic<bool>    nodata_warned {false};
        // 本次连接收到的【控制帧】数量（订阅确认 {"result":null,"id":N}、
        // 错误应答等非数据消息）。
        //
        // ⚠ 它是"没有行情"的两种成因之间唯一的判据，缺了它两者在日志上
        //   完全一样，而修法南辕北辙：
        //     ctrl_msgs > 0  → 币安在应答，是【订阅】的问题（流名错/被拒）
        //     ctrl_msgs == 0 → 连订阅确认都没回，根本没连到币安，是【网络】
        //                      的问题（代理/DNS 把域名劫持到了别处）
        //   实测日志里日志一口咬定"流名有误/被服务端拒绝"，而真因是后者
        std::atomic<int>     ctrl_msgs     {0};
        // 本次连接的【第一条】控制帧原文（截断）。只在零数据告警里打出来。
        //
        // ⚠ 这是"对面到底是不是币安"的唯一直接证据。币安对 SUBSCRIBE 回的是
        //   {"result":null,"id":N}，其中 id 必须与我们发出去的那个对上；
        //   而一个只会敷衍的中间件回不出正确的 id，或者格式会有细微差别。
        //   平时不打（每次重连刷一条纯噪音），只在"连上了却零数据"时才需要它
        std::string          first_ctrl;              // 由外层 mtx_ 保护
        // 本次连接最后一条 SUBSCRIBE 用掉的 id。和 first_ctrl 里的 id 比对，
        // 就能判断那条应答到底是不是在回我们这条消息
        std::atomic<int>     last_sub_id   {-1};
        std::set<std::string> streams;            // 由外层 mtx_ 保护
        int id = 0;

        Conn();
        ~Conn();                                  // 定义在 cpp：ix::WebSocket 只前置声明
        Conn(const Conn&)            = delete;
        Conn& operator=(const Conn&) = delete;
    };

    void on_open   (Conn* c);
    // c==nullptr 时只做解析（测试注入用），不更新任何连接的时间线
    void on_message(const std::string& json, Conn* c);
    // 批量订阅/退订。合并成一条消息而不是一条流一条消息：币安合约 WS 限制
    // 【每秒最多 10 条入站消息】，超了直接断连
    void send_subs (Conn* c, const std::vector<std::string>& streams, bool sub);
    void resubscribe_all(Conn* c);
    // 看门狗 + 出站控制消息合并发送，共用一个线程（两件事都是低频轮询，
    // 各开一个只是多一个要 join 的东西）
    void pump_loop();
    void force_reconnect(Conn* c, const std::string& why);
    void say(const std::string& m) const;
    void wire(Conn* c);                     // 装回调 + setUrl，start() 与新建连接共用

    // ── 自诊断：拿【原始单流端点】对照一次 ────────────────────────────────────
    // 币安有两套 WS 用法：
    //   /stream + SUBSCRIBE 消息   ← 本类平时用的，能动态加减品种
    //   /ws/<流名>                 ← 连上即推，不需要任何 SUBSCRIBE
    // "连上了、SUBSCRIBE 的 id 也对得上、却一个数据包都没有"这个状态，光看
    // 平时那条连接分不出是【我们的 SUBSCRIBE 用法不对】还是【这台机器拿不到
    // 币安的 WS 行情】——而两者的修法一个在代码里、一个在网络里。
    //
    // 所以零数据时自动拿 /ws/ 对照一次：
    //   它有数据 ⇒ 网络没问题，问题在我们这边
    //   它也没有 ⇒ 代码清白，去查出口 IP / 代理分流
    //
    // 整个进程只跑一次（probe_done_），不参与自愈、不影响任何行情路径，
    // 结论只写进日志。这是我反复在"你去试试"上打转之后才想明白该做的事：
    // 一个我自己没法从开发机验证的假设，就该让程序自己去验
    void start_probe(const std::string& stream_name);
    void reap_probe();                      // pump 每轮调用，到期就收掉
    static constexpr int64_t kProbeMs = 8000;   // 给它 8 秒，markPrice@1s 够推 8 条
    Conn* conn_for(const std::string& stream_name);   // 调用方须持 mtx_
    // 一个品种对应的全部流名（小写）。随 Feeds 变化，所以不是静态的
    std::vector<std::string> streams_of(const std::string& symbol) const;

    static std::string to_lower(std::string s);
    static std::string to_upper(std::string s);

    // 出站控制消息的合并周期。币安合约 WS 限制每秒最多 10 条入站消息，超了直接
    // 断连。每 250ms 每条连接最多发 2 条（SUBSCRIBE + UNSUBSCRIBE）= 8 条/秒
    static constexpr int64_t kFlushMs = 250;
    // 单条 SUBSCRIBE 里最多放多少条流。分两条发比一条塞满更稳——
    // 整条被拒时损失一半而不是全部
    static constexpr size_t  kChunk   = 100;
    // 连续 kKickEscalate 次掀连接都没换来一次成功连接，就改用更重的 stop/start。
    // 存在的理由：close() 之后是否自动重拨取决于库的内部状态，不该把自愈
    // 全押在一个我们无法在单测里验证的库行为上
    static constexpr int     kKickEscalate = 3;

    bool  testnet_;
    Feeds feeds_;
    std::atomic<bool> running_  {false};
    std::atomic<bool> connected_{false};   // 至少一条连接通着
    std::atomic<int>  req_id_   {1};
    std::atomic<int>  wd_kicks_ {0};
    std::atomic<bool> cap_warned_{false};

    mutable std::mutex                    mtx_;
    std::vector<std::unique_ptr<Conn>>    conns_;
    // stream 名（小写） → 所属连接。分片必须【按品种整体】落在同一条连接上，
    // 否则一个品种的标记价在死连接上、成交价在活连接上，任何对齐都失去意义
    std::unordered_map<std::string, int>  stream_conn_;
    std::set<std::string>                 streams_;   // 全量，lowercase
    std::set<std::string>                 subs_;      // UPPER 已订阅品种，Health 据此统计
    std::set<std::string>                 want_sub_;  // 待发增量订阅
    std::set<std::string>                 want_unsub_;// 待发增量退订
    std::unordered_map<std::string, Tick> cache_;     // UPPER symbol → Tick
    int64_t                               gaps_ = 0;  // aggTrade 断层累计

    LogCb  srv_cb_;

    std::thread              pump_;
    mutable std::mutex       cv_mtx_;
    std::condition_variable  cv_;       // stop() 靠它立刻叫醒 pump，不用等一个周期

    // ── 自诊断探针（见 start_probe）────────────────────────────────────────────
    // 只由 pump 线程创建/销毁，回调只碰后面两个 atomic —— 所以不需要额外的锁
    std::unique_ptr<ix::WebSocket> probe_;
    std::atomic<bool>    probe_done_ {false};   // 进程级一次性
    std::atomic<bool>    probe_open_ {false};
    std::atomic<int>     probe_msgs_ {0};
    int64_t              probe_start_ms_ = 0;
    std::string          probe_stream_;

    // 那段长诊断（含订阅确认原文、排查顺序）只印一次。
    // ⚠ nodata_warned 是【按连接】的 latch，每次 Open 都重置 —— 而故障持续时
    //   每 20 秒就重连一次，于是那一整段每 20 秒重印一遍。实测日志里三分钟
    //   刷了九段，把自检结论本身都冲得看不见了。
    //   长文的价值在于被读一次；之后只需要"还在坏"这个事实
    std::atomic<bool>    nodata_essay_done_{false};
    int64_t              last_nodata_note_ms_ = 0;   // 短提示的节流（pump 线程独占）
    static constexpr int64_t kNodataNoteMs = 300000; // 5 分钟一条短提示
};

} // namespace ccbot
