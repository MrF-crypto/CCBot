#include "net/rest_price_feed.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace ccbot {

int64_t RestPriceFeed::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

std::string RestPriceFeed::Health::summary() const {
    std::ostringstream o;
    if (!running)            { o << "未启动"; return o.str(); }
    if (last_ok_ms == 0)     { o << "尚未取到行情"; if (fail_streak) o << "（已失败" << fail_streak << "轮）"; return o.str(); }
    o << "轮询中 " << symbols << "个品种";
    if (fresh < symbols)  o << " 陈旧" << (symbols - fresh);
    if (age_ms > 2000)    o << " 上次成功" << (age_ms / 1000) << "秒前";
    if (fail_streak > 0)  o << " 连续失败" << fail_streak << "轮";
    return o.str();
}

RestPriceFeed::RestPriceFeed(FetchMap fetch_marks, FetchMap fetch_changes,
                             FetchMap fetch_fallback)
    : fetch_marks_(std::move(fetch_marks)),
      fetch_changes_(std::move(fetch_changes)),
      fetch_fallback_(std::move(fetch_fallback)) {}

RestPriceFeed::~RestPriceFeed() { stop(); }

void RestPriceFeed::on_server_msg(LogCb cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    log_cb_ = std::move(cb);
}

// 取出回调再调用，不持锁回调：调用方的日志函数可能反过来读这个对象
void RestPriceFeed::say(const std::string& m) const {
    LogCb cb;
    { std::lock_guard<std::mutex> lk(mtx_); cb = log_cb_; }
    if (cb) cb(m);
}

void RestPriceFeed::start() {
    if (running_.exchange(true)) return;
    th_ = std::thread([this] { loop(); });
}

void RestPriceFeed::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void RestPriceFeed::subscribe(const std::string& symbol) {
    const auto up = to_upper(symbol);
    std::lock_guard<std::mutex> lk(mtx_);
    subs_.insert(up);
    // 先占一个空位：界面那一列在首轮回来之前显示"—"而不是整行缺失
    if (!cache_.count(up)) cache_[up].symbol = up;
}

void RestPriceFeed::unsubscribe(const std::string& symbol) {
    const auto up = to_upper(symbol);
    std::lock_guard<std::mutex> lk(mtx_);
    subs_.erase(up);
    // ⚠ 不删 cache_：退订之后界面可能还要显示最后一次的价（比如刚停掉的 bot）。
    //   而且全市场取回是一次性的，留着不占额外请求
}

// ── 轮询主循环 ────────────────────────────────────────────────────────────────
// 没有看门狗、没有重连、没有退避升级。原因见头文件：轮询没有"连接"这个中间状态，
// 每一轮的成败是直接观测到的，不需要任何机制去推断"对面是不是已经死了"。
void RestPriceFeed::loop() {
    int64_t next_chg = 0;   // 下一次拉 24h 涨跌的时刻（0 = 立刻）

    while (running_.load()) {
        const int64_t t0 = now_ms();

        // ① 标记价：一次全取，与品种数无关
        bool ok = false;
        bool used_fallback = false;
        if (fetch_marks_) {
            auto all = fetch_marks_();

            // ② 主源连续失败到"健康灯该转红"的程度，就启用备用源。
            //
            // ⚠ 在它之前，所有价格都来自 premiumIndex 这一个端点 —— 它挂了就全挂，
            //   而"退回单品种查询"退的还是同一个端点，同一个故障下一起死。
            //   备用源走 /fapi/v1/ticker/price，不同路径、不同权重池。
            //
            // ⚠ 口径变了：备用源给的是【成交价】，不是标记价。成交价不抗插针，
            //   拿它推移动止损比标记价更容易被打掉。所以它只在主源真的不行时才上，
            //   而且切换必须报出来 —— 悄悄换掉风控赖以生存的价格口径是不可接受的。
            //   阈值与 kUnhealthyFails 一致：健康灯转红的同一刻启用备用口径
            if (all.empty() && fetch_fallback_ &&
                fail_streak_.load() >= kUnhealthyFails) {
                all = fetch_fallback_();
                used_fallback = !all.empty();
            }

            if (!all.empty()) {
                ok = true;
                const int64_t t = now_ms();
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& s : subs_) {
                    auto it = all.find(s);
                    if (it == all.end() || !(it->second > 0)) continue;
                    auto& c = cache_[s];
                    c.symbol        = s;
                    c.mark_price    = it->second;
                    c.mark_ms       = t;
                    c.ws_mark_ms    = t;   // 与 mark_ms 恒等，见头文件
                    c.from_fallback = used_fallback;
                }
            }
        }

        // 切换进/出备用源各报一次
        if (used_fallback != on_fallback_.exchange(used_fallback)) {
            if (used_fallback)
                say("⚠ 标记价拉不到，已切到【备用价格源】（/fapi/v1/ticker/price）。\n"
                    "    ⚠ 口径变了：这是【成交价】而不是标记价。标记价带指数成分、"
                    "抗单交易所插针，成交价不抗 —— 移动止损会比平时更容易被插针打掉。\n"
                    "    主源一恢复就自动切回。交易所侧那张硬止损单不受影响"
                    "（它由交易所按标记价触发）");
            else
                say("✅ 标记价已恢复，价格口径切回标记价");
        }

        if (ok) {
            last_ok_ms_.store(now_ms());
            fail_streak_.store(0);
            // 恢复只报一次
            if (fail_reported_.exchange(false))
                say("✅ 行情已恢复（REST 轮询）");
        } else {
            const int n = fail_streak_.fetch_add(1) + 1;
            // ⚠ 只在【第一次】失败时报。1 秒一轮，每轮都报就是刷屏——
            //   而"还在失败"这个事实由界面的健康指示器持续呈现，不需要日志复述
            if (!fail_reported_.exchange(true))
                say("⚠ 行情拉取失败（REST 轮询第 " + std::to_string(n) +
                    " 轮）。已有价格在 " + std::to_string(kStaleMs / 1000) +
                    " 秒后转为陈旧，届时引擎会停止开新仓；已持仓的移动止损"
                    "沿用最后一条有效线，不会因为拿不到数据而撤掉保护");
        }

        // ② 24h 涨跌：只用于显示，30 秒一轮足够（权重 40，拉太密不值）
        if (fetch_changes_ && now_ms() >= next_chg) {
            next_chg = now_ms() + kChgPollMs;
            auto all = fetch_changes_();
            if (!all.empty()) {
                const int64_t t = now_ms();
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& s : subs_) {
                    auto it = all.find(s);
                    if (it == all.end()) continue;
                    auto& c = cache_[s];
                    c.symbol  = s;
                    c.chg_24h = it->second;
                    c.chg_ms  = t;
                }
            }
        }

        // 用"距本轮开始"而不是固定 sleep：一轮取数本身要花几十到几百毫秒，
        // 固定 sleep 会让实际周期漂到 1.3~1.5 秒
        const int64_t spent = now_ms() - t0;
        const int64_t wait  = std::max<int64_t>(50, kMarkPollMs - spent);
        std::unique_lock<std::mutex> lk(cv_mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(wait),
                     [this] { return !running_.load(); });
    }
}

RestPriceFeed::Tick RestPriceFeed::get(const std::string& symbol) const {
    const auto up = to_upper(symbol);
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cache_.find(up);
    return it == cache_.end() ? Tick{} : it->second;
}

double RestPriceFeed::mark_price(const std::string& symbol) const {
    const auto t = get(symbol);
    if (!(t.mark_price > 0)) return 0.0;
    // 陈旧保护：缓存里躺着一个冻结价时看起来完全正常，而引擎会拿它推移动止损、
    // 判触线，实际行情暴跌时完全失明。宁可返回 0 让上层知道"没有价"
    if (now_ms() - t.mark_ms > kStaleMs) return 0.0;
    return t.mark_price;
}

bool RestPriceFeed::change_24h(const std::string& symbol, double& out_pct) const {
    const auto t = get(symbol);
    if (t.chg_ms == 0) return false;
    out_pct = t.chg_24h;
    return true;
}

void RestPriceFeed::set_mark_price(const std::string& symbol, double price) {
    if (!(price > 0)) return;
    const auto up = to_upper(symbol);
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mtx_);
    auto& c = cache_[up];
    c.symbol     = up;
    c.mark_price = price;
    c.mark_ms    = t;
    c.ws_mark_ms = t;
}

RestPriceFeed::Health RestPriceFeed::health() const {
    Health h;
    h.running     = running_.load();
    h.last_ok_ms  = last_ok_ms_.load();
    h.age_ms      = h.last_ok_ms ? (now_ms() - h.last_ok_ms) : 0;
    h.fail_streak = fail_streak_.load();
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mtx_);
    h.symbols = subs_.size();
    for (const auto& s : subs_) {
        auto it = cache_.find(s);
        if (it != cache_.end() && it->second.mark_price > 0 &&
            t - it->second.mark_ms <= kStaleMs)
            ++h.fresh;
    }
    return h;
}

std::vector<std::string> RestPriceFeed::stale_symbols() const {
    std::vector<std::string> out;
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& s : subs_) {
        auto it = cache_.find(s);
        if (it == cache_.end() || !(it->second.mark_price > 0) ||
            t - it->second.mark_ms > kStaleMs)
            out.push_back(s);
    }
    return out;
}

} // namespace ccbot
