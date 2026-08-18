#include "net/rate_gate.h"
#include <algorithm>
#include <cctype>
#include <thread>

namespace ccbot {
namespace {

// 大小写不敏感地取一个响应头的值（币安实际返回的大小写在不同网关上不一致）
std::string header_value(const std::string& headers, const std::string& name) {
    std::string lower_h(headers.size(), '\0');
    std::transform(headers.begin(), headers.end(), lower_h.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string lower_n = name;
    std::transform(lower_n.begin(), lower_n.end(), lower_n.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    size_t p = lower_h.find(lower_n + ":");
    if (p == std::string::npos) return {};
    p += lower_n.size() + 1;
    size_t e = headers.find('\n', p);
    std::string v = headers.substr(p, e == std::string::npos ? std::string::npos : e - p);
    // 去掉首尾空白与回车
    size_t b = v.find_first_not_of(" \t\r");
    size_t f = v.find_last_not_of(" \t\r");
    return (b == std::string::npos) ? std::string{} : v.substr(b, f - b + 1);
}

int to_int(const std::string& s, int def = 0) {
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

} // namespace

void RateGate::acquire(bool is_order) {
    for (;;) {
        clock::duration wait{};
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const auto now = clock::now();

            // ① 封禁期间：所有请求（含订单）一律等待。继续发只会延长封禁
            if (ban_until_ > now) {
                wait = ban_until_ - now;
            } else {
                // ② 权重逼近上限：只限速非订单请求。
                //    权重读数超过 1 分钟视为过期（窗口已滚过），按 0 处理
                const bool weight_fresh =
                    weight_at_.time_since_epoch().count() != 0 &&
                    (now - weight_at_) < std::chrono::seconds(60);
                const double ratio = (weight_fresh && lim_.weight_per_min > 0)
                    ? (double)used_weight_ / lim_.weight_per_min : 0.0;

                if (!is_order && ratio >= lim_.hard_ratio) {
                    // 硬阈值：等到当前分钟窗口过完再说
                    auto age = now - weight_at_;
                    auto left = std::chrono::seconds(60) - age;
                    wait = (left.count() > 0) ? left : std::chrono::milliseconds(500);
                    ++throttled_;
                } else {
                    // ③ 请求间隔铺平。软阈值以上按超出比例线性放大间隔——
                    //    突发（几十个请求挤在同一秒）是本项目的真实风险
                    int gap = is_order ? lim_.order_min_gap_ms : lim_.min_gap_ms;
                    if (!is_order && ratio > lim_.soft_ratio && lim_.hard_ratio > lim_.soft_ratio) {
                        const double over = (ratio - lim_.soft_ratio) /
                                            (lim_.hard_ratio - lim_.soft_ratio);
                        gap = (int)(gap * (1.0 + 9.0 * std::min(1.0, over)));   // 最多放大到 10 倍
                        ++throttled_;
                    }
                    const auto since = now - last_send_;
                    const auto need  = std::chrono::milliseconds(gap);
                    if (last_send_.time_since_epoch().count() != 0 && since < need) {
                        wait = need - since;
                    } else {
                        last_send_ = now;
                        return;                      // 放行
                    }
                }
            }
        }
        std::this_thread::sleep_for(wait);
        // 醒来后重新判定（期间可能又被别的线程占了间隔，或封禁被延长）
    }
}

void RateGate::observe(long http_code, const std::string& headers,
                       const std::string& resp_body) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto now = clock::now();

    // 服务器回报的已用权重——这是权威值，本地不再自行累加
    int w = to_int(header_value(headers, "X-MBX-USED-WEIGHT-1M"), -1);
    if (w < 0) w = to_int(header_value(headers, "X-MBX-USED-WEIGHT"), -1);
    if (w >= 0) { used_weight_ = w; weight_at_ = now; }

    // 429=超限告警，418=已被封禁IP。两者都可能带 Retry-After（秒）
    const bool rate_err = (http_code == 429 || http_code == 418) ||
                          (resp_body.find("\"code\":-1003") != std::string::npos) ||
                          (resp_body.find("-1003") != std::string::npos &&
                           resp_body.find("Too many requests") != std::string::npos);
    if (rate_err) {
        ++rejected_;
        int retry_after = to_int(header_value(headers, "Retry-After"), 0);
        // 没给 Retry-After 时的保底退避：418 比 429 严重得多
        if (retry_after <= 0) retry_after = (http_code == 418) ? 120 : 30;
        const auto until = now + std::chrono::seconds(retry_after);
        if (until > ban_until_) ban_until_ = until;
    }
}

RateGate::Snapshot RateGate::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Snapshot s;
    s.used_weight = used_weight_;
    s.limit       = lim_.weight_per_min;
    const auto now = clock::now();
    s.banned = ban_until_ > now;
    if (s.banned)
        s.ban_left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            ban_until_ - now).count();
    s.throttled = throttled_;
    s.rejected  = rejected_;
    return s;
}

} // namespace ccbot
