// 智能模式执行层测试：假交易所（HTTP 钩子）→ 全市场扫描 → 候选池决策落到引擎。
// 重点是"没拿到 ≠ 不合格"：交易规则/行情/K线拉不到时绝不删 bot。
#include "app/smart_runner.h"
#include "net/trading_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static constexpr int64_t kDay = 24LL * 3600 * 1000;
static constexpr int64_t kNow = 1'800'000'000'000LL;

// ── 假交易所 ────────────────────────────────────────────────────────────────
struct FakeMarket {
    struct Sym {
        double  slope = 0;          // 每根K线的涨跌
        double  quote_vol = 20e6;
        int64_t onboard_ms = kNow - 100 * kDay;
        std::string status = "TRADING";
        int64_t delivery_ms = 4133404800000LL;
        // 第 140 根起价格整体乘 (1+pump)：一根K线里暴涨。
        // ⚠ 放在最后：上面几个字段有用例按位置花括号初始化
        double  pump = 0;
    };
    std::map<std::string, Sym> syms;
    bool fail_info = false, fail_t24 = false;
    std::set<std::string> fail_klines;

    // 一根K线（8 列：开盘时间、开高低收、量、收盘时间、成交额）
    static std::string kline(int64_t open_ms, int64_t span_ms, double o, double h, double l, double c,
                             double qv) {
        return "[" + std::to_string(open_ms) + ",\"" + std::to_string(o) + "\",\"" + std::to_string(h) +
               "\",\"" + std::to_string(l) + "\",\"" + std::to_string(c) + "\",\"1000\"," +
               std::to_string(open_ms + span_ms - 1) + ",\"" + std::to_string(qv) + "\"]";
    }

    // 智能裸K 的 1h（埋伏结构）：201 根在 100 附近窄幅横盘，ATR 约 1%，箱体 99.5~100.5，
    // 最后一根已收盘的收在 100.2（箱体内位置 0.7）
    //   atr_x：振幅放大倍数（4 倍时 1h ATR% 约 2.8%，超上限进不了池）
    //   low_pos：最后一根收在 99.6（箱体内位置 0.1，进不了池）
    //   drop：最后一根收到 95（跌破箱体下沿，待命刷新时移出）
    struct Amb { double atr_x = 1; bool low_pos = false; bool drop = false; };
    std::map<std::string, Amb> amb;
    std::string bars1h(const Amb& a) const {
        std::string s = "[";
        for (int i = 0; i < 201; ++i) {
            double o = (i % 2) ? 99.8 : 100.2, c = (i % 2) ? 100.2 : 99.8;
            if (i == 199 && a.low_pos) c = 99.6;
            if (i == 199 && a.drop)    c = 95;
            const double h = std::max(o, c) + 0.3 * a.atr_x, l = std::min(o, c) - 0.3 * a.atr_x;
            s += std::string(i ? "," : "") + kline(kNow - (200 - i) * 3600000LL, 3600000, o, h, l, c, 100000);
        }
        return s + "]";
    }

    // 智能裸K 的 5m（点火判定）：61 根，最后一根未收盘。已收盘的在 99.9 / 100.1 之间（ATR 约 0.6），
    // 成交额各 1000；刚收盘那根（倒数第二）由 Ign 决定
    //   fire：点火K线（开 100.2 收 101.4 高 101.5 低 100.1，成交额 qv）
    //   red5：点火前 5 根连阴（下跌中继反抽，要被否决）
    struct Ign { bool fire = false; double qv = 5000; bool red5 = false; };
    std::map<std::string, Ign> ign;
    int64_t bar_now = kNow;                // 当前（未收盘）5m 的开盘时间
    std::map<std::string, int> k5_calls;   // 每个币的 5m K线被拉了几次
    // 裸K 用例会走到下单：兜住其余所有接口（一律报错），绝不能漏到真实网络
    bool catch_all = false;
    int  orders = 0;
    std::string bars5m(const std::string& sym) const {
        const auto it = ign.find(sym);
        const Ign g = (it == ign.end()) ? Ign{} : it->second;
        std::string s = "[";
        for (int i = 0; i < 61; ++i) {
            double o = (i % 2) ? 99.9 : 100.1, c = (i % 2) ? 100.1 : 99.9, h = 100.3, l = 99.7, qv = 1000;
            if (g.red5 && i >= 54 && i <= 58) { o = 100.3; c = 99.9; h = 100.4; l = 99.8; }
            if (g.fire && i == 59) { o = 100.2; c = 101.4; h = 101.5; l = 100.1; qv = g.qv; }
            s += std::string(i ? "," : "") + kline(bar_now - (60 - i) * 300000LL, 300000, o, h, l, c, qv);
        }
        return s + "]";
    }

    std::string info() const {
        std::string b = "{\"symbols\":[";
        bool first = true;
        for (const auto& kv : syms) {
            const std::string base = kv.first.substr(0, kv.first.size() - 4);
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"baseAsset\":\"" + base +
                 "\",\"quoteAsset\":\"USDT\",\"contractType\":\"PERPETUAL\",\"status\":\"" + kv.second.status +
                 "\",\"onboardDate\":" + std::to_string(kv.second.onboard_ms) +
                 ",\"deliveryDate\":" + std::to_string(kv.second.delivery_ms) +
                 ",\"filters\":[{\"filterType\":\"PRICE_FILTER\",\"tickSize\":\"0.0001\"}]}";
            first = false;
        }
        return b + "]}";
    }
    std::string t24() const {
        std::string b = "[";
        bool first = true;
        for (const auto& kv : syms) {
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"quoteVolume\":\"" +
                 std::to_string(kv.second.quote_vol) + "\",\"priceChangePercent\":\"2.0\",\"lastPrice\":\"100\"}";
            first = false;
        }
        return b + "]";
    }
    std::string funding() const {
        std::string b = "[";
        bool first = true;
        for (const auto& kv : syms) {
            b += std::string(first ? "" : ",") + "{\"symbol\":\"" + kv.first + "\",\"lastFundingRate\":\"0.0001\"}";
            first = false;
        }
        return b + "]";
    }
    // 201 根 1h：一条带斜率的锯齿线，高低各 ±1（ATR 约 2%）
    std::string klines(const std::string& sym) const {
        const auto it = syms.find(sym);
        const double slope = (it == syms.end()) ? 0 : it->second.slope;
        const double pump  = (it == syms.end()) ? 0 : it->second.pump;
        std::string b = "[";
        double prev = 100;
        for (int i = 0; i < 201; ++i) {
            double c = 100 + slope * (i - 200) + ((i % 2) ? 0.6 : -0.6);
            if (i >= 140) c *= (1.0 + pump);
            const double h = std::max(prev, c) + 1, l = std::min(prev, c) - 1;
            b += std::string(i ? "," : "") + "[" + std::to_string(kNow - (201 - i) * 3600000LL) + ",\"" +
                 std::to_string(prev) + "\",\"" + std::to_string(h) + "\",\"" + std::to_string(l) + "\",\"" +
                 std::to_string(c) + "\",\"1000\"]";
            prev = c;
        }
        return b + "]";
    }
    void hook(TradingClient& tc) {
        tc.set_test_hook([this](const std::string&, const std::string& path, const std::string&,
                                TradingClient::FakeReply& out) {
            if (path == "/fapi/v1/exchangeInfo") { out.body = fail_info ? "" : info(); return true; }
            if (path == "/fapi/v1/ticker/24hr")  { out.body = fail_t24 ? "<html>502</html>" : t24(); return true; }
            if (path == "/fapi/v1/premiumIndex") { out.body = funding(); return true; }
            if (path.rfind("/fapi/v1/klines?symbol=", 0) == 0) {
                const auto s = path.substr(23, path.find('&') - 23);
                if (path.find("interval=5m") != std::string::npos) {
                    ++k5_calls[s];
                    out.body = fail_klines.count(s) ? "" : bars5m(s);
                    return true;
                }
                if (path.find("interval=1h") != std::string::npos && amb.count(s)) {
                    out.body = fail_klines.count(s) ? "" : bars1h(amb.at(s));
                    return true;
                }
                out.body = fail_klines.count(s) ? "" : klines(s);
                return true;
            }
            if (catch_all) {
                if (path.find("/order") != std::string::npos) ++orders;
                out.code = 400;
                out.body = "{\"code\":-1121,\"msg\":\"test\"}";
                return true;
            }
            return false;
        });
    }
};

struct Rig {
    FakeMarket mk;
    std::shared_ptr<TradingClient> tc;
    std::shared_ptr<TrendEngine> eng;
    std::unique_ptr<SmartRunner> run;
    std::vector<std::string> logs;
    std::set<std::string> subs;
    std::string path;

    explicit Rig(const std::string& p) : path(p) {
        TradingClient::Config c; c.api_key = "k"; c.api_secret = "s"; c.testnet = true;
        tc = std::make_shared<TradingClient>(c);
        mk.hook(*tc);
        EngineHost h;
        h.submit = [](std::function<void()> fn) { fn(); };
        eng = std::make_shared<TrendEngine>(tc, h);
        std::filesystem::remove(path);
        // 告警（会推 webhook）带 "W:" 前缀，方便断言去重
        run = std::make_unique<SmartRunner>(tc, eng, path,
                                            [this](const std::string& m, bool warn) {
                                                logs.push_back((warn ? "W:" : "I:") + m);
                                            });
        run->on_symbol_added   = [this](const std::string& s) { subs.insert(s); };
        run->on_symbol_removed = [this](const std::string& s) { subs.erase(s); };
    }
    // 默认按突破模式（两份名单）——下面大部分用例测的是那一套；混合模式单独一段
    smart::Config cfg(int ln = 2, int sn = 2) {
        smart::Config c;
        c.enabled = true;
        c.dir_mode = smart::DirMode::Breakout;
        c.long_n = ln; c.short_n = sn;
        c.min_score = 0;
        // 假行情的 1h ATR 约 2%（贴着默认上限）。这里测的是候选池机制，不是 ATR 门槛：放宽
        c.atr_pct_min = 0.8;
        c.atr_pct_max = 6.0;
        return c;
    }
    TrendBot bot(const std::string& sym) {
        for (const auto& b : eng->get_bots()) if (b.cfg.symbol == sym) return b;
        return {};
    }
    bool has_bot(const std::string& sym) {
        for (const auto& b : eng->get_bots()) if (b.cfg.symbol == sym) return true;
        return false;
    }
    std::string all_logs() const {
        std::string s;
        for (const auto& l : logs) s += l + "\n";
        return s;
    }
};

static void basic_market(FakeMarket& m) {
    m.syms["UPAUSDT"].slope = 0.05;
    m.syms["UPBUSDT"].slope = 0.03;
    m.syms["DNAUSDT"].slope = -0.05;
    m.syms["DNBUSDT"].slope = -0.03;
    m.syms["FLATUSDT"].slope = 0;
    m.syms["BTCUSDT"].slope = 0;
    m.syms["ETHUSDT"].slope = 0.04;
    m.syms["THINUSDT"] = {0.05, 1e6};                       // 成交额太小
    m.syms["NEWUSDT"]  = {0.05, 20e6, kNow - 3 * kDay};     // 上线太短
}

int main() {
    const std::string dir = (std::filesystem::temp_directory_path() / "ccg_smart_runner_test").string();
    std::filesystem::create_directories(dir);

    // ── 基本流程：扫描 → 两份名单各取前 2 → 自动建 bot ─────────────────────
    {
        Rig r(dir + "/a.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        check(r.run->due(kNow), "启用后立刻到点");
        const auto rep = r.run->run_once(kNow);
        check(rep.ok, "扫描成功");
        check(rep.added == 4, "加了 4 个（做多 2 + 做空 2），实际 " + std::to_string(rep.added));
        check(r.has_bot("UPAUSDT") && r.has_bot("DNAUSDT"), "  最强的上涨币和下跌币都在");
        check(!r.has_bot("BTCUSDT") && !r.has_bot("ETHUSDT"), "  BTC/ETH 内置排除");
        check(!r.has_bot("THINUSDT") && !r.has_bot("NEWUSDT"), "  成交额不足 / 上线不满 14 天的不加");
        const auto b = r.bot("UPAUSDT");
        check(b.is_auto && !b.armed && b.cfg.interval == "1h" &&
              b.cfg.rule.direction == trend::Direction::Both && b.cfg.use_disaster_stop &&
              b.cfg.rule.lock_atr && b.cfg.rule.accel_exit,
              "  自动 bot：未上膛、1h 海龟、多空、硬止损、出场优化");
        check(b.smart_list == 'L' && b.smart_rank == 1 && b.smart_score > 0, "  名单信息写进了 bot");
        check(r.subs.count("UPAUSDT") == 1, "  通知应用层订阅价格");
        check(!r.run->due(kNow + 60000), "扫完之后：下个整点前不再到点");
        check(r.run->due(kNow / 3600000 * 3600000 + 3600000 + 61000), "  下个整点后 1 分钟到点");
        check(has(r.all_logs(), "新增："), "日志：新增一行");
    }

    // ── 整轮拉取失败：一个 bot 都不动 ─────────────────────────────────────────
    {
        Rig r(dir + "/b.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        const auto n0 = r.eng->get_bots().size();
        r.mk.fail_info = true;
        r.run->force();
        auto rep = r.run->run_once(kNow + 3600000 * 30LL);
        check(!rep.ok && r.eng->get_bots().size() == n0, "交易规则拉不到：整轮作废，bot 一个不动");
        check(has(rep.summary, "失败"), "  日志说明失败原因");
        r.mk.fail_info = false;
        r.mk.fail_t24 = true;
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 * 31LL);
        check(!rep.ok && r.eng->get_bots().size() == n0, "24h 行情拉不到（返回 HTML）：同样不动");

        // 字段解析出问题（比如成交额全成了很小的数）：全市场 0 个合格，当数据异常
        r.mk.fail_t24 = false;
        for (auto& kv : r.mk.syms) kv.second.quote_vol = 1;
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 * 32LL);
        check(!rep.ok && has(rep.summary, "数据异常") && r.eng->get_bots().size() == n0,
              "全市场合格为 0：疑似数据异常，整轮作废、bot 一个不动");
        for (auto& kv : r.mk.syms) kv.second.quote_vol = 20e6;
    }

    // ── 单个币的K线拉不到：它是"未知"，不淘汰；成交额掉了：淘汰 ──────────────
    {
        Rig r(dir + "/c.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        r.mk.fail_klines.insert("UPAUSDT");
        for (int i = 1; i <= 3; ++i) {
            r.run->force();
            r.run->run_once(kNow + i * 30 * 3600000LL);
        }
        check(r.has_bot("UPAUSDT"), "K线连续 3 轮拉不到：仍然保留（没拿到 ≠ 不合格）");
        r.mk.fail_klines.clear();

        r.mk.syms["DNAUSDT"].quote_vol = 1e6;
        r.run->force();
        r.run->run_once(kNow + 5 * 30 * 3600000LL);
        check(!r.has_bot("DNAUSDT"), "成交额跌破门槛（1M < 4M）：空仓立即移除");
        check(r.subs.count("DNAUSDT") == 0, "  通知应用层退订");
        check(has(r.all_logs(), "移出：DNAUSDT"), "  日志写明移出原因");
    }

    // ── 手动 bot 占着的品种不加；方向改成只做多，空仓的自动 bot 换参数 ─────────
    {
        Rig r(dir + "/d.json");
        basic_market(r.mk);
        TrendConfig manual;
        manual.symbol = "UPAUSDT";
        r.eng->add_bot(manual);
        auto c = r.cfg();
        r.run->set_config(c);
        r.run->run_once(kNow);
        int autos_on_upa = 0;
        for (const auto& b : r.eng->get_bots())
            if (b.cfg.symbol == "UPAUSDT" && b.is_auto) ++autos_on_upa;
        check(autos_on_upa == 0, "手动 bot 占着的品种：智能模式不加");

        c.dir_short = false;
        r.run->set_config(c);
        check(r.run->due(kNow + 60000), "改了配置：立刻重扫");
        r.run->run_once(kNow + 60000);
        bool all_long = true;
        int autos = 0;
        for (const auto& b : r.eng->get_bots())
            if (b.is_auto) { ++autos; all_long &= (b.cfg.rule.direction == trend::Direction::LongOnly); }
        check(autos > 0 && all_long, "只勾做多：空仓的自动 bot 都换成仅多");
    }

    // ── 关闭智能模式：空仓的自动 bot 移除，手动 bot 不动 ──────────────────────
    {
        Rig r(dir + "/e.json");
        basic_market(r.mk);
        TrendConfig manual;
        manual.symbol = "FLATUSDT";
        r.eng->add_bot(manual);
        auto c = r.cfg();
        r.run->set_config(c);
        r.run->run_once(kNow);
        c.enabled = false;
        r.run->set_config(c);
        check(!r.run->due(kNow + 3600000 * 5LL), "关闭后不再到点");
        r.run->housekeep(kNow);
        int autos = 0;
        for (const auto& b : r.eng->get_bots()) if (b.is_auto) ++autos;
        check(autos == 0 && r.has_bot("FLATUSDT"), "关闭：空仓自动 bot 全部移除，手动 bot 保留");
    }

    // ── 混合模式：强势仅多 / 弱势仅空 / 中间多空 / 过热仅空 ─────────────────────
    {
        Rig r(dir + "/m.json");
        auto& m = r.mk.syms;
        m["UPAUSDT"].slope  = 0.05;    // 最强（除了暴涨币）⇒ 强势
        m["UPBUSDT"].slope  = 0.02;    // 偏强但没进前 30% ⇒ 中间
        m["DNAUSDT"].slope  = -0.05;   // 弱 ⇒ 弱势
        m["DNBUSDT"].slope  = -0.03;
        m["FLATUSDT"].slope = 0;       // 横着 ⇒ 中间
        m["FLAT2USDT"].slope = 0;
        m["PUMPUSDT"].pump  = 0.9;     // 一根K线涨 90% ⇒ 过热
        m["BTCUSDT"].slope  = 0;
        auto c = r.cfg();
        c.dir_mode = smart::DirMode::Mixed;
        c.weak_vol_ratio = 1.0;
        r.run->set_config(c);
        const auto rep = r.run->run_once(kNow);
        check(rep.ok && rep.groups.size() == 4, "混合模式：名单分四组（强 / 弱 / 中 / 过热）");
        auto dir_of = [&](const std::string& s) { return r.bot(s).cfg.rule.direction; };
        auto seg_of = [&](const std::string& s) { return r.bot(s).smart_list; };
        check(seg_of("UPAUSDT") == smart::kSegStrong && dir_of("UPAUSDT") == trend::Direction::LongOnly,
              "  强势币：强势段、仅多");
        check(seg_of("DNAUSDT") == smart::kSegWeak && dir_of("DNAUSDT") == trend::Direction::ShortOnly,
              "  弱势币：弱势段、仅空");
        check(seg_of("FLATUSDT") == smart::kSegMid && dir_of("FLATUSDT") == trend::Direction::Both,
              "  横盘币：中间段、多空（谁突破做谁）");
        check(seg_of("PUMPUSDT") == smart::kSegHot && dir_of("PUMPUSDT") == trend::Direction::ShortOnly,
              "  单日暴涨 90% 的币：过热段、仅空（不因强弱分高就去做多）");
        check(r.bot("PUMPUSDT").cfg.rule.breakout_vol_ratio == 0,
              "  过热段不要求量比（暴涨后的回落多是缩量阴跌）");
        check(r.bot("DNAUSDT").cfg.rule.breakout_vol_ratio == 1.0,
              "  弱势段用自己的量比设置（这里设成 1.0）");
        check(r.bot("FLATUSDT").cfg.rule.breakout_vol_ratio == c.tmpl.rule.breakout_vol_ratio &&
              c.tmpl.rule.breakout_vol_ratio > 0,
              "  中间段照常要量比（横盘币缩量跌破多半是洗盘）");
        check(has(rep.summary, "强势") && has(rep.summary, "过热"), "  汇总里按段列出");

        // 数据记录层：扫描快照与候选池事件
        const auto jdir = std::filesystem::path(dir) / "journal_m";
        std::filesystem::remove_all(jdir);
        Rig rj(dir + "/mj.json");
        rj.mk.syms = m;
        rj.run->set_journal(std::make_shared<Journal>(jdir.string()));
        rj.run->set_config(c);
        rj.run->run_once(kNow);
        bool scan_file = false;
        for (const auto& e : std::filesystem::directory_iterator(jdir))
            if (e.path().filename().string().rfind("scan-", 0) == 0) scan_file = true;
        check(scan_file && std::filesystem::exists(jdir / "pool.csv"),
              "数据记录层：每轮写扫描快照，加入的币记进 pool.csv");

        // 只勾做多：弱势、过热不加；中间段只做多
        Rig r2(dir + "/m2.json");
        r2.mk.syms = m;
        auto c2 = c; c2.dir_short = false;
        r2.run->set_config(c2);
        r2.run->run_once(kNow);
        check(!r2.has_bot("DNAUSDT") && !r2.has_bot("PUMPUSDT"), "只勾做多：弱势段、过热段不加");
        check(r2.bot("FLATUSDT").cfg.rule.direction == trend::Direction::LongOnly,
              "  中间段只做多（与勾选取交集）");
    }

    // ── 扫描连续失败：只有第一轮告警（推 webhook），之后只记日志，恢复时说一声 ──
    {
        Rig r(dir + "/g.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.mk.fail_info = true;
        r.run->run_once(kNow);
        check(!r.run->due(kNow + 4 * 60000) && r.run->due(kNow + 5 * 60000),
              "扫描失败：5 分钟后重试（不空等到下个整点）");
        // 5 分钟一试：连续 12 轮（一小时）
        for (int i = 1; i < 12; ++i) r.run->run_once(kNow + i * 5 * 60000LL);
        int warns = 0, infos = 0;
        for (const auto& l : r.logs) {
            if (l.rfind("W:", 0) == 0 && has(l, "扫描失败")) ++warns;
            if (l.rfind("I:", 0) == 0 && has(l, "扫描失败")) ++infos;
        }
        check(warns == 1 && infos == 1, "连续失败 12 轮：第一轮告警，之后每小时只记一行（实际 告警 " +
                                        std::to_string(warns) + " / 日志 " + std::to_string(infos) + "）");
        check(has(r.all_logs(), "已连续 12 轮") && has(r.all_logs(), "5 分钟后重试"), "  日志里写明连续几轮、何时重试");
        r.mk.fail_info = false;
        r.run->run_once(kNow + 12 * 5 * 60000LL);
        check(has(r.all_logs(), "扫描已恢复（此前连续失败 12 轮）"), "  恢复时说一声");
    }

    // ── 作废的运行器：正在跑的那轮不落地、不写文件 ────────────────────────────
    {
        Rig r(dir + "/h.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->cancel();
        r.run->run_once(kNow);
        int autos = 0;
        for (const auto& b : r.eng->get_bots()) if (b.is_auto) ++autos;
        check(autos == 0, "作废后：扫描结果不落地（断线重连时旧运行器不往旧引擎里加 bot）");
        r.run->housekeep(kNow);
        check(!std::filesystem::exists(r.path), "  也不写状态文件（不和新运行器抢）");
    }

    // ── 当日已实现：状态文件里没有就用成交历史补 ───────────────────────────────
    {
        const auto now = std::chrono::system_clock::now();
        const int64_t today = TrendEngine::utc_day(now);
        std::vector<TrendTrade> tr(3);
        tr[0].close_time = now;                         tr[0].pnl = -10; tr[0].entry_price = 100; tr[0].exit_price = 95; tr[0].qty = 2;
        tr[1].close_time = now;                         tr[1].pnl = 4;   tr[1].entry_price = 50;  tr[1].exit_price = 52; tr[1].qty = 1;
        tr[2].close_time = now - std::chrono::hours(48); tr[2].pnl = -99; tr[2].entry_price = 1;  tr[2].exit_price = 1;  tr[2].qty = 1;
        const double want = (-10 - (100 + 95) * 2 * TrendEngine::kEstFeeRate) +
                            (4 - (50 + 52) * 1 * TrendEngine::kEstFeeRate);
        check(std::fabs(sum_day_realized(tr, today) - want) < 1e-9,
              "按成交历史算今天的已实现：只算今天的，扣估算手续费（与引擎同口径）");

        Rig r(dir + "/i.json");
        r.run->seed_day_from_history(tr);
        check(std::fabs(r.eng->day_pnl().realized - want) < 1e-9, "状态里没有今天的数：用历史补上");
        r.eng->restore_day_pnl({today, -1});
        r.run->seed_day_from_history(tr);
        check(r.eng->day_pnl().realized == -1, "状态里已有今天的数：以状态为准，不覆盖");
    }

    // ── 智能裸K（埋伏点火）：建池 → 5m 收盘判定点火 → 开仓；待命刷新移出；每天重建 ──
    {
        Rig r(dir + "/bare.json");
        r.mk.catch_all = true;
        for (const char* s : {"AAAUSDT", "BBBUSDT", "CCCUSDT", "HOTUSDT", "LOWUSDT", "PAXGUSDT"}) {
            r.mk.syms[s].slope = 0;
            r.mk.amb[s] = {};
        }
        r.mk.amb["HOTUSDT"].atr_x   = 4;      // 1h ATR% 超 2%
        r.mk.amb["LOWUSDT"].low_pos = true;   // 收在箱体下部
        r.mk.syms["THINUSDT"] = {0, 2e6};     // 成交额 200 万 < 裸K 的 300 万
        r.mk.amb["THINUSDT"] = {};
        r.mk.syms["BTCUSDT"].slope = 0;
        const std::string jdir = dir + "/journal_bare";
        std::filesystem::remove_all(jdir);
        r.run->set_journal(std::make_shared<Journal>(jdir));
        std::set<std::string> watched;
        r.run->on_watch = [&](const std::string& s, bool on) {
            if (on) watched.insert(s); else watched.erase(s);
        };
        const SmartRunner::PriceFn price_of = [](const std::string&) { return 101.4; };
        auto c = r.cfg();
        c.strategy = smart::Strategy::BareK;
        r.run->set_config(c);

        const int64_t t0 = kNow + 60000;
        auto rep = r.run->run_once(t0);
        check(rep.ok && rep.groups.size() == 1 && rep.groups[0].rows.size() == 3,
              "建埋伏池：3 个进池（ATR% 超限、箱体下部、成交额不足、黄金代币、BTC 都不进）");
        check(watched == std::set<std::string>{"AAAUSDT", "BBBUSDT", "CCCUSDT"} && r.eng->get_bots().empty(),
              "  埋伏池只订阅价格、不建 bot");
        check(has(rep.summary, "建埋伏池") && has(rep.summary, "入池 3"), "  一行汇总：建池");
        if (!rep.groups.empty() && !rep.groups[0].rows.empty()) {
            const auto& row = rep.groups[0].rows[0];
            check(row.status == "埋伏中" && row.box_pos > 0.6 && row.kappa > 0, "  名单：状态、κ、箱体内位置");
        }
        check(!r.run->due(t0 + 60000) && r.run->due(kNow / 3600000 * 3600000 + 3600000 + 60000),
              "  下一次：下一根 1h 收盘后 1 分钟");

        // 新的 5m 收盘：AAA 点火，BBB 点火但前 5 根连阴（否决），CCC 没动静
        r.mk.bar_now = kNow + 300000;
        r.mk.ign["AAAUSDT"].fire = true;
        r.mk.ign["BBBUSDT"].fire = true;
        r.mk.ign["BBBUSDT"].red5 = true;
        r.run->bare_tick(kNow + 300000 + 1000, price_of);
        check(r.mk.k5_calls["AAAUSDT"] == 0, "收盘后不到 3 秒：先不判（交易所K线可能还没定稿）");
        r.run->bare_tick(kNow + 300000 + 3500, price_of);
        check(r.mk.k5_calls["AAAUSDT"] == 1 && r.mk.k5_calls["CCCUSDT"] == 1, "收盘后 3 秒：池里每个币拉一次 5m");
        check(r.has_bot("AAAUSDT") && !r.has_bot("BBBUSDT") && !r.has_bot("CCCUSDT"),
              "  AAA 点火开仓；BBB 前 5 根连阴被否决；CCC 没点火");
        {
            const auto b = r.bot("AAAUSDT");
            check(b.is_auto && b.smart_list == smart::kSegBare && b.cfg.interval == "1h" &&
                      b.cfg.rule.direction == trend::Direction::LongOnly && b.retiring &&
                      std::fabs(b.cfg.rule.atr_mult - 3.0) < 1e-9,
                  "  bot：1h、仅多、3×ATR 追踪、待移出（自己不开仓，只认点火）");
            check(b.smart_trigger == 'I' && r.mk.orders >= 1, "  按点火直接下单（触发方式记为点火）");
        }
        check(has(r.all_logs(), "AAAUSDT 点火") && has(r.all_logs(), "止损 @100.1"),
              "  日志：点火、止损在点火K线最低点");
        r.run->bare_tick(kNow + 300000 + 6500, price_of);
        check(r.mk.k5_calls["CCCUSDT"] == 1, "同一根 5m 每个币只判一次");

        // 下单在测试里被拒，bot 一直空仓：下一拍删掉，币留在池里
        r.run->bare_tick(kNow + 300000 + 9500, price_of);
        check(!r.has_bot("AAAUSDT") && r.run->watching("AAAUSDT"), "没开成的空仓 bot 删掉，币留在埋伏池");
        {
            const auto rows = r.run->bare_rows(kNow + 300000 + 9500, price_of);
            bool ign_today = false;
            for (const auto& row : rows) if (row.symbol == "AAAUSDT") ign_today = row.status == "今日已点火";
            check(ign_today, "  名单状态：今日已点火");
        }

        // 记录层：否决的写进 filter（量比够了才记），建池快照写进 ambush
        bool amb_file = false, veto = false;
        for (const auto& e : std::filesystem::directory_iterator(jdir)) {
            const auto n = e.path().filename().string();
            if (n.rfind("ambush-", 0) == 0) amb_file = true;
            if (n.rfind("filter-", 0) == 0) {
                std::ifstream f(e.path());
                std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                veto = has(all, "BBBUSDT") && has(all, "点火否决") && has(all, "连阴") && !has(all, "CCCUSDT");
            }
        }
        check(amb_file, "记录层：ambush-日期.csv 写了埋伏池快照");
        check(veto, "  量比达标却被否决的写进 filter（没动静的不记）");

        // 待命刷新：CCC 跌破箱体下沿 → 移出；汇总带上这一小时的点火判定
        r.mk.amb["CCCUSDT"].drop = true;
        rep = r.run->run_once(kNow / 3600000 * 3600000 + 3600000 + 60000);
        check(rep.ok && has(rep.summary, "待命刷新") && has(rep.summary, "埋伏池 2") &&
                  !r.run->watching("CCCUSDT") && !watched.count("CCCUSDT"),
              "待命刷新：跌破箱体下沿的移出、退订");
        check(has(rep.summary, "点火判定 3 次") && has(rep.summary, "被否决 1") && has(rep.summary, "点火 1") &&
                  has(rep.summary, "开仓 1"),
              "  汇总：点火判定 / 被否决 / 点火 / 开仓，实际：" + rep.summary);
        check(has(r.all_logs(), "CCCUSDT（跌破箱体下沿"), "  日志写明移出原因");

        // 第二天 UTC 0 点后：清空重建
        r.mk.amb["CCCUSDT"].drop = false;
        const int64_t next_day = (kNow / 86400000 + 1) * 86400000 + 60000;
        rep = r.run->run_once(next_day);
        check(rep.ok && has(rep.summary, "建埋伏池") && r.run->watching("CCCUSDT"),
              "每天 UTC 0 点（北京 08:00）后清空重建：CCC 又回来了");

        // 切回海龟：埋伏池清空、退订
        c.strategy = smart::Strategy::Turtle;
        r.run->set_config(c);
        r.run->run_once(next_day + 3600000);
        check(!r.run->watching("AAAUSDT") && watched.empty(), "切回海龟：埋伏池清空、退订");
        std::filesystem::remove_all(jdir);
    }

    // ── 只跑一套：海龟还有持仓时裸K 不启动；空仓的海龟 bot 与候选池立刻清掉 ──────
    {
        Rig r(dir + "/switch.json");
        r.mk.catch_all = true;
        basic_market(r.mk);
        r.mk.amb["FLATUSDT"] = {};   // 裸K 启动后要有一个能进埋伏池的
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);                               // 海龟：建了 4 个自动 bot（空仓）
        // 再放一笔持仓中的海龟自动 bot
        TrendBot held;
        held.cfg = smart::make_bot_config(r.cfg(), "HELDUSDT", trend::Direction::Both, smart::kSegMid);
        held.is_auto = true;
        held.smart_list = smart::kSegMid;
        held.smart_rank = 3;
        held.st.pos = trend::Pos::Long;
        held.st.entry_price = 100; held.st.peak = 100; held.st.stop = 94;
        held.qty = 1;
        const auto held_id = r.eng->restore_bot(held);
        check(!held_id.empty() && smart_holdings(r.eng->get_bots(), smart::Strategy::Turtle) == 1 &&
                  smart_holdings(r.eng->get_bots(), smart::Strategy::BareK) == 0,
              "smart_holdings：按策略数持仓（海龟 1 笔、裸K 0）");

        std::set<std::string> watched;
        r.run->on_watch = [&](const std::string& s, bool on) {
            if (on) watched.insert(s); else watched.erase(s);
        };
        auto c = r.cfg();
        c.strategy = smart::Strategy::BareK;
        r.run->set_config(c);
        auto rep = r.run->run_once(kNow + 3600000);
        check(!rep.ok && has(rep.summary, "等海龟持仓走完") && watched.empty(),
              "切到裸K 时海龟还有持仓：裸K 不扫描、不建种子池");
        check(r.eng->get_bots().size() == 1 && r.has_bot("HELDUSDT"),
              "  空仓的海龟 bot 立刻删掉，持仓的留着");
        check(r.bot("HELDUSDT").retiring && r.bot("HELDUSDT").smart_rank == 0,
              "  持仓的标待移出：管到平仓、不再开新仓（名次清零，界面显示待移出）");
        check(r.run->watching("AAAUSDT") == false && has(r.all_logs(), "还有 1 笔海龟持仓"),
              "  日志说明为什么没启动");
        const auto warns = std::count_if(r.logs.begin(), r.logs.end(),
                                         [](const std::string& l) { return has(l, "W:智能模式：还有"); });
        r.run->force();
        r.run->run_once(kNow + 3600000 + 60000);
        const auto warns2 = std::count_if(r.logs.begin(), r.logs.end(),
                                          [](const std::string& l) { return has(l, "智能模式：还有"); });
        check(warns == 1 && warns2 == 1, "  同样的情况不重复提示（告警只一次）");
        r.run->bare_tick(kNow + 3600000 + 65000, [](const std::string&) { return 0.0; });
        check(r.eng->get_bots().size() == 1, "  实时部分同样不动");

        // 这笔走完（这里直接删掉模拟手动平仓后移除）：裸K 启动
        r.eng->remove_bot(held_id);
        r.run->force();
        rep = r.run->run_once(kNow + 3600000 + 120000);
        check(rep.ok && !watched.empty() && has(r.all_logs(), "海龟持仓已全部走完，裸K开始运行"),
              "海龟持仓走完：裸K 开始运行");
    }

    // ── 池状态与当日盈亏落盘 ─────────────────────────────────────────────────
    {
        Rig r(dir + "/f.json");
        basic_market(r.mk);
        r.run->set_config(r.cfg());
        r.run->run_once(kNow);
        r.eng->restore_day_pnl({TrendEngine::utc_day(std::chrono::system_clock::now()), -12.5});
        r.run->housekeep(kNow);

        auto eng2 = std::make_shared<TrendEngine>(r.tc, EngineHost{});
        SmartRunner r2(r.tc, eng2, r.path, nullptr);
        r2.load();
        check(std::fabs(eng2->day_pnl().realized + 12.5) < 1e-9, "当日盈亏跨重启接得上（亏损限额不因重启清零）");
    }

    std::filesystem::remove_all(dir);
    if (g_fail == 0) {
        std::printf("OK: 智能模式执行层测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
