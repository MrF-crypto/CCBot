#include "net/trading_client.h"
#include "core/indicators.h"
#include <curl/curl.h>
#include <mbedtls/md.h>
#include <simdjson.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdlib>

namespace ccbot {

// ── HMAC-SHA256 via mbedtls ─────────────────────────────────────────────────────
// 跨平台实现（Windows/Linux 通用）：项目本来就通过 ixwebsocket 的 TLS 后端间接
// 依赖 mbedtls，这里直接复用它的 HMAC 接口，不用再额外区分 Windows CNG / Linux OpenSSL
static std::string hmac_sha256(const std::string& key, const std::string& msg) {
    unsigned char result[32] = {};
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info,
                     reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                     reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
                     result);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
    return oss.str();
}

// ── 数量/价格精度工具 ──────────────────────────────────────────────────────────
// 返回 step_size 对应的小数位数（step=1→0, step=0.1→1, step=0.001→3）
static int step_decimals(double step) {
    if (step >= 1.0 - 1e-9) return 0;
    int dp = 0;
    double s = step;
    while (s < 1.0 - 1e-9 && dp < 10) { s *= 10.0; ++dp; }
    return dp;
}

// 按 step_size 精度格式化数量（避免多余小数位导致 Binance -1111）
static std::string fmt_qty(double qty, double step_size) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(step_decimals(step_size)) << qty;
    return oss.str();
}

// 前向声明（定义在文件后部）
static double floor_to_step(double val, double step);

// ── CURL helpers ──────────────────────────────────────────────────────────────
static size_t curl_write(char* ptr, size_t sz, size_t n, void* ud) {
    ((std::string*)ud)->append(ptr, sz * n);
    return sz * n;
}

static CURL* make_curl(const std::string& api_key, std::string& resp,
                        struct curl_slist*& hdrs) {
    CURL* c = curl_easy_init();
    hdrs = curl_slist_append(nullptr, ("X-MBX-APIKEY: " + api_key).c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    return c;
}

// ── simdjson helpers ─────────────────────────────────────────────────────────
static double parse_dbl_str(simdjson::dom::element& obj, const char* key) {
    std::string_view v;
    if (obj[key].get(v) == simdjson::SUCCESS) {
        try { return std::stod(std::string(v)); } catch (...) {}
    }
    double d = 0;
    obj[key].get(d);
    return d;
}

static bool binance_error(simdjson::dom::element& doc, std::string& err) {
    int64_t code = 0;
    if (doc["code"].get(code) == simdjson::SUCCESS && code < 0) {
        std::string_view msg;
        doc["msg"].get(msg);
        err = "[" + std::to_string(code) + "] " + std::string(msg);
        return true;
    }
    return false;
}

// ── TradingClient ─────────────────────────────────────────────────────────────
TradingClient::TradingClient(const Config& cfg) : cfg_(cfg) {
    base_ = cfg.testnet ? "https://testnet.binancefuture.com"
                        : "https://fapi.binance.com";

    // Pre-warm persistent CURL pool: one handle per slot shares TCP+TLS across calls
    for (int i = 0; i < kCurlPoolSize; ++i) {
        CURL* c = curl_easy_init();
        auto* h = curl_slist_append(nullptr, ("X-MBX-APIKEY: " + cfg.api_key).c_str());
        h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 15L);
        curl_pool_[i].handle = c;
        curl_pool_[i].hdrs   = h;
    }
}

TradingClient::~TradingClient() {
    for (auto& slot : curl_pool_) {
        if (slot.handle) curl_easy_cleanup(static_cast<CURL*>(slot.handle));
        if (slot.hdrs)   curl_slist_free_all(static_cast<struct curl_slist*>(slot.hdrs));
    }
}

std::string TradingClient::sign(const std::string& q) const {
    return hmac_sha256(cfg_.api_secret, q);
}

int64_t TradingClient::ts_ms() const {
    using namespace std::chrono;
    auto local = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    return local + time_offset_ms_;
}

std::string TradingClient::http_get_public(const std::string& path) {
    std::string url  = base_ + path;
    std::string resp;
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return resp;
}

void TradingClient::sync_server_time() {
    using namespace std::chrono;
    auto t0   = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    auto resp = http_get_public("/fapi/v1/time");
    auto t1   = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if (resp.empty()) return;

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return;

    int64_t server_time = 0;
    if (doc["serverTime"].get(server_time) != simdjson::SUCCESS) return;

    // 用请求往返的中点估算本机与服务器的时差
    int64_t local_mid   = (t0 + t1) / 2;
    time_offset_ms_     = server_time - local_mid;
}

std::string TradingClient::http_get(const std::string& path, std::string params) {
    if (!params.empty()) {
        params += "&timestamp=" + std::to_string(ts_ms());
        params += "&signature=" + sign(params);
    }
    std::string url = base_ + path + (params.empty() ? "" : "?" + params);
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return resp;
}

std::string TradingClient::http_post(const std::string& path, std::string params) {
    params += "&timestamp=" + std::to_string(ts_ms());
    params += "&signature=" + sign(params);
    std::string url  = base_ + path;
    std::string resp;

    // Acquire a free pooled slot; try non-blocking first, fall back to blocking on slot 0
    std::unique_lock<std::mutex> lk;
    CurlSlot* slot = nullptr;
    for (auto& s : curl_pool_) {
        std::unique_lock<std::mutex> try_lk(s.mtx, std::try_to_lock);
        if (try_lk.owns_lock()) { slot = &s; lk = std::move(try_lk); break; }
    }
    if (!slot) { lk = std::unique_lock<std::mutex>(curl_pool_[0].mtx); slot = &curl_pool_[0]; }

    CURL* c = static_cast<CURL*>(slot->handle);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, params.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)params.size());
    curl_easy_perform(c);
    return resp;
}

std::string TradingClient::http_del(const std::string& path, std::string params) {
    params += "&timestamp=" + std::to_string(ts_ms());
    params += "&signature=" + sign(params);
    std::string url  = base_ + path + "?" + params;
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return resp;
}

// ── API Methods ───────────────────────────────────────────────────────────────
TradingClient::AccountInfo TradingClient::fetch_account() {
    AccountInfo info;
    auto resp = http_get("/fapi/v2/account", "recvWindow=5000");
    if (resp.empty()) { info.error = "无响应"; return info; }

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) {
        info.error = "JSON解析失败";
        return info;
    }
    if (binance_error(doc, info.error)) return info;

    info.total_equity   = parse_dbl_str(doc, "totalMarginBalance");
    info.available      = parse_dbl_str(doc, "availableBalance");
    info.unrealized_pnl = parse_dbl_str(doc, "totalUnrealizedProfit");
    info.ok = true;
    return info;
}

std::vector<TradingClient::Position> TradingClient::fetch_positions() {
    std::vector<Position> result;
    auto resp = http_get("/fapi/v2/positionRisk", "recvWindow=5000");
    if (resp.empty()) return result;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return result;

    for (auto item : arr) {
        std::string_view sym, amt_s, ep_s, mp_s, pnl_s, liq_s;
        int64_t lev = 1;
        item["symbol"].get(sym);
        item["positionAmt"].get(amt_s);
        item["entryPrice"].get(ep_s);
        item["markPrice"].get(mp_s);
        item["unRealizedProfit"].get(pnl_s);
        item["liquidationPrice"].get(liq_s);
        item["leverage"].get(lev);

        double pos_amt = 0;
        try { pos_amt = std::stod(std::string(amt_s)); } catch (...) {}
        if (std::abs(pos_amt) < 1e-10) continue;

        Position pos;
        pos.symbol    = std::string(sym);
        pos.direction = pos_amt > 0 ? 1 : -1;
        pos.qty       = std::abs(pos_amt);
        try { pos.entry_price    = std::stod(std::string(ep_s));  } catch (...) {}
        try { pos.mark_price     = std::stod(std::string(mp_s));  } catch (...) {}
        try { pos.unrealized_pnl = std::stod(std::string(pnl_s)); } catch (...) {}
        try { pos.liq_price      = std::stod(std::string(liq_s)); } catch (...) {}
        pos.notional  = pos.qty * pos.mark_price;
        pos.leverage  = (int)lev;
        result.push_back(pos);
    }
    return result;
}

std::vector<TradingClient::OpenOrder> TradingClient::fetch_open_orders() {
    std::vector<OpenOrder> result;
    auto resp = http_get("/fapi/v1/openOrders", "recvWindow=5000");
    if (resp.empty()) return result;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return result;

    for (auto item : arr) {
        std::string_view sym, side, type, status;
        int64_t oid = 0;
        item["symbol"].get(sym);
        item["side"].get(side);
        item["type"].get(type);
        item["status"].get(status);
        item["orderId"].get(oid);

        OpenOrder ord;
        ord.order_id  = std::to_string(oid);
        ord.symbol    = std::string(sym);
        ord.side      = std::string(side);
        ord.type      = std::string(type);
        ord.status    = std::string(status);
        ord.price     = parse_dbl_str(item, "price");
        ord.orig_qty  = parse_dbl_str(item, "origQty");
        ord.exec_qty  = parse_dbl_str(item, "executedQty");
        result.push_back(ord);
    }
    return result;
}

// 根据持仓模式生成 positionSide 字段
// dual=true  开仓: BUY→LONG  SELL→SHORT
//            平仓(reduce_only): BUY→SHORT  SELL→LONG
// dual=false 使用 reduceOnly，不传 positionSide
static std::string pos_side_param(bool dual, const std::string& side, bool reduce_only) {
    if (!dual) return "";
    // 平仓时方向相反：SELL平多头(LONG)，BUY平空头(SHORT)
    bool is_long_side = reduce_only ? (side == "SELL") : (side == "BUY");
    return "&positionSide=" + std::string(is_long_side ? "LONG" : "SHORT");
}

// 生成本程序专属的 clientOrderId（幂等性标识）：cb-<运行码>-<序号>。
// 运行码 = 启动时刻毫秒时间戳的36进制（跨重启天然唯一）+ 2位随机（防同毫秒双进程）。
// 若跨重启碰撞，超时查单恢复会按 origClientOrderId 查到【历史订单】的成交并错误入账，
// 所以唯一性不是洁癖，是记账正确性的前提
static std::string make_client_order_id() {
    static const std::string run_tag = [] {
        auto ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        static const char cs[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string s;
        for (uint64_t v = ms; v > 0; v /= 36) s += cs[v % 36];
        std::srand((unsigned)(ms ^ (ms >> 17)));
        s += cs[std::rand() % 36];
        s += cs[std::rand() % 36];
        return s;
    }();
    static std::atomic<int> seq{0};
    return "cb-" + run_tag + "-" + std::to_string(seq++);
}

TradingClient::OrderResult TradingClient::query_order(const std::string& sym,
                                                       const std::string& client_order_id) {
    OrderResult r;
    auto resp = http_get("/fapi/v1/order",
                         "symbol=" + sym + "&origClientOrderId=" + client_order_id +
                         "&recvWindow=5000");
    if (resp.empty()) { r.error = "无响应"; r.uncertain = true; return r; }

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) { r.error = "JSON解析失败"; r.uncertain = true; return r; }
    if (binance_error(doc, r.error)) {
        // 只有 -2013（订单不存在）是"确认没到交易所"的可靠答案；其他业务错误
        // （-1021时间戳超窗/-1003限流/-1022签名…）说明【查询本身】没成功，
        // 订单状态依然未知——绝不能让恢复路径误判为"确认未成交"然后放行重试
        r.uncertain = (r.error.find("[-2013]") == std::string::npos);
        return r;
    }

    int64_t oid = 0;
    doc["orderId"].get(oid);
    r.order_id     = std::to_string(oid);
    r.avg_price    = parse_dbl_str(doc, "avgPrice");
    r.executed_qty = parse_dbl_str(doc, "executedQty");
    r.ok = true;
    return r;
}

TradingClient::OrderResult TradingClient::place_market(const std::string& sym,
                                                        const std::string& side,
                                                        double qty, bool reduce_only) {
    OrderResult r;
    const auto& minfo = get_symbol_info(sym);

    // 附加参数（双向持仓 / reduceOnly）
    std::string extra;
    if (dual_mode_)       extra = pos_side_param(true, side, reduce_only);
    else if (reduce_only) extra = "&reduceOnly=true";

    // 部分新上线币种 exchangeInfo 报告的 stepSize 比实际执行精度细（Binance API 不一致）
    // 遇到 -1111 自动放宽精度，最多尝试 4 次：0.001→0.01→0.1→1
    double try_step = minfo.effective_market_step();
    for (int attempt = 0; attempt < 4; ++attempt, try_step *= 10.0) {
        double try_qty = floor_to_step(qty, try_step);
        if (try_qty < minfo.min_qty) { r.error = "数量小于最小下单量"; return r; }

        // 每次尝试用独立的 clientOrderId（-1111 重试是真正的新订单）
        const std::string coid = make_client_order_id();
        std::string body = "symbol=" + sym + "&side=" + side
            + "&type=MARKET&quantity=" + fmt_qty(try_qty, try_step)
            + "&newClientOrderId=" + coid
            + "&newOrderRespType=RESULT&recvWindow=5000" + extra;

        auto resp = http_post("/fapi/v1/order", body);

        // 可疑响应统一走查单恢复：空响应（超时/断网）和"非空但不是JSON"（网关5xx
        // HTML页、Cloudflare拦截页、被截断的响应）都意味着【订单状态未知】——
        // 请求可能已到达交易所并成交。当普通失败返回会让上层重试造成双倍仓位
        simdjson::dom::parser p;
        simdjson::dom::element doc;
        simdjson::padded_string ps(resp);   // 必须活到 doc 使用结束（doc 内字符串指向此缓冲）
        bool suspicious = resp.empty();
        if (!suspicious && p.parse(ps).get(doc) != simdjson::SUCCESS) suspicious = true;
        if (suspicious) {
            for (int q = 0; q < 3; ++q) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                auto qr = query_order(sym, coid);
                if (qr.ok) return qr;                      // 找到了：实际已到达交易所，按真实成交返回
                if (!qr.uncertain) {                       // 明确回答"-2013订单不存在"：确认没下进去
                    r.error = "下单响应异常，已确认订单未到达交易所";
                    return r;
                }
            }
            r.error     = "下单响应异常且无法确认订单状态（网络中断?）";
            r.uncertain = true;
            return r;
        }

        // -1111 = 精度超限，放宽一档重试
        int64_t code = 0;
        doc["code"].get(code);
        if (code == -1111) {
            r.error = "[-1111]";
            continue;
        }

        if (binance_error(doc, r.error)) return r;
        int64_t oid = 0;
        doc["orderId"].get(oid);
        r.order_id     = std::to_string(oid);
        r.avg_price    = parse_dbl_str(doc, "avgPrice");
        r.executed_qty = parse_dbl_str(doc, "executedQty");
        r.ok = true;
        return r;
    }
    return r;  // 返回最后一次 -1111 错误
}

TradingClient::OrderResult TradingClient::place_limit(const std::string& sym,
                                                       const std::string& side,
                                                       double qty, double price,
                                                       bool reduce_only) {
    OrderResult r;
    qty   = round_qty(sym, qty);
    price = round_price(sym, price);
    if (qty <= 0) { r.error = "数量小于最小下单量"; return r; }

    const auto& linfo = get_symbol_info(sym);
    std::ostringstream oss;
    oss << "symbol=" << sym << "&side=" << side
        << "&type=LIMIT&timeInForce=GTC"
        << "&quantity=" << fmt_qty(qty, linfo.step_size)
        << "&price=" << std::fixed << std::setprecision(step_decimals(linfo.tick_size)) << price
        << "&recvWindow=5000";

    if (dual_mode_) {
        oss << pos_side_param(true, side, reduce_only);
    } else if (reduce_only) {
        oss << "&reduceOnly=true";
    }

    auto resp = http_post("/fapi/v1/order", oss.str());
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) { r.error = "JSON解析失败"; return r; }
    if (binance_error(doc, r.error)) return r;

    int64_t oid = 0;
    doc["orderId"].get(oid);
    r.order_id = std::to_string(oid);
    r.ok = true;
    return r;
}

bool TradingClient::fetch_position_mode() {
    auto resp = http_get("/fapi/v1/positionSide/dual", "recvWindow=5000");
    if (resp.empty()) return false;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    bool dual = false;
    doc["dualSidePosition"].get(dual);
    dual_mode_ = dual;
    return dual;
}

bool TradingClient::cancel_order(const std::string& sym, const std::string& order_id) {
    auto resp = http_del("/fapi/v1/order",
        "symbol=" + sym + "&orderId=" + order_id + "&recvWindow=5000");
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    std::string err;
    return !binance_error(doc, err);
}

bool TradingClient::cancel_all_orders(const std::string& sym) {
    auto resp = http_del("/fapi/v1/allOpenOrders",
        "symbol=" + sym + "&recvWindow=5000");
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    std::string err;
    return !binance_error(doc, err);
}

bool TradingClient::close_position(const std::string& sym) {
    // 先拿持仓方向和数量
    auto positions = fetch_positions();
    for (const auto& pos : positions) {
        if (pos.symbol != sym) continue;
        std::string close_side = pos.direction == 1 ? "SELL" : "BUY";
        auto r = place_market(sym, close_side, pos.qty, true);
        return r.ok;
    }
    return false;
}

bool TradingClient::close_all_positions() {
    auto positions = fetch_positions();
    bool all_ok = true;
    for (const auto& pos : positions) {
        std::string close_side = pos.direction == 1 ? "SELL" : "BUY";
        auto r = place_market(pos.symbol, close_side, pos.qty, true);
        if (!r.ok) all_ok = false;
    }
    return all_ok;
}

bool TradingClient::set_leverage(const std::string& sym, int lev) {
    auto resp = http_post("/fapi/v1/leverage",
        "symbol=" + sym + "&leverage=" + std::to_string(lev));
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    int64_t code = 0;
    if (doc["code"].get(code) == simdjson::SUCCESS) {
        if (code == -4046) return true;
        return code == 0;
    }
    return true;
}

// ── unsigned HTTP（listenKey 端点无需签名）────────────────────────────────────
std::string TradingClient::http_post_unsigned(const std::string& path,
                                               const std::string& body) {
    std::string url = base_ + path;
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return resp;
}

void TradingClient::http_put_unsigned(const std::string& path, const std::string& body) {
    std::string url = base_ + path;
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
}

void TradingClient::http_del_unsigned(const std::string& path, const std::string& body) {
    std::string url = base_ + path;
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
}

// ── ListenKey（UserData Stream 用）───────────────────────────────────────────
std::string TradingClient::create_listen_key() {
    auto resp = http_post_unsigned("/fapi/v1/listenKey");
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return "";
    std::string_view key;
    if (doc["listenKey"].get(key) != simdjson::SUCCESS) return "";
    return std::string(key);
}

bool TradingClient::keepalive_listen_key(const std::string& key) {
    http_put_unsigned("/fapi/v1/listenKey", "listenKey=" + key);
    return true;
}

void TradingClient::delete_listen_key(const std::string& key) {
    http_del_unsigned("/fapi/v1/listenKey", "listenKey=" + key);
}

// ── LOT_SIZE / 价格精度缓存 ───────────────────────────────────────────────────
static double parse_step(const std::string& s) {
    if (s.empty()) return 0.001;
    try { return std::stod(s); } catch (...) { return 0.001; }
}

bool TradingClient::try_get_symbol_info(const std::string& sym, SymbolInfo& out) const {
    std::lock_guard<std::mutex> lk(sym_mtx_);
    auto it = sym_cache_.find(sym);
    if (it == sym_cache_.end()) return false;
    out = it->second;
    return true;
}

const TradingClient::SymbolInfo& TradingClient::get_symbol_info(const std::string& sym) {
    {
        std::lock_guard<std::mutex> lk(sym_mtx_);
        auto it = sym_cache_.find(sym);
        // 命中即返回，不管有效与否——这个函数会被 100ms 的 UI 定时器高频调用，
        // 如果品种本身无效（比如用户手滑打错的品种名），之前的逻辑会导致每次都
        // 当场发一个同步 HTTP 请求，把 GUI 线程连续打满导致整个界面卡死（已实测复现）
        if (it != sym_cache_.end()) return it->second;
    }

    SymbolInfo info;
    auto resp = http_get("/fapi/v1/exchangeInfo", "symbol=" + sym);
    if (!resp.empty()) {
        simdjson::dom::parser p;
        simdjson::dom::element doc;
        auto ps = simdjson::padded_string(resp);
        if (p.parse(ps).get(doc) == simdjson::SUCCESS) {
            simdjson::dom::array symbols;
            if (doc["symbols"].get(symbols) == simdjson::SUCCESS) {
                for (auto sym_elem : symbols) {
                    simdjson::dom::array filters;
                    if (sym_elem["filters"].get(filters) != simdjson::SUCCESS) continue;
                    auto safe_stod = [](std::string_view sv) -> double {
                        try { return std::stod(std::string(sv)); }
                        catch (...) { return 0.0; }
                    };

                    bool lot_found = false;
                    for (auto f : filters) {
                        std::string_view ft;
                        if (f["filterType"].get(ft) != simdjson::SUCCESS) continue;
                        if (ft == "LOT_SIZE") {
                            std::string_view step, minq;
                            if (f["stepSize"].get(step) == simdjson::SUCCESS) {
                                double sv = safe_stod(step);
                                if (sv > 0) { info.step_size = sv; lot_found = true; }
                            }
                            if (f["minQty"].get(minq) == simdjson::SUCCESS) {
                                double mv = safe_stod(minq);
                                if (mv > 0) info.min_qty = mv;
                            }
                        } else if (ft == "MARKET_LOT_SIZE") {
                            std::string_view step;
                            if (f["stepSize"].get(step) == simdjson::SUCCESS) {
                                double sv = safe_stod(step);
                                if (sv > 0) info.market_step_size = sv;
                            }
                        } else if (ft == "PRICE_FILTER") {
                            std::string_view tick;
                            if (f["tickSize"].get(tick) == simdjson::SUCCESS) {
                                double tv = safe_stod(tick);
                                if (tv > 0) info.tick_size = tv;
                            }
                        }
                    }
                    info.valid = lot_found;
                    break;
                }
            }
        }
    }

    std::lock_guard<std::mutex> lk(sym_mtx_);
    if (!info.valid) {
        // 拉取失败不写缓存（负缓存会把"首次网络抖动"固化成整个进程生命周期的
        // 错误精度）——返回临时默认值，下次调用重试拉取
        static const SymbolInfo fallback{};
        return fallback;
    }
    sym_cache_[sym] = info;
    return sym_cache_[sym];
}

static double floor_to_step(double val, double step) {
    if (step <= 0) return val;
    double result = std::floor(val / step + 1e-9) * step;
    // 消除浮点误差
    int dp = (int)std::round(-std::log10(step));
    if (dp > 0) {
        double factor = std::pow(10.0, (double)dp);
        result = std::round(result * factor) / factor;
    }
    return result;
}

double TradingClient::round_qty(const std::string& sym, double qty) {
    const auto& info = get_symbol_info(sym);
    double result = floor_to_step(qty, info.step_size);
    return result < info.min_qty ? 0.0 : result;
}

double TradingClient::round_price(const std::string& sym, double price) {
    const auto& info = get_symbol_info(sym);
    return floor_to_step(price, info.tick_size);
}

// ── TP/SL 条件市价单（显式数量 + reduceOnly，兼容所有账户模式）─────────────
TradingClient::OrderResult
TradingClient::place_tp_market(const std::string& sym, double stop_price,
                                const std::string& entry_side, double qty) {
    OrderResult r;
    stop_price = round_price(sym, stop_price);
    if (stop_price <= 0) { r.error = "TP价格取整后为0，跳过"; return r; }

    std::string close_side = (entry_side == "BUY") ? "SELL" : "BUY";
    const auto& info = get_symbol_info(sym);
    const int price_dp = step_decimals(info.tick_size);
    const int qty_dp   = step_decimals(info.effective_market_step());
    qty = round_qty(sym, qty);
    if (qty <= 0) { r.error = "TP数量取整后为0，跳过"; return r; }

    std::ostringstream oss;
    oss << "symbol=" << sym
        << "&side=" << close_side
        << "&type=TAKE_PROFIT_MARKET"
        << "&stopPrice=" << std::fixed << std::setprecision(price_dp) << stop_price
        << "&quantity=" << std::setprecision(qty_dp) << qty
        << "&reduceOnly=true"
        << "&workingType=MARK_PRICE"
        << "&priceProtect=false"
        << "&recvWindow=5000";
    if (dual_mode_)
        oss << "&positionSide=" << ((entry_side == "BUY") ? "LONG" : "SHORT");

    auto resp = http_post("/fapi/v1/order", oss.str());
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) { r.error = "JSON解析失败"; return r; }
    if (binance_error(doc, r.error)) return r;
    int64_t oid = 0; doc["orderId"].get(oid);
    r.order_id = std::to_string(oid); r.ok = true;
    return r;
}

TradingClient::OrderResult
TradingClient::place_sl_market(const std::string& sym, double stop_price,
                                const std::string& entry_side, double qty) {
    OrderResult r;
    stop_price = round_price(sym, stop_price);
    if (stop_price <= 0) { r.error = "SL价格取整后为0，跳过"; return r; }

    std::string close_side = (entry_side == "BUY") ? "SELL" : "BUY";
    const auto& info = get_symbol_info(sym);
    const int price_dp = step_decimals(info.tick_size);
    const int qty_dp   = step_decimals(info.effective_market_step());
    qty = round_qty(sym, qty);
    if (qty <= 0) { r.error = "SL数量取整后为0，跳过"; return r; }

    std::ostringstream oss;
    oss << "symbol=" << sym
        << "&side=" << close_side
        << "&type=STOP_MARKET"
        << "&stopPrice=" << std::fixed << std::setprecision(price_dp) << stop_price
        << "&quantity=" << std::setprecision(qty_dp) << qty
        << "&reduceOnly=true"
        << "&workingType=MARK_PRICE"
        << "&priceProtect=false"
        << "&recvWindow=5000";
    if (dual_mode_)
        oss << "&positionSide=" << ((entry_side == "BUY") ? "LONG" : "SHORT");

    auto resp = http_post("/fapi/v1/order", oss.str());
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) { r.error = "JSON解析失败"; return r; }
    if (binance_error(doc, r.error)) return r;
    int64_t oid = 0; doc["orderId"].get(oid);
    r.order_id = std::to_string(oid); r.ok = true;
    return r;
}

// ── RSI（1h K线，公开接口）────────────────────────────────────────────────────
double TradingClient::fetch_rsi(const std::string& sym,
                                 const std::string& interval, int period) {
    std::string path = "/fapi/v1/klines?symbol=" + sym +
                       "&interval=" + interval +
                       "&limit=" + std::to_string(period + 20);
    auto resp = http_get_public(path);
    if (resp.empty()) return 50.0;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return 50.0;

    std::vector<double> closes;
    for (auto kline : arr) {
        simdjson::dom::array ka;
        if (kline.get_array().get(ka) != simdjson::SUCCESS) continue;
        std::string_view close_s;
        auto it = ka.begin();
        for (int i = 0; i < 4 && it != ka.end(); ++i, ++it); // index 4 = close
        if (it == ka.end()) continue;
        (*it).get(close_s);
        try { closes.push_back(std::stod(std::string(close_s))); } catch (...) {}
    }

    return indicators::rsi(closes, period);
}

// ── BOLL + RSI 快照（一次K线拉取，两个指标一起算）───────────────────────────────
TradingClient::IndicatorSnapshot TradingClient::fetch_indicators(
        const std::string& sym, const std::string& interval,
        int boll_period, double boll_mult, int rsi_period) {
    IndicatorSnapshot out;
    int need = std::max(boll_period, rsi_period + 20) + 5;
    std::string path = "/fapi/v1/klines?symbol=" + sym +
                       "&interval=" + interval +
                       "&limit=" + std::to_string(need);
    auto resp = http_get_public(path);
    if (resp.empty()) return out;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return out;

    std::vector<double> closes;
    for (auto kline : arr) {
        simdjson::dom::array ka;
        if (kline.get_array().get(ka) != simdjson::SUCCESS) continue;
        std::string_view close_s;
        auto it = ka.begin();
        for (int i = 0; i < 4 && it != ka.end(); ++i, ++it); // index 4 = close
        if (it == ka.end()) continue;
        (*it).get(close_s);
        try { closes.push_back(std::stod(std::string(close_s))); } catch (...) {}
    }
    if (closes.empty()) return out;

    out.price = closes.back();

    auto boll = indicators::bollinger(closes, boll_period, boll_mult);
    out.boll_ub = boll.ub;
    out.boll_mb = boll.mb;
    out.boll_lb = boll.lb;
    out.rsi     = indicators::rsi(closes, rsi_period);

    out.ok = boll.ok;
    return out;
}

// ── 高周期趋势快照（趋势状态机）───────────────────────────────────────────────
TradingClient::TrendSnapshot TradingClient::fetch_trend(
        const std::string& sym, const std::string& interval,
        int ema_period, int slope_bars) {
    TrendSnapshot out;
    int need = ema_period + slope_bars + 25;
    std::string path = "/fapi/v1/klines?symbol=" + sym +
                       "&interval=" + interval +
                       "&limit=" + std::to_string(std::min(need, 1500));
    auto resp = http_get_public(path);
    if (resp.empty()) return out;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return out;

    std::vector<double> closes;
    for (auto kline : arr) {
        simdjson::dom::array ka;
        if (kline.get_array().get(ka) != simdjson::SUCCESS) continue;
        std::string_view close_s;
        auto it = ka.begin();
        for (int i = 0; i < 4 && it != ka.end(); ++i, ++it); // index 4 = close
        if (it == ka.end()) continue;
        (*it).get(close_s);
        try { closes.push_back(std::stod(std::string(close_s))); } catch (...) {}
    }
    if ((int)closes.size() < ema_period + slope_bars) return out;   // 新币历史不够，不判定趋势

    out.price   = closes.back();
    out.ema_val = indicators::ema(closes, ema_period);
    double mb_now  = indicators::sma_at(closes, 20, 0);
    double mb_prev = indicators::sma_at(closes, 20, slope_bars);
    if (out.ema_val <= 0 || mb_prev <= 0) return out;
    out.mb_slope_pct = (mb_now - mb_prev) / mb_prev * 100.0;

    // 空头态双条件：价格在 EMA 之下 且 中轨明显下拐（-0.2%阈值防横盘抖动）
    out.bearish = (out.price < out.ema_val) && (out.mb_slope_pct < -0.2);
    out.ok = true;
    return out;
}

// ── 完整 OHLCV K线（SR区域检测用）────────────────────────────────────────────
std::vector<TradingClient::Bar> TradingClient::fetch_bars(
        const std::string& sym, const std::string& interval, int limit) {
    std::vector<Bar> out;
    std::string path = "/fapi/v1/klines?symbol=" + sym +
                       "&interval=" + interval +
                       "&limit=" + std::to_string(std::min(std::max(limit, 1), 1500));
    auto resp = http_get_public(path);
    if (resp.empty()) return out;

    simdjson::dom::parser p;
    simdjson::dom::array arr;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get_array().get(arr) != simdjson::SUCCESS) return out;

    for (auto kline : arr) {
        simdjson::dom::array ka;
        if (kline.get_array().get(ka) != simdjson::SUCCESS) continue;
        // klines 数组下标：1=open 2=high 3=low 4=close 5=volume（都是字符串）
        Bar b;
        int idx = 0;
        bool ok = true;
        for (auto field : ka) {
            if (idx >= 1 && idx <= 5) {
                std::string_view sv;
                if (field.get(sv) != simdjson::SUCCESS) { ok = false; break; }
                double v = 0;
                try { v = std::stod(std::string(sv)); } catch (...) { ok = false; break; }
                switch (idx) {
                case 1: b.open = v; break;
                case 2: b.high = v; break;
                case 3: b.low  = v; break;
                case 4: b.close = v; break;
                case 5: b.volume = v; break;
                }
            }
            if (++idx > 5) break;
        }
        if (ok) out.push_back(b);
    }
    return out;
}

double TradingClient::fetch_mark_price(const std::string& sym) {
    auto resp = http_get_public("/fapi/v1/premiumIndex?symbol=" + sym);
    if (resp.empty()) return 0.0;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return 0.0;
    std::string_view mp;
    if (doc["markPrice"].get(mp) != simdjson::SUCCESS) return 0.0;
    try { return std::stod(std::string(mp)); } catch (...) { return 0.0; }
}

} // namespace ccbot
