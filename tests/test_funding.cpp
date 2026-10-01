// 资金费账本的单元测试。
//
// 这是【账本】不是风控——它唯一的职责就是把数记对。所以测试全部围绕
// "记账正确性"：去重、口径分离、落盘往返、以及把利息换算成回本价的公式。
#include "core/funding_ledger.h"
#include "core/fee_ledger.h"
#include "net/trading_client.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}
static void check_near(double got, double want, double tol, const std::string& what) {
    bool ok = std::fabs(got - want) <= tol;
    std::printf("%s  %s (期望 %.4f，实际 %.4f)\n", ok ? "[ OK ]" : "[FAIL]",
                what.c_str(), want, got);
    if (!ok) ++g_fail;
}

int main() {
    std::printf("── 用例1：累计与去重 ──\n");
    {
        FundingLedger L;
        L.apply("BTCUSDT", -1.25, 1000);
        L.apply("BTCUSDT", -1.50, 2000);
        check_near(L.get("BTCUSDT").total, -2.75, 1e-9, "两笔累计");

        // 补历史按 7 天窗口分页，窗口边界会重叠 —— 同一条流水必然被拉两次。
        // 同品种一次结算只有一条记录，所以"时间戳不大于游标"即重复
        L.apply("BTCUSDT", -1.50, 2000);
        check_near(L.get("BTCUSDT").total, -2.75, 1e-9, "重复时间戳不重复计数");
        L.apply("BTCUSDT", -9.99, 1500);
        check_near(L.get("BTCUSDT").total, -2.75, 1e-9, "游标之前的旧记录被丢弃");

        check(L.cursor("BTCUSDT") == 2000, "游标停在最后一条");
        L.apply("BTCUSDT", -0.25, 3000);
        check_near(L.get("BTCUSDT").total, -3.00, 1e-9, "游标之后的新记录正常入账");
    }

    std::printf("\n── 用例2：两个口径互不干扰 ──\n");
    {
        FundingLedger L;
        L.apply("ETHUSDT", -5.0, 1000);
        L.apply("ETHUSDT", -3.0, 2000);
        check_near(L.get("ETHUSDT").total,      -8.0, 1e-9, "总累计 -8");
        check_near(L.get("ETHUSDT").since_open, -8.0, 1e-9, "本轮累计 -8");

        L.reset_position("ETHUSDT");            // 仓位归零
        check_near(L.get("ETHUSDT").total,      -8.0, 1e-9, "平仓后总累计【保留】");
        check_near(L.get("ETHUSDT").since_open,  0.0, 1e-9, "平仓后本轮累计清零");

        L.apply("ETHUSDT", -2.0, 3000);
        check_near(L.get("ETHUSDT").total,      -10.0, 1e-9, "新一轮继续累加到总数");
        check_near(L.get("ETHUSDT").since_open,  -2.0, 1e-9, "本轮只算新一轮");
    }

    std::printf("\n── 用例3：收资金费的情况（费率为负）──\n");
    {
        FundingLedger L;
        L.apply("XRPUSDT", 1.20, 1000);         // 正数 = 你收到钱
        check_near(L.get("XRPUSDT").since_open, 1.20, 1e-9, "正收入正常记账");
        check_near(FundingLedger::annualized_pct(-0.0001), -10.95, 0.01,
                   "负费率年化为负（多头在收钱）");
    }

    std::printf("\n── 用例4：年化换算 ──\n");
    {
        // 每 8 小时一次 = 每天 3 次
        check_near(FundingLedger::annualized_pct(0.0001), 10.95, 0.01, "0.01%/8h → 年化 10.95%");
        check_near(FundingLedger::annualized_pct(0.0005), 54.75, 0.01, "0.05%/8h → 年化 54.75%");
        check_near(FundingLedger::annualized_pct(0.0),     0.0,  1e-9, "零费率");
    }

    std::printf("\n── 用例5：资金费把回本价推高多少 ──\n");
    {
        // 0.015 BTC @ 60000 = 900U 名义；已付 50U 资金费 = 名义的 5.556%
        double be = FundingLedger::effective_breakeven(60000.0, 0.015, -50.0, true);
        check_near(be, 60000.0 + 50.0 / 0.015, 0.01, "多头：均价 + 已付/持仓量");
        check_near(be / 60000.0 - 1.0, 50.0 / 900.0, 1e-6,
                   "回本涨幅增量 = 已付资金费 / 持仓成本");

        // 空头方向相反（本框架目前只做多，但公式要对）
        double bs = FundingLedger::effective_breakeven(60000.0, 0.015, -50.0, false);
        check_near(bs, 60000.0 - 50.0 / 0.015, 0.01, "空头：均价 − 已付/持仓量");

        // 没付过资金费时不该动回本价
        check_near(FundingLedger::effective_breakeven(60000.0, 0.015, 0.0, true),
                   60000.0, 1e-9, "未付资金费时回本价不变");
        // 非法输入不该产生 inf/nan
        check_near(FundingLedger::effective_breakeven(60000.0, 0.0, -50.0, true),
                   60000.0, 1e-9, "持仓量为0时安全返回均价");
    }

    std::printf("\n── 用例6：落盘往返 ──\n");
    {
        const std::string path = "test_funding.json";
        {
            FundingLedger L;
            L.apply("BTCUSDT", -12.5, 5000);
            L.apply("ETHUSDT",  -3.25, 6000);
            L.reset_position("ETHUSDT");
            L.set_rate("BTCUSDT", 0.0001, 7000, 6500);
            check(L.save(path), "写盘");
        }
        {
            FundingLedger L;
            check(L.load(path), "读盘");
            check_near(L.get("BTCUSDT").total,      -12.5, 1e-9, "BTC 总累计");
            check_near(L.get("BTCUSDT").since_open, -12.5, 1e-9, "BTC 本轮累计");
            check(L.cursor("BTCUSDT") == 5000, "BTC 游标（决定下次从哪拉，错了会重复计数）");
            check_near(L.get("ETHUSDT").total,      -3.25, 1e-9, "ETH 总累计保留");
            check_near(L.get("ETHUSDT").since_open,  0.0,  1e-9, "ETH 本轮累计（平仓后）");
            // 费率是易变的实时量，不落盘 —— 重启后等下一次拉取即可
            check(L.get("BTCUSDT").rate_ms == 0, "费率不落盘（重启后重新拉）");
        }
        std::remove(path.c_str());
    }

    std::printf("\n── 用例7：未知品种返回零值而非崩溃 ──\n");
    {
        FundingLedger L;
        auto e = L.get("NOSUCHUSDT");
        check(e.total == 0 && e.since_open == 0 && e.cursor_ms == 0, "未知品种是干净的零值");
        check(L.symbols().empty(), "只读查询不会创建条目");
    }

    // ═══ 手续费账本 ══════════════════════════════════════════════════════════
    std::printf("\n── 手续费1：同一毫秒多条流水，按 tranId 去重 ──\n");
    {
        // 一笔市价单分多次成交，会在同一毫秒产生多条手续费。资金费那种
        // "时间戳不大于游标就是重复"的去重会把后面几条当重复丢掉
        FeeLedger L;
        L.open_span("BTCUSDT", 0);
        check(L.apply("BTCUSDT", 11, "USDT", -0.10, 1000), "同一毫秒第 1 条计入");
        check(L.apply("BTCUSDT", 12, "USDT", -0.20, 1000), "⚠ 同一毫秒第 2 条也要计入（tranId 不同）");
        check(!L.apply("BTCUSDT", 11, "USDT", -0.10, 1000), "  同一条再喂一次不重复计");
        check(!L.apply("BTCUSDT", 9, "USDT", -5.0, 500), "  比游标早的不计（已处理过的时段）");
        check_near(L.get("BTCUSDT").total, -0.30, 1e-12, "  合计 = −0.30");
        check(L.get("BTCUSDT").count == 2, "  计入 2 条");
    }

    std::printf("\n── 手续费2：只计 bot 存在的时间段 ──\n");
    {
        FeeLedger L;
        L.add_closed_span("ETHUSDT", 1000, 2000);
        check(!L.apply("ETHUSDT", 1, "USDT", -1.0, 500),  "时间段之前的不计");
        check(L.apply("ETHUSDT", 2, "USDT", -1.0, 1500),  "时间段内的计入");
        check(!L.apply("ETHUSDT", 3, "USDT", -1.0, 2500), "时间段之后（bot 已删）不计");
        check(L.get("ETHUSDT").cursor_ms == 2500, "  不计入的也要推进游标（已经看过了）");
        check(!L.apply("ETHUSDT", 4, "BNB", -1.0, 3000) && L.get("ETHUSDT").skipped_asset == 0,
              "  时间段外的非 USDT 流水既不计入也不算作跳过");
        L.add_closed_span("ETHUSDT", 2800, 4000);
        check(!L.apply("ETHUSDT", 5, "BNB", -0.5, 3500), "非 USDT 计价的不计入合计");
        check(L.get("ETHUSDT").skipped_asset == 1, "  但单独计数，界面上要说出来");
        check_near(L.total(), -1.0, 1e-12, "全账本合计只有那一条");
    }

    std::printf("\n── 手续费3：按当前 bot 列表开/关时间段 ──\n");
    {
        FeeLedger L;
        check(L.reconcile_active({"SOLUSDT"}, 1000), "新出现的品种开一段");
        check(!L.reconcile_active({"SOLUSDT"}, 2000),
              "⚠ 品种一直在列表里就不动 —— 改策略参数（删了再建）不能把一段切成两段");
        check(L.reconcile_active({}, 3000), "从列表里消失：关掉");
        auto e = L.get("SOLUSDT");
        check(e.spans.size() == 1 && e.spans[0].from_ms == 1000 && e.spans[0].to_ms == 3000,
              "  时间段 = [1000, 3000]");
        check(L.reconcile_active({"SOLUSDT"}, 5000), "删了再加：开第二段");
        e = L.get("SOLUSDT");
        check(e.spans.size() == 2 && e.spans[1].to_ms == 0, "  两段都在，第二段仍开着");
        check(L.covers("SOLUSDT", 2000) && !L.covers("SOLUSDT", 4000) && L.covers("SOLUSDT", 9999),
              "  两段之间的空档不计，两段里面都计");
    }

    std::printf("\n── 手续费4：同步计划 ──\n");
    {
        const int64_t now = 100LL * 24 * 3600 * 1000;
        FeeLedger L;
        L.open_span("A", now - 3600 * 1000);
        auto jobs = L.plan(now);
        check(jobs.size() == 1 && jobs[0].from_ms == now - 3600 * 1000 && jobs[0].to_ms == now,
              "bot 还在：从时间段起点同步到现在");
        L.mark_synced("A", now - 10 * 60 * 1000);
        jobs = L.plan(now);
        check(jobs.size() == 1 && jobs[0].from_ms == now - 10 * 60 * 1000 - FeeLedger::kOverlapMs,
              "  同步过之后从上次同步点往回重叠 1 分钟（重复的靠 tranId 去重）");

        FeeLedger D;
        const int64_t closed = now - 60 * 1000;
        D.add_closed_span("B", now - 3600 * 1000, closed);
        check(D.plan(now).size() == 1,
              "⚠ bot 刚删：还要再同步，否则删除前最后那笔平仓的手续费会漏");
        D.mark_synced("B", closed + FeeLedger::kGraceMs);
        check(D.plan(now + FeeLedger::kGraceMs).empty(),
              "  同步过删除时刻 + 宽限之后就收手，不再为它发请求");

        FeeLedger O;
        O.open_span("C", 1000);   // 远早于交易所能查到的范围
        jobs = O.plan(now);
        check(jobs.size() == 1 && jobs[0].from_ms == now - FeeLedger::kMaxLookbackMs,
              "起点早于交易所能查的范围时，截到能查的最早时刻");
    }

    std::printf("\n── 手续费5：落盘往返 ──\n");
    {
        FeeLedger L;
        L.open_span("X", 1000);
        L.add_closed_span("Y", 10, 20);
        L.apply("X", 7, "USDT", -0.123456, 1500);
        L.apply("X", 8, "USDT", -0.000001, 1500);
        L.mark_synced("X", 1600);
        L.set_backfilled(true);
        const std::string p = "fee_ledger_test.json";
        check(L.save(p), "保存成功");
        FeeLedger R;
        check(R.load(p), "读回成功");
        const auto a = L.get("X"), b = R.get("X");
        check_near(b.total, a.total, 1e-12, "合计无损");
        check(b.count == 2 && b.cursor_ms == 1500 && b.synced_to_ms == 1600, "计数、游标、同步点无损");
        check(b.ids_at_cursor.size() == 2, "  游标那一毫秒的 tranId 也要存 —— 否则重启后会重复计");
        check(R.get("Y").spans.size() == 1 && R.get("Y").spans[0].to_ms == 20, "已结束的时间段无损");
        check(R.backfilled(), "「已补算过」标记无损 —— 否则每次启动都补一遍");
        check(!R.apply("X", 8, "USDT", -0.000001, 1500), "  读回后同一条不会重复计");
        std::remove(p.c_str());
    }

    std::printf("\n── 手续费6：向交易所同步（翻页与失败）──\n");
    {
        // 假交易所：一页拉满时返回区间里【最新】的那几条。照"接着上一页最后一条的
        // 时间往后拉"的翻页写法，中间那段会静默漏掉；对半切分的写法与顺序无关
        struct Rec { int64_t t; int64_t id; double v; };
        std::vector<Rec> data;
        int64_t id = 100;
        const int64_t now = 50LL * 24 * 3600 * 1000;
        const int64_t base = now - 2 * 3600 * 1000;
        for (int i = 0; i < 20; ++i) {
            data.push_back({base + i * 1000, ++id, -0.01});
            if (i % 4 == 0) data.push_back({base + i * 1000, ++id, -0.01});   // 同一毫秒第二条
        }
        double expect = 0;
        for (const auto& r : data) expect += r.v;

        auto num = [](const std::string& s, const char* key) -> int64_t {
            const auto p = s.find(key);
            if (p == std::string::npos) return 0;
            return std::stoll(s.substr(p + std::string(key).size()));
        };
        int calls = 0, fail_on = -1;
        TradingClient::Config c; c.api_key = "k"; c.api_secret = "s"; c.testnet = true;
        TradingClient tc(c);
        tc.set_test_hook([&](const std::string&, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("income") == std::string::npos) return false;
            ++calls;
            if (calls == fail_on) { out.body = "<html>502</html>"; return true; }
            const int64_t s = num(params, "startTime="), e = num(params, "endTime=");
            const int64_t lim = num(params, "limit=");
            std::vector<Rec> in;
            for (const auto& r : data) if (r.t >= s && r.t <= e) in.push_back(r);
            if ((int64_t)in.size() > lim) in.erase(in.begin(), in.end() - lim);   // 只给最新的
            std::string b = "[";
            for (size_t k = 0; k < in.size(); ++k)
                b += std::string(k ? "," : "") + "{\"symbol\":\"BTCUSDT\",\"incomeType\":\"COMMISSION\","
                     "\"income\":\"" + std::to_string(in[k].v) + "\",\"asset\":\"USDT\",\"time\":" +
                     std::to_string(in[k].t) + ",\"tranId\":" + std::to_string(in[k].id) + "}";
            out.body = b + "]";
            return true;
        });

        FeeLedger L;
        L.open_span("BTCUSDT", base - 1000);
        bool ok = false;
        sync_fee_ledger(tc, L, now, &ok, /*page_limit=*/3);
        check(ok, "同步成功");
        check(L.get("BTCUSDT").count == (int)data.size(),
              "⚠ 一页只给 3 条且给的是最新的，也必须一条不漏（实际 " +
              std::to_string(L.get("BTCUSDT").count) + " / " + std::to_string(data.size()) + "）");
        check_near(L.total(), expect, 1e-9, "  合计正确");

        sync_fee_ledger(tc, L, now + 1000, &ok, 3);
        check(L.get("BTCUSDT").count == (int)data.size(), "再同步一次不会重复计");

        // 失败：没拉成的那段不能被标成已同步，否则那段手续费永久漏掉
        FeeLedger F;
        F.open_span("BTCUSDT", base - 1000);
        calls = 0; fail_on = 1;
        sync_fee_ledger(tc, F, now, &ok, 3);
        check(!ok, "交易所返回 HTML 错误页 ⇒ 报告没同步完");
        check(F.get("BTCUSDT").synced_to_ms == 0, "  ⚠ 那段不能标成已同步");
        calls = 0; fail_on = -1;
        sync_fee_ledger(tc, F, now, &ok, 3);
        check(ok && F.get("BTCUSDT").count == (int)data.size(), "  下次同步补齐，一条不漏");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
