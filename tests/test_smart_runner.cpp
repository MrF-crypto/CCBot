// 智能模式执行层测试：假交易所（HTTP 钩子）→ 全市场扫描 → 候选池决策落到引擎。
// 重点是"没拿到 ≠ 不合格"：交易规则/行情/K线拉不到时绝不删 bot。
#include "app/smart_runner.h"
#include "net/trading_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
                out.body = fail_klines.count(s) ? "" : klines(s);
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
        for (int i = 0; i < 3; ++i) { r.run->force(); r.run->run_once(kNow + i * 3600000LL); }
        int warns = 0, infos = 0;
        for (const auto& l : r.logs) {
            if (l.rfind("W:", 0) == 0 && has(l, "扫描失败")) ++warns;
            if (l.rfind("I:", 0) == 0 && has(l, "扫描失败")) ++infos;
        }
        check(warns == 1 && infos == 2, "连续失败 3 轮：告警 1 次，其余只记日志（实际 告警 " +
                                        std::to_string(warns) + " / 日志 " + std::to_string(infos) + "）");
        check(has(r.all_logs(), "已连续 3 轮"), "  日志里写明连续几轮");
        r.mk.fail_info = false;
        r.run->force();
        r.run->run_once(kNow + 5 * 3600000LL);
        check(has(r.all_logs(), "扫描已恢复（此前连续失败 3 轮）"), "  恢复时说一声");
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
