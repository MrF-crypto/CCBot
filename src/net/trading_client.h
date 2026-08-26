#pragma once
#include "core/itrading_client.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>

namespace ccbot { class RateGate; }

namespace ccbot {

// Binance USDT-M Futures REST API 客户端（同步 CURL + mbedtls HMAC）
class TradingClient : public ITradingClient {
public:
    // 账户类型。两者是**不同的 API 域名和路径**，不是同一套接口的开关：
    //   Futures         —— 普通合约账户，fapi.binance.com，/fapi/*
    //   PortfolioMargin —— 统一账户，papi.binance.com，/papi/v1/um/*
    // 统一账户没有测试网（币安只在主网提供），选它时 testnet 会被强制关掉。
    enum class AccountMode { Futures = 0, PortfolioMargin = 1 };

    struct Config {
        std::string api_key;
        std::string api_secret;
        bool        testnet = true;
        AccountMode account_mode = AccountMode::Futures;
    };

    struct AccountInfo {
        bool   ok               = false;
        double total_equity     = 0;   // totalMarginBalance / accountEquity(统一账户)
        double available        = 0;   // availableBalance / totalAvailableBalance(统一账户)
        double unrealized_pnl   = 0;   // totalUnrealizedProfit
        // 统一账户维持保证金率（uniMMR）。统一账户是**全账户**统一算强平的，只看 UM
        // 子账户的数字会低估风险，所以这个值要单独盯：<1.05 币安开始强制减仓。
        // 0 = 普通合约账户，不适用
        double uni_mmr          = 0;
        std::string error;
    };

    struct Position {
        std::string symbol;
        int    direction      = 0;   // 1=多  -1=空
        double qty            = 0;   // 绝对值
        double entry_price    = 0;
        double mark_price     = 0;
        double unrealized_pnl = 0;
        double liq_price      = 0;
        double notional       = 0;   // qty * mark_price
        int    leverage       = 1;
    };

    struct OpenOrder {
        std::string order_id;
        std::string symbol;
        std::string side;        // BUY / SELL
        std::string type;        // LIMIT / MARKET / STOP_MARKET / ...
        double price      = 0;
        double orig_qty   = 0;
        double exec_qty   = 0;
        std::string status;
    };

    struct OrderResult {
        bool        ok = false;
        std::string order_id;
        std::string error;
        double      avg_price    = 0;   // 实际成交均价（MARKET 单，newOrderRespType=RESULT 才有）
        double      executed_qty = 0;   // 实际成交数量
        // 网络超时且事后查单也无法确认订单是否到达交易所（可能已成交也可能没有）。
        // 调用方对 uncertain 必须区别对待：开仓不能盲目重试（可能双倍仓位），
        // 平仓可以重试（reduceOnly 天然幂等）
        bool        uncertain    = false;
    };

    explicit TradingClient(const Config& cfg);
    ~TradingClient();

    AccountInfo            fetch_account();
    std::vector<Position>  fetch_positions();
    std::vector<OpenOrder> fetch_open_orders();

    OrderResult place_market(const std::string& symbol, const std::string& side,
                              double qty, bool reduce_only = false);
    // 按自定义 clientOrderId 查单（幂等性恢复：下单请求超时后确认它到底成交没有）
    OrderResult query_order(const std::string& symbol, const std::string& client_order_id);
    OrderResult place_limit (const std::string& symbol, const std::string& side,
                              double qty, double price, bool reduce_only = false);

    bool cancel_order(const std::string& symbol, const std::string& order_id);
    bool cancel_all_orders(const std::string& symbol);
    bool close_position(const std::string& symbol);
    bool close_all_positions();

    // set_leverage / round_qty 见下方 ITradingClient 实现区（override 声明）

    // TP/SL 条件市价平仓单（reduceOnly=true + 显式数量，兼容所有账户模式）
    // entry_side: "BUY"=做多仓位, "SELL"=做空仓位; qty=持仓数量
    OrderResult place_tp_market(const std::string& symbol, double stop_price,
                                 const std::string& entry_side, double qty);
    OrderResult place_sl_market(const std::string& symbol, double stop_price,
                                 const std::string& entry_side, double qty);

    // 拉取最近 K 线并计算 RSI（公开接口，无需签名）
    double fetch_rsi(const std::string& symbol,
                     const std::string& interval = "1h", int period = 14);

    // 布林带(BOLL) + RSI 快照——用于指标信号首单判定，公开接口不占用签名限流。
    // 拉的最后一根K线是币安还在滚动更新的"未收盘"K线，所以数值是实时估算值，
    // 不是等K线收盘才确定的值（收盘前可能还会变）
    struct IndicatorSnapshot {
        bool   ok      = false;
        double price   = 0;      // 最新价（未收盘K线的当前收盘值，近似最新成交价）
        double boll_ub = 0;
        double boll_mb = 0;
        double boll_lb = 0;
        double rsi     = 50.0;
    };
    IndicatorSnapshot fetch_indicators(const std::string& symbol, const std::string& interval,
                                        int boll_period, double boll_mult, int rsi_period);

    // 高周期趋势快照（趋势状态机用，公开接口不占签名限流）。
    // bearish = 价格在 EMA 之下 且 中轨（SMA20）在最近 slope_bars 根K线里下移超过 0.2%
    // ——两个条件都满足才判定为"空头态"，避免横盘时反复切换状态
    struct TrendSnapshot {
        bool   ok       = false;
        bool   bearish  = false;
        double price    = 0;
        double ema_val  = 0;
        double mb_slope_pct = 0;   // 中轨斜率（最近 slope_bars 根K线的位移%）
    };
    TrendSnapshot fetch_trend(const std::string& symbol,
                              const std::string& interval = "4h",
                              int ema_period = 200, int slope_bars = 3);

    // 完整 OHLCV K线（SR区域检测等需要高低点/成交量的场景用，公开接口）
    struct Bar {
        double open = 0, high = 0, low = 0, close = 0, volume = 0;
    };
    std::vector<Bar> fetch_bars(const std::string& symbol,
                                const std::string& interval, int limit);
    // 统一的 K 线拉取+解析（fetch_rsi/fetch_indicators/fetch_trend/fetch_bars 共用）
    std::vector<Bar> fetch_klines(const std::string& symbol,
                                  const std::string& interval, int limit);

    // 拉取标记价格（公开接口，CCG 价格轮询用）
    double fetch_mark_price(const std::string& symbol);

    // ── 资金费 ───────────────────────────────────────────────────────────────
    // premiumIndex 一次返回标记价 + 当前费率 + 下次结算时间，我们原先只取了标记价
    // 就把后两个丢了。对"套住就长期持有"的用法，费率就是持有成本的定价
    struct PremiumInfo {
        bool    ok           = false;
        double  mark_price   = 0;
        double  funding_rate = 0;   // 本期费率（正数=多头付给空头）
        int64_t next_ms      = 0;   // 下次结算时间
    };
    PremiumInfo fetch_premium(const std::string& symbol);

    // 历史资金费流水。income 带符号，负数=你付出去的。
    // 币安不带时间范围时只返回最近7天，所以补历史要按7天窗口分页（见 funding_ledger）
    struct FundingRecord {
        std::string symbol;
        double      income = 0;
        int64_t     time   = 0;
    };
    std::vector<FundingRecord> fetch_funding_income(int64_t start_ms, int64_t end_ms,
                                                    const std::string& symbol = "");

    // 持仓模式检测（连接时调用一次）
    bool fetch_position_mode();


    // ── LOT_SIZE / 价格精度 ─────────────────────────────────────────────────
    struct SymbolInfo {
        double step_size        = 0.001;  // LOT_SIZE.stepSize（限价单精度）
        double market_step_size = 0.0;   // MARKET_LOT_SIZE.stepSize（市价单精度，0=同step_size）
        double min_qty          = 0.001;
        double tick_size        = 0.01;
        bool   valid            = false;

        // 返回市价单实际使用的步长
        double effective_market_step() const {
            return (market_step_size > 0) ? market_step_size : step_size;
        }
    };
    // 按【值】返回，不是引用。返回引用的话指向的是 sym_cache_ 内部对象，而锁在
    // 函数返回时就放掉了：两个线程同时首次查询同一品种，一个通过引用在读、另一个
    // 在 `sym_cache_[sym] = info` 覆写同一个对象，是货真价实的数据竞争（UB）。
    // 结构体只有 4 个 double + 1 个 bool，拷贝的代价可以忽略
    SymbolInfo get_symbol_info(const std::string& symbol);
    // 只读缓存，不发网络请求；未命中返回 false —— 给 GUI 线程用，绝不能阻塞界面
    bool try_get_symbol_info(const std::string& symbol, SymbolInfo& out) const;
    double round_price(const std::string& symbol, double price);

    // ── UserData Stream ─────────────────────────────────────────────────────
    std::string create_listen_key();
    bool        keepalive_listen_key(const std::string& key);
    void        delete_listen_key(const std::string& key);

    // 拉取 Binance 服务器时间，计算本机与服务器的时钟偏移（一次即可）
    // ── 与交易所对时 ──────────────────────────────────────────────────────────
    // 返回诊断信息而不是 void：这个函数此前是个彻底的黑盒——成功没成功、往返多久、
    // 算出的偏移是多少、有没有被限流闸门压住，全都不对外说。而 -1021 排查恰恰
    // 需要这些：本机时钟慢 2.17 秒、网络往返 270ms，按预算算根本不该失败，
    // 实盘却有 33% 的时间在报错，靠猜是定位不了的。
    struct TimeSyncResult {
        bool        accepted   = false;  // 本次测量是否被采纳
        int64_t     rtt_ms     = 0;      // 整个调用的耗时【含限流闸门的等待】——
                                         // 若闸门把请求压了 30 秒，这里就会显示 30000
        int64_t     offset_ms  = 0;      // 调用结束后生效的偏移
        int64_t     prev_ms    = 0;      // 调用前的偏移（跳变幅度＝时钟被步进的证据）
        const char* skip_reason = "";    // 未采纳的原因

        // 这次测量值不值得打日志。
        //
        // v3.9.6 为排查 -1021 让每次对时都吭声，结果一天 331 条、占了日志的 54%，
        // 把真正的交易信息全淹了。问题查清之后就不该再这么吵：正常对时是常态，
        // 常态不该说话——只有【异常】才值得占用你的注意力。
        //
        // 三种异常：测量被丢弃（网络慢/失败）、偏移大幅跳变（本机时钟被系统步进）。
        // 其余一律静默。
        bool noteworthy() const {
            if (!accepted) return true;
            const int64_t d = offset_ms - prev_ms;
            return d > 500 || d < -500;
        }

        // 排成一行人读的日志。放在这里而不是各调用点各写一份——GUI 和 headless
        // 都要打，格式必须一致，否则两边日志没法对照着看
        std::string to_log() const {
            std::string s = "[对时] 往返 " + std::to_string(rtt_ms) + "ms | ";
            if (accepted) {
                s += "偏移 " + std::to_string(offset_ms) + "ms";
                const int64_t d = offset_ms - prev_ms;
                // 跳变幅度是关键证据：偏移本该只随时钟漂移缓慢变化，一次同步就
                // 跳好几秒，说明本机时钟被系统步进了（macOS 的 timed 会干这事）
                if (d > 500 || d < -500)
                    s += "（较上次跳变 " + std::string(d > 0 ? "+" : "") +
                         std::to_string(d) + "ms ⚠）";
                else
                    s += "（原 " + std::to_string(prev_ms) + "ms）";
            } else {
                s += std::string("未采纳：") + skip_reason +
                     "，保留原偏移 " + std::to_string(prev_ms) + "ms";
            }
            return s;
        }
    };
    TimeSyncResult sync_server_time();

    // ── 测试注入点 ──────────────────────────────────────────────────────────
    // 订单路径（超时→查单恢复、部分成交、零成交、-1111重试）平时【永远不执行】，
    // 只有网络出问题的那几秒才跑，也没法在实盘演练——所以必须能喂假响应。
    // 钩子返回 true 表示"这次由我应答"，false 则照常走真实网络。
    // 生产环境钩子为空，代码路径与未引入前完全一致
    struct FakeReply { long code = 200; std::string headers; std::string body; };
    using TestHook = std::function<bool(const std::string& method, const std::string& path,
                                        const std::string& params, FakeReply& out)>;
    void set_test_hook(TestHook h) { test_hook_ = std::move(h); }

    // 仅测试用：替换掉"墙上时钟"的读数，用来构造【系统时钟被外部程序拨动】
    // 这个场景。它是本类里唯一无法用真实环境复现的分支——总不能让单测去改
    // 机器的系统时间。有了它才能验证 ts_ms() 确实对此免疫（见 test_time_sync ⑧）。
    // 传空还原为真实时钟。生产环境永不调用，代码路径与未引入前一致
    void set_wall_clock_for_test(std::function<int64_t()> f) { wall_hook_ = std::move(f); }

    // 限流状态（界面/日志展示用）
    struct RateStatus { int used_weight, limit, throttled, rejected; bool banned; int64_t ban_left_ms; };
    RateStatus rate_status() const;

    bool is_testnet()   const { return cfg_.testnet; }
    bool is_pm()        const { return cfg_.account_mode == AccountMode::PortfolioMargin; }
    bool is_dual_mode() const override { return dual_mode_; }

    // ── ITradingClient 实现（引擎通过接口调用，实盘走真实下单）──────────────
    OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                    double qty, bool reduce_only) override {
        auto r = place_market(symbol, side, qty, reduce_only);
        return { r.ok, r.order_id, r.error, r.avg_price, r.executed_qty, r.uncertain };
    }
    double round_qty(const std::string& symbol, double qty) override;
    bool   set_leverage(const std::string& symbol, int lev) override;
    // 灾难止损单：STOP_MARKET + closePosition=true。
    // 用 closePosition 而不是"显式数量+reduceOnly"，因为补仓会让仓位不断变大，
    // 显式数量的单子会立刻过期失真；closePosition 永远平掉整个仓位，且仓位归零
    // 时交易所自动撤单，不留垃圾挂单
    std::string place_disaster_stop(const std::string& symbol, double stop_price,
                                    const std::string& entry_side) override;
    bool cancel_disaster_stop(const std::string& symbol,
                              const std::string& order_id) override;

private:
    // 签名端点的逻辑名。普通合约和统一账户的路径不是简单的前缀替换（listenKey 就没有
    // um 前缀），所以用枚举查表，不做字符串拼接，免得漏改一处就打到错误的账户上。
    enum class Ep {
        Account, PositionRisk, OpenOrders, Order, AllOpenOrders,
        PositionSideDual, Leverage, ListenKey,
        PmAccount,     // 统一账户专属：/papi/v1/account（全账户视角，uniMMR 在这里）
        CondOrder,     // 统一账户专属：条件单（STOP_MARKET / TAKE_PROFIT_MARKET）
        Income,        // 资金费/手续费流水
    };
    const char* ep(Ep e) const;

    OrderResult place_cond_market(const std::string& symbol, const char* order_type,
                                   double stop_price, const std::string& entry_side,
                                   double qty);

    Config      cfg_;
    // 限流闸门：所有 HTTP 出口都过它。放在传输层而不是各调用点——
    // 这样订单路径的超时/查单恢复逻辑一行不用动
    std::shared_ptr<RateGate> gate_;
    TestHook    test_hook_;
    std::string base_;       // 签名端点的域名（统一账户 = papi.binance.com）
    std::string pub_base_;   // 公开行情端点的域名（永远是 fapi，papi 没有行情接口）
    // ⚠ 这两个都是【一处写、多处读，且跨线程】，必须是原子的。
    //
    // time_offset_ms_ 由 sync_server_time() 在线程池线程里写，而 ts_ms() 在【每一个
    // 签名请求】里读——引擎池、GUI 线程都会读。做成普通 int64_t 是数据竞争：
    // x86-64 上 64 位对齐读写不会撕裂，所以值不会变成垃圾，真正的风险是【可见性】——
    // 编译器可以把它缓存在寄存器里不再重新加载，于是"看到 -1021 就立即重新对时"
    // 这个自愈路径对那个线程【完全无效】，-1021 会一直反复出现而看不出原因。
    //
    // dual_mode_ 同理：由 set_dual_mode() 写（连接时探测持仓模式），而下单路径每次
    // 都要读它来决定带不带 positionSide。读到陈旧值会让参数组合不对而被交易所拒单。
    std::atomic<bool>    dual_mode_      {false};

    // ── 时间戳锚点 ────────────────────────────────────────────────────────────
    // 锚在【单调时钟】上，不是墙上时钟：steady_ms() + steady_offset = 服务器时间。
    // 完整理由见 ts_ms() 的注释——一句话是同机器上别的程序调 SetSystemTime 时，
    // 我们发出的时间戳不能跟着一起被平移。
    std::atomic<int64_t> steady_offset_ms_ {0};
    std::atomic<bool>    has_anchor_       {false};   // 首次对时前为 false
    // 只用于日志与跳变检测：本机【墙上时钟】相对服务器差多少毫秒。
    // 不参与任何时间戳计算——它存在的意义恰恰是把"系统时钟被谁动了"显示出来，
    // 而 ts_ms() 对此免疫
    std::atomic<int64_t> wall_offset_ms_   {0};

    mutable std::mutex                                sym_mtx_;
    std::unordered_map<std::string, SymbolInfo>       sym_cache_;

    // Persistent CURL pool — reuse TCP+TLS connections across order calls
    static constexpr int kCurlPoolSize = 6;
    struct CurlSlot {
        void*      handle = nullptr;  // CURL*
        void*      hdrs   = nullptr;  // curl_slist*
        std::mutex mtx;
        CurlSlot() = default;
        CurlSlot(const CurlSlot&)            = delete;
        CurlSlot& operator=(const CurlSlot&) = delete;
    };
    CurlSlot curl_pool_[kCurlPoolSize];
    // 公开行情单独一个池，不与签名池共用，两个理由：
    //   ① 主机不同——统一账户下签名走 papi，公开行情永远走 fapi，
    //      共用句柄等于让连接缓存在两个 host 之间来回切，复用率反而下降
    //   ② 公开端点按 IP 计权重，不需要身份，不该携带 X-MBX-APIKEY
    // 补这个池的实测依据：实盘环境下每次新建连接要付完整的 DNS+TCP+TLS 握手，
    // 干净测量的纯网络往返只有 83ms，而框架自己测到 538~693ms，差额就在握手上。
    // 更糟的是它会污染对时——中点估算把握手时间当成对称网络延迟平摊，
    // 于是偏移被系统性抬高约 H/2（实测约 +200ms，与日志基线和干净测量的差值吻合）
    CurlSlot pub_pool_[kCurlPoolSize];

    std::string sign(const std::string& q) const;
    int64_t     ts_ms() const;
    // 墙上时钟读数；测试可经 set_wall_clock_for_test 替换
    int64_t     wall_now_ms() const;
    std::function<int64_t()> wall_hook_;

    AccountInfo fetch_account_futures();
    AccountInfo fetch_account_pm();

    // 带签名（需要 timestamp + HMAC）
    std::string http_get (const std::string& path, std::string params = "");
    std::string http_post(const std::string& path, std::string params = "");
    std::string http_del (const std::string& path, std::string params = "");
    // 不带签名（listenKey 端点只需 X-MBX-APIKEY header）
    std::string http_post_unsigned(const std::string& path, const std::string& body = "");
    void        http_put_unsigned (const std::string& path, const std::string& body = "");
    void        http_del_unsigned (const std::string& path, const std::string& body = "");
    std::string http_get_public  (const std::string& path);
};

} // namespace ccbot
