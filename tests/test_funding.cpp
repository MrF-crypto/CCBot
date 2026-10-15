// 资金费账本的单元测试。
//
// 这是【账本】不是风控——它唯一的职责就是把数记对。所以测试全部围绕
// "记账正确性"：去重、年化换算、落盘往返。
#include "core/funding_ledger.h"
#include "net/trading_client.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <fstream>
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

    std::printf("\n── 用例2：收资金费的情况（费率为负）──\n");
    {
        FundingLedger L;
        L.apply("XRPUSDT", 1.20, 1000);         // 正数 = 你收到钱
        check_near(L.get("XRPUSDT").total, 1.20, 1e-9, "正收入正常记账");
        check_near(FundingLedger::annualized_pct(-0.0001), -10.95, 0.01,
                   "负费率年化为负（多头在收钱）");
    }

    std::printf("\n── 用例3：年化换算 ──\n");
    {
        // 每 8 小时一次 = 每天 3 次
        check_near(FundingLedger::annualized_pct(0.0001), 10.95, 0.01, "0.01%/8h → 年化 10.95%");
        check_near(FundingLedger::annualized_pct(0.0005), 54.75, 0.01, "0.05%/8h → 年化 54.75%");
        check_near(FundingLedger::annualized_pct(0.0),     0.0,  1e-9, "零费率");
    }

    std::printf("\n── 用例4：落盘往返 ──\n");
    {
        const std::string path = "test_funding.json";
        {
            FundingLedger L;
            L.apply("BTCUSDT", -12.5, 5000);
            L.apply("ETHUSDT",  -3.25, 6000);
            L.set_rate("BTCUSDT", 0.0001, 7000, 6500);
            check(L.save(path), "写盘");
        }
        {
            FundingLedger L;
            check(L.load(path), "读盘");
            check_near(L.get("BTCUSDT").total, -12.5, 1e-9, "BTC 总累计");
            check(L.cursor("BTCUSDT") == 5000, "BTC 游标（决定下次从哪拉，错了会重复计数）");
            check_near(L.get("ETHUSDT").total, -3.25, 1e-9, "ETH 总累计");
            // 费率是易变的实时量，不落盘 —— 重启后等下一次拉取即可
            check(L.get("BTCUSDT").rate_ms == 0, "费率不落盘（重启后重新拉）");
        }
        std::remove(path.c_str());
    }

    std::printf("\n── 用例5：老文件里的 since_open 读进来忽略，不报错 ──\n");
    {
        const std::string path = "test_funding_old.json";
        std::ofstream(path, std::ios::binary)
            << "{\"BTCUSDT\":{\"total\":-4.5,\"since_open\":-4.5,\"cursor_ms\":9000}}";
        FundingLedger L;
        check(L.load(path) && std::fabs(L.get("BTCUSDT").total + 4.5) < 1e-9 &&
                  L.cursor("BTCUSDT") == 9000,
              "老格式照常读出总累计与游标");
        std::remove(path.c_str());
    }

    std::printf("\n── 用例6：未知品种返回零值而非崩溃 ──\n");
    {
        FundingLedger L;
        auto e = L.get("NOSUCHUSDT");
        check(e.total == 0 && e.cursor_ms == 0, "未知品种是干净的零值");
        check(L.symbols().empty(), "只读查询不会创建条目");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
