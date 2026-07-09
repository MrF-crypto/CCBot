#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace ccbot {

// Binance USDT-M Futures REST API 客户端（同步 CURL + Windows CNG HMAC）
class TradingClient {
public:
    struct Config {
        std::string api_key;
        std::string api_secret;
        bool        testnet = true;
    };

    struct AccountInfo {
        bool   ok               = false;
        double total_equity     = 0;   // totalMarginBalance
        double available        = 0;   // availableBalance
        double unrealized_pnl   = 0;   // totalUnrealizedProfit
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
    };

    explicit TradingClient(const Config& cfg);
    ~TradingClient();

    AccountInfo            fetch_account();
    std::vector<Position>  fetch_positions();
    std::vector<OpenOrder> fetch_open_orders();

    OrderResult place_market(const std::string& symbol, const std::string& side,
                              double qty, bool reduce_only = false);
    OrderResult place_limit (const std::string& symbol, const std::string& side,
                              double qty, double price, bool reduce_only = false);

    bool cancel_order(const std::string& symbol, const std::string& order_id);
    bool cancel_all_orders(const std::string& symbol);
    bool close_position(const std::string& symbol);
    bool close_all_positions();

    bool set_leverage(const std::string& symbol, int lev);

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

    // 拉取标记价格（公开接口，CCG 价格轮询用）
    double fetch_mark_price(const std::string& symbol);

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
    const SymbolInfo& get_symbol_info(const std::string& symbol);
    // 只读缓存，不发网络请求；未命中返回 false —— 给 GUI 线程用，绝不能阻塞界面
    bool try_get_symbol_info(const std::string& symbol, SymbolInfo& out) const;
    double round_qty  (const std::string& symbol, double qty);
    double round_price(const std::string& symbol, double price);

    // ── UserData Stream ─────────────────────────────────────────────────────
    std::string create_listen_key();
    bool        keepalive_listen_key(const std::string& key);
    void        delete_listen_key(const std::string& key);

    // 拉取 Binance 服务器时间，计算本机与服务器的时钟偏移（一次即可）
    void sync_server_time();

    bool is_testnet()   const { return cfg_.testnet; }
    bool is_dual_mode() const { return dual_mode_; }

private:
    Config      cfg_;
    std::string base_;
    bool        dual_mode_       = false;
    int64_t     time_offset_ms_  = 0;   // local_clock + offset = server_clock

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

    std::string sign(const std::string& q) const;
    int64_t     ts_ms() const;

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
