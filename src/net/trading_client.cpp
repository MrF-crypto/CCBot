#include "net/trading_client.h"
#include "net/rate_gate.h"
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

// 响应头收集：币安在 X-MBX-USED-WEIGHT-1M 里回报本分钟已用权重，
// 这是限流闸门的权威数据源（比在本地维护权重表可靠）
static size_t curl_header(char* ptr, size_t sz, size_t n, void* ud) {
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
const char* TradingClient::ep(Ep e) const {
    const bool pm = is_pm();
    switch (e) {
    case Ep::Account:          return pm ? "/papi/v1/um/account"           : "/fapi/v2/account";
    case Ep::PositionRisk:     return pm ? "/papi/v1/um/positionRisk"      : "/fapi/v2/positionRisk";
    case Ep::OpenOrders:       return pm ? "/papi/v1/um/openOrders"        : "/fapi/v1/openOrders";
    case Ep::Order:            return pm ? "/papi/v1/um/order"             : "/fapi/v1/order";
    case Ep::AllOpenOrders:    return pm ? "/papi/v1/um/allOpenOrders"     : "/fapi/v1/allOpenOrders";
    case Ep::PositionSideDual: return pm ? "/papi/v1/um/positionSide/dual" : "/fapi/v1/positionSide/dual";
    case Ep::Leverage:         return pm ? "/papi/v1/um/leverage"          : "/fapi/v1/leverage";
    // listenKey 在统一账户下**没有** um 前缀，是全账户一条流
    case Ep::ListenKey:        return pm ? "/papi/v1/listenKey"            : "/fapi/v1/listenKey";
    case Ep::PmAccount:        return "/papi/v1/account";
    case Ep::CondOrder:        return "/papi/v1/um/conditional/order";
    case Ep::Income:           return pm ? "/papi/v1/um/income"            : "/fapi/v1/income";
    }
    return "";
}

TradingClient::TradingClient(const Config& cfg) : cfg_(cfg) {
    // 统一账户(papi) 的 IP 权重上限比 fapi 高一倍多
    RateGate::Limits lim;
    lim.weight_per_min = is_pm() ? 6000 : 2400;
    gate_ = std::make_shared<RateGate>(lim);

    // 公开行情永远走 fapi：papi 域名下**不存在** exchangeInfo/klines/premiumIndex/time，
    // 打过去一律 404。所以行情和签名走两个 base，不能合并。
    pub_base_ = cfg.testnet ? "https://testnet.binancefuture.com"
                            : "https://fapi.binance.com";
    base_ = is_pm() ? "https://papi.binance.com" : pub_base_;

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

    // 公开行情池：同样预热，但【不挂 API Key header】——这类端点按 IP 计权重
    for (int i = 0; i < kCurlPoolSize; ++i) {
        CURL* c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 15L);
        pub_pool_[i].handle = c;
        pub_pool_[i].hdrs   = nullptr;
    }
}

TradingClient::~TradingClient() {
    for (auto& slot : curl_pool_) {
        if (slot.handle) curl_easy_cleanup(static_cast<CURL*>(slot.handle));
        if (slot.hdrs)   curl_slist_free_all(static_cast<struct curl_slist*>(slot.hdrs));
    }
    for (auto& slot : pub_pool_) {
        if (slot.handle) curl_easy_cleanup(static_cast<CURL*>(slot.handle));
    }
}

std::string TradingClient::sign(const std::string& q) const {
    return hmac_sha256(cfg_.api_secret, q);
}

// 墙上时钟（会被 SetSystemTime 改动）与单调时钟（不会）各取一个毫秒读数
static int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
static int64_t mono_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t TradingClient::ts_ms() const {
    // 刻意回拨1秒：币安对"时间戳超前"零容忍（>1000ms直接-1021拒绝），对"滞后"
    // 有 recvWindow=5000ms 的宽容——把时间戳往安全的一侧靠。
    //
    // ⚠ 锚点是【单调时钟】而不是墙上时钟。起因是实盘抓到的一个真实故障：
    // 同一台机器上另一个程序（第三方交易机器人）每 5 分钟调一次 SetSystemTime
    // 把系统时钟拨到整秒，而它算出来的那个整秒偶尔会差一秒——安全日志实录：
    //     旧 15:24:19.0738746Z → 新 15:24:18.0000000Z   （往回 1074ms）
    // 若锚在墙上时钟上，这一拨会直接平移我们发出的每一个时间戳，而超前侧的
    // 预算只有 2000ms，一次就吃掉一半。
    //
    // steady_clock 是单调的，SetSystemTime 对它没有任何影响，所以别的程序
    // 怎么拨系统时钟都与我们无关。代价只有晶振漂移（典型 10~50ppm，
    // 15 分钟对时间隔内累计 0.04~0.18ms），可以忽略。
    if (has_anchor_.load(std::memory_order_acquire))
        return mono_ms() + steady_offset_ms_.load(std::memory_order_relaxed) - 1000;

    // 还没成功对过时：退回墙上时钟。此刻本来也没有可用偏移，两者等价
    return wall_now_ms() - 1000;
}

int64_t TradingClient::wall_now_ms() const {
    return wall_hook_ ? wall_hook_() : wall_ms();
}

std::string TradingClient::http_get_public(const std::string& path) {
    if (test_hook_) {
        FakeReply fr;
        if (test_hook_("GET", path, "", fr)) {
            gate_->observe(fr.code, fr.headers, fr.body);
            return fr.body;
        }
    }
    std::string url  = pub_base_ + path;
    std::string resp, rhdr;

    // 从公开池取一个空闲槽：先非阻塞轮询，全忙则阻塞等 0 号槽（与订单池同一策略）
    std::unique_lock<std::mutex> lk;
    CurlSlot* slot = nullptr;
    for (auto& s : pub_pool_) {
        std::unique_lock<std::mutex> try_lk(s.mtx, std::try_to_lock);
        if (try_lk.owns_lock()) { slot = &s; lk = std::move(try_lk); break; }
    }
    if (!slot) { lk = std::unique_lock<std::mutex>(pub_pool_[0].mtx); slot = &pub_pool_[0]; }

    // 公开行情按 IP 计权重，和签名请求共用同一个配额——这里是请求量最大的一类
    // （31品种×每5分钟的指标+趋势批次），突发风险主要来自它
    gate_->acquire(false);
    CURL* c = static_cast<CURL*>(slot->handle);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);      // 槽只用于 GET，显式复位以防将来被复用
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &rhdr);
    curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    gate_->observe(code, rhdr, resp);
    return resp;
}

// 往返超过这个时长就丢弃本次测量。
//
// 中点估算 local_mid=(t0+t1)/2 隐含了一个假设：去程与回程耗时相同。网络正常时
// 这没问题（实测往返 250~290ms，最大误差 ~145ms）；但网络一慢，去回程往往是
// 不对称的，一次 8 秒的往返最坏能算出 4 秒的偏移误差——而整个 recvWindow 预算
// 才 4 秒。更糟的是方向：回程慢会让偏移偏负，时间戳更旧，于是更容易 -1021，
// 自愈越修越坏，形成自我强化。
//
// 阈值取 2000ms（正常值的 7 倍以上）：最坏误差 1 秒，仍在预算内；同时足够宽松，
// 不会把偶尔的抖动也拒掉。丢弃后保留原偏移——宁可用一个略旧的好值，
// 也不要一个当场测出来的坏值。
static constexpr int64_t kMaxSyncRttMs = 2000;

// 择优采样的三个参数（见 sync_server_time 的说明）：
//   最多采几次；快到什么程度就不必再采；总耗时预算
// kSyncGoodRttMs 取 150ms：实盘热连接实测中位 84ms、P95 101ms，所以常态下
// 第一枪就达标、直接收工——不给正常情况增加任何请求。冷连接（747ms）和
// 隧道卡顿（991ms）都远在阈值之上，正是需要补枪的那些场合。
// kSyncBudgetMs 取 2000ms 与 kMaxSyncRttMs 对齐：已经花掉这么久说明网络此刻
// 不健康，再采也救不回精度，只会把调用线程堵得更久。
static constexpr int     kSyncSamples   = 3;
static constexpr int64_t kSyncGoodRttMs = 150;
static constexpr int64_t kSyncBudgetMs  = 2000;

TradingClient::TimeSyncResult TradingClient::sync_server_time() {
    using namespace std::chrono;
    TimeSyncResult r;
    // 跳变检测比的是【墙上时钟偏移】——它正是"系统时钟被谁动了"的度量。
    // 现在 ts_ms() 已经对此免疫，所以这条日志的定位从"告警"变成了"取证"
    r.prev_ms = wall_offset_ms_.load();
    r.offset_ms = r.prev_ms;

    // ── 择优采样：最多采 kSyncSamples 次，取【往返最短】的那一次 ──────────────
    // NTP 的标准做法。理由是中点估算的误差上界恰好是 ±RTT/2 —— 往返越短，
    // 去回程不对称能造成的偏差就越小，所以最快的样本必然是最可信的样本。
    //
    // 为什么需要它：单次采样时，一次慢样本（连接重建、隧道卡顿）就会把偏移
    // 写偏几百毫秒，而那个坏值要一直用到下次对时。实盘实测过这个形态——
    // 冷连接那一次往返 747ms、偏移 293ms，紧接着热连接往返 86ms、偏移 −14ms，
    // 差的 307ms 正好是握手时长的一半。
    //
    // 代价被压到几乎为零：第一个样本只要够快就直接收工，所以常态（热连接
    // 80ms 左右）根本不会有第二次请求。只有第一枪打偏时才补枪，
    // 而那正是需要冗余的时候。
    int64_t best_rtt = INT64_MAX;
    int64_t best_steady_off = 0;    // 喂给 ts_ms() 的那个（锚在单调时钟上）
    int64_t best_wall_off   = 0;    // 只用于日志与跳变检测
    bool    got = false;
    const auto began = steady_clock::now();

    for (int i = 0; i < kSyncSamples; ++i) {
        // 两个时钟在同一时刻各取一次读数：单调的那个用来做锚点，
        // 墙上的那个只为算出"系统时钟偏了多少"给人看
        auto m0 = mono_ms();
        auto w0 = wall_now_ms();
        // 注意：http_get_public 内部会先过限流闸门，闸门在权重逼近上限时会阻塞——
        // 所以下面这个 rtt 是【含闸门等待】的总耗时。这是刻意的：如果对时被闸门
        // 压了几十秒，日志里会直接看到一个几万毫秒的 rtt，一眼就能定位
        auto resp = http_get_public("/fapi/v1/time");
        auto m1 = mono_ms();
        auto w1 = wall_now_ms();
        // 往返用单调时钟量：墙上时钟若在这中间被别的程序拨了，量出来的往返会是
        // 负数或荒谬的大数，进而把中点估算和阈值判断一起带偏
        const int64_t rtt = m1 - m0;
        // rtt 始终反映"最近一次实际测量"，除非后面有更优样本把它替换掉
        if (!got) r.rtt_ms = rtt;

        do {
            if (resp.empty())           { r.skip_reason = "无响应";        break; }
            simdjson::dom::parser p;
            simdjson::dom::element doc;
            auto ps = simdjson::padded_string(resp);
            if (p.parse(ps).get(doc) != simdjson::SUCCESS)
                                        { r.skip_reason = "JSON解析失败";  break; }
            int64_t server_time = 0;
            if (doc["serverTime"].get(server_time) != simdjson::SUCCESS)
                                        { r.skip_reason = "无serverTime";  break; }
            if (rtt > kMaxSyncRttMs)    { r.skip_reason = "往返过长，中点估算不可信"; break; }

            if (rtt < best_rtt) {
                best_rtt = rtt;
                // 用请求往返的中点估算时差。两个锚点各算一份：
                //   单调 —— 之后 ts_ms() 就靠它，不受任何人改系统时钟的影响
                //   墙上 —— 只进日志，用来暴露"系统时钟被谁动了"
                best_steady_off = server_time - (m0 + m1) / 2;
                best_wall_off   = server_time - (w0 + w1) / 2;
                got = true;
                r.rtt_ms = rtt;
                r.skip_reason = "";     // 已有可用样本，之前的失败不再是结论
            }
        } while (false);

        // 够快就收工——常态下这里第一轮就返回，不产生任何额外请求
        if (got && best_rtt <= kSyncGoodRttMs) break;
        // 已经花掉的时间超过预算就别再补枪了：网络此刻明显不健康，
        // 多采几次既救不回精度，还会把调用线程堵得更久
        if (duration_cast<milliseconds>(steady_clock::now() - began).count() >= kSyncBudgetMs)
            break;
    }

    if (!got) return r;                  // 一个可用样本都没有：保留原偏移

    // 先写偏移、再置 has_anchor_（release），保证读侧一旦看到锚点有效，
    // 读到的偏移一定是配套的那个
    steady_offset_ms_.store(best_steady_off, std::memory_order_relaxed);
    has_anchor_.store(true, std::memory_order_release);
    wall_offset_ms_.store(best_wall_off);

    r.offset_ms = best_wall_off;         // 对外汇报的是【人能看懂的那个】：系统时钟差多少
    r.accepted  = true;
    return r;
}

std::string TradingClient::http_get(const std::string& path, std::string params) {
    std::string resp, rhdr;
    struct curl_slist* hdrs = nullptr;
    // ⚠ 顺序至关重要：先过闸门，【放行之后】才算时间戳并签名。
    //
    // 反过来（先签名再等闸门）会制造一个隐蔽的 -1021 工厂：闸门在权重逼近上限时
    // 会阻塞，实测最长压过 44 秒（对时诊断日志里往返 P99=20.6s、max=44.2s，而
    // 中位数只有 596ms——那条长尾只可能来自闸门）。请求带着 44 秒前的时间戳抵达
    // 币安，而 recvWindow 只有 5 秒，必然被判 "Timestamp outside of recvWindow"。
    //
    // 这正是那台 macOS 实盘 33% 时间在报 -1021 的真正原因：与本机时钟无关
    // （诊断显示偏移中位数只有 300ms），纯粹是"签名早、发送晚"。
    gate_->acquire(false);
    if (!params.empty()) {
        params += "&timestamp=" + std::to_string(ts_ms());
        params += "&signature=" + sign(params);
    }
    // 测试钩子放在签名【之后】：它拦截的应当是"即将发出的那个请求"，
    // 而不是刚进函数、还没加时间戳的半成品。用例⑤靠这一点验证时间戳的新鲜度
    if (test_hook_) {
        FakeReply fr;
        if (test_hook_("GET", path, params, fr)) {
            gate_->observe(fr.code, fr.headers, fr.body);
            return fr.body;
        }
    }
    std::string url = base_ + path + (params.empty() ? "" : "?" + params);
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &rhdr);
    curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    gate_->observe(code, rhdr, resp);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return resp;
}

std::string TradingClient::http_post(const std::string& path, std::string params) {
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

    // 订单请求享有优先权：只在真正被封禁时才等，不参与权重软限速。
    // 平仓/止损延迟直接对应资金损失，而拉一次指标晚几秒无所谓
    gate_->acquire(true);

    // ⚠ 时间戳必须在【闸门放行之后】才算——顺序反了就是个 -1021 工厂，
    //   详见 http_get 里的说明。订单路径虽然享有优先权、极少被压，
    //   但被封禁时同样会等，所以这里同样不能提前签名
    params += "&timestamp=" + std::to_string(ts_ms());
    params += "&signature=" + sign(params);

    // 测试钩子放在签名之后：拦截的应当是"即将发出的那个请求"，而不是半成品
    if (test_hook_) {
        FakeReply fr;
        if (test_hook_("POST", path, params, fr)) {
            gate_->observe(fr.code, fr.headers, fr.body);
            return fr.body;
        }
    }

    std::string rhdr;
    CURL* c = static_cast<CURL*>(slot->handle);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, params.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)params.size());
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &rhdr);
    curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    gate_->observe(code, rhdr, resp);
    return resp;
}

std::string TradingClient::http_del(const std::string& path, std::string params) {
    std::string resp, rhdr;
    struct curl_slist* hdrs = nullptr;
    gate_->acquire(true);          // 撤单同属订单类，优先
    // ⚠ 闸门放行之后才算时间戳，详见 http_get 里的说明
    params += "&timestamp=" + std::to_string(ts_ms());
    params += "&signature=" + sign(params);
    if (test_hook_) {
        FakeReply fr;
        if (test_hook_("DELETE", path, params, fr)) {
            gate_->observe(fr.code, fr.headers, fr.body);
            return fr.body;
        }
    }
    std::string url  = base_ + path + "?" + params;
    CURL* c = make_curl(cfg_.api_key, resp, hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &rhdr);
    curl_easy_perform(c);
    long dcode = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &dcode);
    gate_->observe(dcode, rhdr, resp);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return resp;
}

// ── API Methods ───────────────────────────────────────────────────────────────

// 取字段并报告"到底有没有取到"。统一账户的部分字段币安会返回空串（""），
// parse_dbl_str 对空串返回 0，光看返回值分不清"真的是0"和"没这个字段"——
// 而这两者在余额展示上含义完全不同，所以要单独把 found 传出来。
static double parse_dbl_str_opt(simdjson::dom::element obj, const char* key, bool& found) {
    found = false;
    std::string_view v;
    if (obj[key].get(v) == simdjson::SUCCESS) {
        if (v.empty()) return 0.0;
        try { double d = std::stod(std::string(v)); found = true; return d; }
        catch (...) { return 0.0; }
    }
    double d = 0;
    if (obj[key].get(d) == simdjson::SUCCESS) { found = true; return d; }
    return 0.0;
}

TradingClient::AccountInfo TradingClient::fetch_account() {
    return is_pm() ? fetch_account_pm() : fetch_account_futures();
}

TradingClient::AccountInfo TradingClient::fetch_account_futures() {
    AccountInfo info;
    auto resp = http_get(ep(Ep::Account), "recvWindow=5000");
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

// 统一账户的余额要两个接口一起看：
//   /papi/v1/um/account —— UM（U本位合约）子账户视角，字段名与 fapi 一致
//   /papi/v1/account    —— 全账户视角，uniMMR / accountEquity / totalAvailableBalance
// 强平是按**全账户**算的，所以 uniMMR 必须取到；而"可用于开仓的钱"用全账户口径
// （totalAvailableBalance）才对——现货抵押品折算进来的额度只体现在这里。
TradingClient::AccountInfo TradingClient::fetch_account_pm() {
    AccountInfo info;
    simdjson::dom::parser p1, p2;

    // ① UM 子账户：未实现盈亏，以及 totalAvailableBalance 缺失时的兜底
    auto um_resp = http_get(ep(Ep::Account), "recvWindow=5000");
    if (um_resp.empty()) { info.error = "统一账户无响应（papi）"; return info; }

    simdjson::dom::element um;
    auto um_ps = simdjson::padded_string(um_resp);
    if (p1.parse(um_ps).get(um) != simdjson::SUCCESS) {
        info.error = "统一账户JSON解析失败";
        return info;
    }
    if (binance_error(um, info.error)) return info;

    bool f = false;
    info.unrealized_pnl = parse_dbl_str_opt(um, "totalUnrealizedProfit", f);
    double um_equity    = parse_dbl_str_opt(um, "totalMarginBalance", f);
    bool   has_um_eq    = f;
    double um_avail     = parse_dbl_str_opt(um, "availableBalance", f);
    bool   has_um_av    = f;

    // 顶层没有汇总字段时，从 assets[] 里挑 USDT 那条（币安两种形态都出现过）
    if (!has_um_eq || !has_um_av) {
        simdjson::dom::array assets;
        if (um["assets"].get(assets) == simdjson::SUCCESS) {
            for (auto a : assets) {
                std::string_view name;
                if (a["asset"].get(name) != simdjson::SUCCESS || name != "USDT") continue;
                if (!has_um_eq) { um_equity = parse_dbl_str_opt(a, "marginBalance",    f); has_um_eq = f; }
                if (!has_um_av) { um_avail  = parse_dbl_str_opt(a, "availableBalance", f); has_um_av = f; }
                bool fu = false;
                double upnl = parse_dbl_str_opt(a, "unrealizedProfit", fu);
                if (fu && info.unrealized_pnl == 0.0) info.unrealized_pnl = upnl;
                break;
            }
        }
    }

    // ② 全账户：uniMMR + 权益 + 可用
    auto acc_resp = http_get(ep(Ep::PmAccount), "recvWindow=5000");
    double pm_equity = 0, pm_avail = 0;
    bool has_pm_eq = false, has_pm_av = false;
    if (!acc_resp.empty()) {
        simdjson::dom::element acc;
        auto acc_ps = simdjson::padded_string(acc_resp);
        if (p2.parse(acc_ps).get(acc) == simdjson::SUCCESS) {
            std::string ignore;
            if (!binance_error(acc, ignore)) {
                info.uni_mmr = parse_dbl_str_opt(acc, "uniMMR", f);
                pm_equity    = parse_dbl_str_opt(acc, "accountEquity", f);        has_pm_eq = f;
                pm_avail     = parse_dbl_str_opt(acc, "totalAvailableBalance", f); has_pm_av = f;
            }
        }
    }

    info.total_equity = has_pm_eq ? pm_equity : um_equity;
    info.available    = has_pm_av ? pm_avail  : um_avail;

    // 两个接口都没给出权益 —— 宁可报连接失败，也不要在界面上显示"权益 $0"：
    // 那会让人以为账户是空的，或者误判风控还有多少余量
    if (!has_pm_eq && !has_um_eq) {
        info.error = "统一账户余额字段解析失败（papi 返回结构与预期不符）";
        return info;
    }
    info.ok = true;
    return info;
}

std::vector<TradingClient::Position> TradingClient::fetch_positions() {
    std::vector<Position> result;
    auto resp = http_get(ep(Ep::PositionRisk), "recvWindow=5000");
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
    auto resp = http_get(ep(Ep::OpenOrders), "recvWindow=5000");
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
    auto resp = http_get(ep(Ep::Order),
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

// 下单遇到 -1021 时就地重发的次数。
// 取 2 而不是更多：这类停顿是离散的秒级事件，两次重发（间隔 250ms）足以跨过去；
// 再多只会在真正的持续性故障里拖长每一次下单的耗时，而那种情况本就该让上层看见
static constexpr int kTimestampRetries = 2;

// 响应是否为【可确认的】-1021（时间戳超出 recvWindow）。
//
// 判定必须严格：只有合法 JSON 且 code 明确等于 -1021 才算。空响应、非 JSON、
// 网关错误页一律不算——那些是"订单状态未知"，必须走查单恢复，绝不能重发。
//
// 为什么 -1021 可以安全重发：recvWindow 校验发生在订单进撮合引擎【之前】，
// 校验没过订单根本不会被创建。所以它是"确定没成交"的证据，
// 与超时/空响应那种"可能已经成交"是性质完全不同的两类失败。
static bool is_timestamp_reject(const std::string& resp) {
    if (resp.empty()) return false;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    simdjson::padded_string ps(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    int64_t code = 0;
    if (doc["code"].get(code) != simdjson::SUCCESS) return false;
    return code == -1021;
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

        auto resp = http_post(ep(Ep::Order), body);

        // -1021 就地重发：网络（尤其 TCP 隧道）的秒级停顿会让时间戳抵达时已超窗，
        // 而这类停顿是离散的、过去就好。时间戳在 http_post 内部每次重算，
        // 所以重发一次通常就落回窗口内了。
        // 【复用同一个 clientOrderId】——不像 -1111 那样换新的。-1111 是精度被拒、
        // 重试是真正的新订单；-1021 则是同一笔订单的重发，复用 ID 等于多一层保险：
        // 万一"确定没成交"这个判断有误、原单真进去了，重复 ID 会被币安拒掉，
        // 而不是开出第二个仓位
        for (int ts_retry = 0;
             ts_retry < kTimestampRetries && is_timestamp_reject(resp);
             ++ts_retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            resp = http_post(ep(Ep::Order), body);
        }

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

    auto resp = http_post(ep(Ep::Order), oss.str());
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
    auto resp = http_get(ep(Ep::PositionSideDual), "recvWindow=5000");
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
    auto resp = http_del(ep(Ep::Order),
        "symbol=" + sym + "&orderId=" + order_id + "&recvWindow=5000");
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    std::string err;
    return !binance_error(doc, err);
}

bool TradingClient::cancel_all_orders(const std::string& sym) {
    auto resp = http_del(ep(Ep::AllOpenOrders),
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
    auto resp = http_post(ep(Ep::Leverage),
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
    auto resp = http_post_unsigned(ep(Ep::ListenKey));
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return "";
    std::string_view key;
    if (doc["listenKey"].get(key) != simdjson::SUCCESS) return "";
    return std::string(key);
}

bool TradingClient::keepalive_listen_key(const std::string& key) {
    http_put_unsigned(ep(Ep::ListenKey), "listenKey=" + key);
    return true;
}

void TradingClient::delete_listen_key(const std::string& key) {
    http_del_unsigned(ep(Ep::ListenKey), "listenKey=" + key);
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

TradingClient::SymbolInfo TradingClient::get_symbol_info(const std::string& sym) {
    {
        std::lock_guard<std::mutex> lk(sym_mtx_);
        auto it = sym_cache_.find(sym);
        // 命中即返回，不管有效与否——这个函数会被 100ms 的 UI 定时器高频调用，
        // 如果品种本身无效（比如用户手滑打错的品种名），之前的逻辑会导致每次都
        // 当场发一个同步 HTTP 请求，把 GUI 线程连续打满导致整个界面卡死（已实测复现）
        if (it != sym_cache_.end()) return it->second;
    }

    SymbolInfo info;
    auto resp = http_get_public("/fapi/v1/exchangeInfo?symbol=" + sym);
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
        // 错误精度）——返回默认值，下次调用重试拉取
        return SymbolInfo{};
    }
    sym_cache_[sym] = info;
    return info;
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
// 统一账户下条件单**不走** /papi/v1/um/order —— 那个端点只收 LIMIT/MARKET，
// 发 STOP_MARKET 会被拒。条件单是独立的一套：
//   路径   /papi/v1/um/conditional/order
//   参数   type → strategyType
//   返回   orderId → strategyId（撤单时也要用 strategyId，不是 orderId）
TradingClient::OrderResult
TradingClient::place_cond_market(const std::string& sym, const char* order_type,
                                  double stop_price, const std::string& entry_side,
                                  double qty) {
    OrderResult r;
    stop_price = round_price(sym, stop_price);
    if (stop_price <= 0) { r.error = std::string(order_type) + "价格取整后为0，跳过"; return r; }

    std::string close_side = (entry_side == "BUY") ? "SELL" : "BUY";
    const auto& info = get_symbol_info(sym);
    const int price_dp = step_decimals(info.tick_size);
    const int qty_dp   = step_decimals(info.effective_market_step());
    qty = round_qty(sym, qty);
    if (qty <= 0) { r.error = std::string(order_type) + "数量取整后为0，跳过"; return r; }

    const bool pm = is_pm();
    std::ostringstream oss;
    oss << "symbol=" << sym
        << "&side=" << close_side
        << (pm ? "&strategyType=" : "&type=") << order_type
        << "&stopPrice=" << std::fixed << std::setprecision(price_dp) << stop_price
        << "&quantity=" << std::setprecision(qty_dp) << qty
        << "&reduceOnly=true"
        << "&workingType=MARK_PRICE"
        << "&priceProtect=false"
        << "&recvWindow=5000";
    if (dual_mode_)
        oss << "&positionSide=" << ((entry_side == "BUY") ? "LONG" : "SHORT");

    auto resp = http_post(pm ? ep(Ep::CondOrder) : ep(Ep::Order), oss.str());
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) { r.error = "JSON解析失败"; return r; }
    if (binance_error(doc, r.error)) return r;
    int64_t oid = 0;
    doc[pm ? "strategyId" : "orderId"].get(oid);
    r.order_id = std::to_string(oid); r.ok = true;
    return r;
}

// ── 灾难止损单：STOP_MARKET + closePosition ───────────────────────────────────
// 与 place_cond_market 的区别：不带 quantity / reduceOnly（币安对 closePosition
// 同时带这两者会直接拒单），仓位平掉后交易所自动撤销。
std::string TradingClient::place_disaster_stop(const std::string& sym, double stop_price,
                                                const std::string& entry_side) {
    stop_price = round_price(sym, stop_price);
    if (stop_price <= 0) return "";

    const std::string close_side = (entry_side == "BUY") ? "SELL" : "BUY";
    const auto& info = get_symbol_info(sym);
    const int price_dp = step_decimals(info.tick_size);
    const bool pm = is_pm();

    std::ostringstream oss;
    oss << "symbol=" << sym
        << "&side=" << close_side
        << (pm ? "&strategyType=" : "&type=") << "STOP_MARKET"
        << "&stopPrice=" << std::fixed << std::setprecision(price_dp) << stop_price
        << "&closePosition=true"
        << "&workingType=MARK_PRICE"   // 用标记价，避免插针成交价误触发
        << "&priceProtect=true"
        << "&recvWindow=5000";
    if (dual_mode_)
        oss << "&positionSide=" << ((entry_side == "BUY") ? "LONG" : "SHORT");

    auto resp = http_post(pm ? ep(Ep::CondOrder) : ep(Ep::Order), oss.str());
    if (resp.empty()) return "";
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return "";
    std::string err;
    if (binance_error(doc, err)) return "";
    int64_t oid = 0;
    doc[pm ? "strategyId" : "orderId"].get(oid);
    return oid > 0 ? std::to_string(oid) : "";
}

bool TradingClient::cancel_disaster_stop(const std::string& sym,
                                          const std::string& order_id) {
    if (order_id.empty()) return true;
    const bool pm = is_pm();
    // 统一账户的条件单不在普通撤单端点上，且用 strategyId 而不是 orderId
    auto resp = http_del(pm ? ep(Ep::CondOrder) : ep(Ep::Order),
        "symbol=" + sym + (pm ? "&strategyId=" : "&orderId=") + order_id +
        "&recvWindow=5000");
    if (resp.empty()) return false;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return false;
    std::string err;
    if (binance_error(doc, err)) {
        // -2011 Unknown order：单子已经不在了（已触发/已被交易所自动撤销），
        // 对调用方而言目的已达成，算成功——否则会陷入无意义的重试
        return err.find("-2011") != std::string::npos;
    }
    return true;
}

TradingClient::OrderResult
TradingClient::place_tp_market(const std::string& sym, double stop_price,
                                const std::string& entry_side, double qty) {
    return place_cond_market(sym, "TAKE_PROFIT_MARKET", stop_price, entry_side, qty);
}

TradingClient::OrderResult
TradingClient::place_sl_market(const std::string& sym, double stop_price,
                                const std::string& entry_side, double qty) {
    return place_cond_market(sym, "STOP_MARKET", stop_price, entry_side, qty);
}

// ── K线拉取+解析（统一入口）─────────────────────────────────────────────────
// 原先 fetch_rsi / fetch_indicators / fetch_trend / fetch_bars 四处各自拼一遍
// URL、各自解析一遍数组，四份几乎相同的代码。改动 K 线口径时漏掉一处就会出现
// "指标和趋势用的不是同一批数据"这种极难查的问题，所以合成一处。
// 只要收盘价的调用方多解析 4 个字段，几百根 K 线的开销可以忽略。
std::vector<TradingClient::Bar> TradingClient::fetch_klines(
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

    out.reserve(arr.size());
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

static std::vector<double> closes_of(const std::vector<TradingClient::Bar>& bars) {
    std::vector<double> c;
    c.reserve(bars.size());
    for (const auto& b : bars) c.push_back(b.close);
    return c;
}

// ── RSI（1h K线，公开接口）────────────────────────────────────────────────────
double TradingClient::fetch_rsi(const std::string& sym,
                                 const std::string& interval, int period) {
    auto closes = closes_of(fetch_klines(sym, interval, period + 20));
    if (closes.empty()) return 50.0;
    return indicators::rsi(closes, period);
}

// ── BOLL + RSI 快照（一次K线拉取，两个指标一起算）───────────────────────────────
TradingClient::IndicatorSnapshot TradingClient::fetch_indicators(
        const std::string& sym, const std::string& interval,
        int boll_period, double boll_mult, int rsi_period) {
    IndicatorSnapshot out;
    int need = std::max(boll_period, rsi_period + 20) + 5;
    auto closes = closes_of(fetch_klines(sym, interval, need));
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
    auto closes = closes_of(fetch_klines(sym, interval, need));
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
    return fetch_klines(sym, interval, limit);
}

TradingClient::PremiumInfo TradingClient::fetch_premium(const std::string& sym) {
    PremiumInfo info;
    auto resp = http_get_public("/fapi/v1/premiumIndex?symbol=" + sym);
    if (resp.empty()) return info;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return info;
    std::string_view mp;
    if (doc["markPrice"].get(mp) != simdjson::SUCCESS) return info;
    try { info.mark_price = std::stod(std::string(mp)); } catch (...) { return info; }
    if (info.mark_price <= 0) return info;
    std::string_view fr;
    if (doc["lastFundingRate"].get(fr) == simdjson::SUCCESS) {
        try { info.funding_rate = std::stod(std::string(fr)); } catch (...) {}
    }
    doc["nextFundingTime"].get(info.next_ms);
    info.ok = true;
    return info;
}

double TradingClient::fetch_mark_price(const std::string& sym) {
    return fetch_premium(sym).mark_price;
}

std::vector<TradingClient::FundingRecord>
TradingClient::fetch_funding_income(int64_t start_ms, int64_t end_ms,
                                     const std::string& sym) {
    std::vector<FundingRecord> out;
    std::string params = "incomeType=FUNDING_FEE&limit=1000";
    if (!sym.empty())   params += "&symbol=" + sym;
    if (start_ms > 0)   params += "&startTime=" + std::to_string(start_ms);
    if (end_ms   > 0)   params += "&endTime="   + std::to_string(end_ms);
    params += "&recvWindow=5000";

    auto resp = http_get(ep(Ep::Income), params);
    if (resp.empty()) return out;
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    auto ps = simdjson::padded_string(resp);
    if (p.parse(ps).get(doc) != simdjson::SUCCESS) return out;
    simdjson::dom::array arr;
    if (doc.get(arr) != simdjson::SUCCESS) return out;   // 错误对象而非数组

    for (auto e : arr) {
        FundingRecord r;
        std::string_view sv;
        if (e["symbol"].get(sv) == simdjson::SUCCESS) r.symbol = std::string(sv);
        if (e["income"].get(sv) == simdjson::SUCCESS) {
            try { r.income = std::stod(std::string(sv)); } catch (...) { continue; }
        }
        // time 币安有时给数字有时给字符串，两种都收
        if (e["time"].get(r.time) != simdjson::SUCCESS) {
            if (e["time"].get(sv) == simdjson::SUCCESS) {
                try { r.time = std::stoll(std::string(sv)); } catch (...) {}
            }
        }
        if (!r.symbol.empty() && r.time > 0) out.push_back(r);
    }
    return out;
}


TradingClient::RateStatus TradingClient::rate_status() const {
    auto sn = gate_->snapshot();
    return { sn.used_weight, sn.limit, sn.throttled, sn.rejected, sn.banned, sn.ban_left_ms };
}

} // namespace ccbot
