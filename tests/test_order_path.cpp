// 订单路径测试。
//
// 这是整个项目最值钱、也最没人守着的一段代码：超时→查单恢复、部分成交、
// 零成交、-1111 精度重试。它们的共同特点是**平时永远不执行**——只有网络出问题
// 的那几秒才跑，而那一刻正是最需要它对的时候。而且没法在实盘演练：你不能要求
// 币安给你超时一次。
//
// 于是形成恶性循环：平时不跑 → 无法通过日常使用发现它坏了 → 改别的东西时顺手
// 动到它，几个月都没人知道，直到某天真断网了。
//
// 这些测试用假 HTTP 响应把那几秒复现出来。不需要网络。
#include "net/trading_client.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// 从 params 里抠出 newClientOrderId 的值（-1111 重试必须换新 ID，见用例6）
static std::string coid_of(const std::string& params) {
    const std::string k = "newClientOrderId=";
    auto p = params.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    auto e = params.find('&', p);
    return params.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static TradingClient::Config test_cfg() {
    TradingClient::Config c;
    c.api_key = "k"; c.api_secret = "s"; c.testnet = true;
    return c;
}

// 让 round_qty 有确定行为：不喂 exchangeInfo 时用的是内置默认（step 0.001）
static const char* kExchangeInfo =
    R"({"symbols":[{"symbol":"BTCUSDT","filters":[)"
    R"({"filterType":"LOT_SIZE","stepSize":"0.001","minQty":"0.001"},)"
    R"({"filterType":"PRICE_FILTER","tickSize":"0.10"}]}]})";

int main() {
    std::printf("── 用例1：下单超时，但订单其实已成交 ──\n");
    {
        // 最危险的场景：请求超时（空响应），而订单其实已经到达交易所并成交。
        // 若当普通失败返回，上层重试 = 双倍仓位
        TradingClient tc(test_cfg());
        int posts = 0, queries = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") { ++posts; out.body = ""; return true; }          // 超时：空响应
            if (m == "GET" && path.find("/order") != std::string::npos) {
                ++queries;
                out.body = R"({"orderId":991,"status":"FILLED","avgPrice":"63000.5",)"
                           R"("executedQty":"0.015","origQty":"0.015"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(r.ok, "查单恢复后返回成功");
        check(!r.uncertain, "状态已确认，uncertain=false");
        check(r.executed_qty > 0.0149 && r.executed_qty < 0.0151, "拿到真实成交数量 0.015");
        check(r.avg_price > 63000.0, "拿到真实成交均价");
        check(queries >= 1, "确实走了查单恢复（查了 " + std::to_string(queries) + " 次）");
    }

    std::printf("\n── 用例2：网关返回 HTML（Cloudflare 拦截页）──\n");
    {
        // 非空但不是 JSON，同样意味着"订单状态未知"，必须走查单，不能当普通失败
        TradingClient tc(test_cfg());
        bool queried = false;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") { out.code = 503; out.body = "<html><body>502 Bad Gateway</body></html>"; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) {
                queried = true;
                out.body = R"({"orderId":992,"status":"FILLED","avgPrice":"100","executedQty":"0.015"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(queried, "HTML 响应同样触发查单恢复（不能当普通失败）");
        check(r.ok, "查到已成交则返回成功");
    }

    std::printf("\n── 用例3：查单明确回答「订单不存在」──\n");
    {
        // 交易所明确说没有这笔单 ⇒ 确认没下进去 ⇒ uncertain 必须为 false，
        // 上层才敢安全重试。若误标 uncertain，bot 会被无谓地停掉
        TradingClient tc(test_cfg());
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") { out.body = ""; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) {
                out.body = R"({"code":-2013,"msg":"Order does not exist."})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "返回失败");
        check(!r.uncertain, "【关键】确认未到达交易所，uncertain=false（上层可安全重试）");
        check(r.error.find("未到达") != std::string::npos, "错误信息说明已确认: " + r.error);
    }

    std::printf("\n── 用例4：下单和查单都联系不上 ──\n");
    {
        // 真断网：无法确认订单到底成没成 ⇒ 必须 uncertain=true。
        // 引擎收到 uncertain 会停掉该 bot 等人工对账，绝不盲目重试（可能双倍仓位）
        TradingClient tc(test_cfg());
        int queries = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) ++queries;
            out.body = "";      // 一律空响应
            return true;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "返回失败");
        check(r.uncertain, "【关键】无法确认 → uncertain=true（引擎据此停机等对账）");
        check(queries == 3, "查单重试 3 次后放弃（实际 " + std::to_string(queries) + " 次）");
    }

    std::printf("\n── 用例5：下单被接受但零成交 ──\n");
    {
        // 市价单可能被接受却零成交（EXPIRED，无流动性）。客户端如实返回
        // executed_qty=0，由引擎的防幽灵仓逻辑拦住入账——两层各司其职
        TradingClient tc(test_cfg());
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") {
                out.body = R"({"orderId":993,"status":"EXPIRED","avgPrice":"0","executedQty":"0"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(r.ok, "交易所受理了，ok=true");
        check(r.executed_qty == 0.0, "【关键】executed_qty=0 如实上报，不能编造成下单量");
    }

    std::printf("\n── 用例6：-1111 精度超限重试必须换新的 clientOrderId ──\n");
    {
        // 每次重试都是【一笔新订单】。若复用同一个 clientOrderId，前一次若已部分
        // 送达交易所，重试就可能变成双倍
        TradingClient tc(test_cfg());
        std::vector<std::string> coids;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") {
                coids.push_back(coid_of(params));
                if (coids.size() <= 2) {
                    out.body = R"({"code":-1111,"msg":"Precision is over the maximum"})";
                } else {
                    out.body = R"({"orderId":994,"status":"FILLED","avgPrice":"100","executedQty":"0.01"})";
                }
                return true;
            }
            return false;
        });
        // 用 0.15：放宽到 0.1 档仍不会取整成 0。
        // （0.015 在第三档会归零，循环会正确地提前退出——见用例 6b）
        auto r = tc.place_market("BTCUSDT", "BUY", 0.15, false);
        check(r.ok, "第三次尝试成功");
        check(coids.size() == 3, "总共尝试 3 次（实际 " + std::to_string(coids.size()) + "）");
        bool all_diff = coids.size() == 3 &&
                        coids[0] != coids[1] && coids[1] != coids[2] && coids[0] != coids[2] &&
                        !coids[0].empty();
        check(all_diff, "【关键】每次重试都用【新的】clientOrderId，绝不复用");
    }

    std::printf("\n── 用例6b：放宽精度会把数量取整成 0 时必须停止 ──\n");
    {
        // -1111 重试是逐档放宽精度（0.001→0.01→0.1→1）。小额单放宽两档后会被
        // 取整成 0，此时必须停止而不是继续放宽——否则会拿 0 数量去下单。
        // 这条是写测试时才发现的：原本以为一定会重试满 4 次
        TradingClient tc(test_cfg());
        int posts = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") {
                ++posts;
                out.body = R"({"code":-1111,"msg":"Precision is over the maximum"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "返回失败");
        check(posts == 2, "放宽到会归零那一档就停止（实际发了 " + std::to_string(posts) + " 次）");
        check(r.error.find("最小下单量") != std::string::npos, "错误说明是数量不足: " + r.error);
    }

    std::printf("\n── 用例7：交易所业务错误直接返回，不走查单 ──\n");
    {
        // 保证金不足这类明确错误，订单确定没成立，走查单纯属浪费 1.8 秒
        TradingClient tc(test_cfg());
        int queries = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) ++queries;
            if (m == "POST") {
                out.body = R"({"code":-2019,"msg":"Margin is insufficient."})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "返回失败");
        check(!r.uncertain, "明确的业务错误，状态是确定的");
        check(queries == 0, "不触发查单恢复（省掉无谓的 1.8 秒等待）");
        check(r.error.find("-2019") != std::string::npos, "错误码透传: " + r.error);
    }

    // ── -1021 重发 ────────────────────────────────────────────────────────────
    // 网络（尤其 TCP 隧道）的秒级停顿会让时间戳抵达时已超出 recvWindow。
    // 这类失败与超时是【性质不同】的：recvWindow 校验在订单进撮合之前，
    // 校验没过订单根本没被创建，所以重发不存在双倍仓位风险。
    std::printf("\n── 用例8：首次 -1021、重发后成功 ──\n");
    {
        TradingClient tc(test_cfg());
        int posts = 0, queries = 0;
        std::string first_coid, second_coid;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) { ++queries; }
            if (m == "POST") {
                ++posts;
                // 记录两次用的 clientOrderId，验证是否复用（与用例6 恰好相反的要求）
                (posts == 1 ? first_coid : second_coid) = coid_of(params);
                if (posts == 1) {
                    out.body = R"({"code":-1021,"msg":"Timestamp for this request is outside of the recvWindow."})";
                    return true;
                }
                out.body = R"({"orderId":7001,"status":"FILLED","avgPrice":"63210.0",)"
                           R"("executedQty":"0.015","origQty":"0.015"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(r.ok, "重发后返回成功");
        check(posts == 2, "只重发了一次（实际下单 " + std::to_string(posts) + " 次）");
        check(queries == 0, "不走查单恢复——-1021 是确定没成交，不是状态未知");
        check(r.executed_qty > 0.0149 && r.executed_qty < 0.0151, "拿到真实成交数量，仓位只有一份");
        check(!first_coid.empty() && first_coid == second_coid,
              "重发复用同一个 clientOrderId（万一原单真进去了，重复ID会被拒而不是开第二个仓位）");
    }

    std::printf("\n── 用例9：持续 -1021，重发耗尽后如实失败 ──\n");
    {
        TradingClient tc(test_cfg());
        int posts = 0, queries = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) ++queries;
            if (m == "POST") {
                ++posts;
                out.body = R"({"code":-1021,"msg":"Timestamp for this request is outside of the recvWindow."})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "持续失败时如实返回失败，不假装成功");
        check(!r.uncertain, "状态是确定的（交易所明确拒绝），不该标记 uncertain");
        check(posts == 3, "首次 + 2 次重发 = 3（实际 " + std::to_string(posts) + "）");
        check(queries == 0, "始终不触发查单恢复");
        check(r.error.find("-1021") != std::string::npos, "错误码透传: " + r.error);
    }

    std::printf("\n── 用例10：空响应仍走查单，不被 -1021 重发路径吞掉 ──\n");
    {
        // 回归防护：重发判定必须严格。空响应是"状态未知"，如果被误判成可重发，
        // 就会在订单可能已成交的情况下再下一单 —— 双倍仓位
        TradingClient tc(test_cfg());
        int posts = 0, queries = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (m == "POST") { ++posts; out.body = ""; return true; }
            if (m == "GET" && path.find("/order") != std::string::npos) {
                ++queries;
                out.body = R"({"orderId":7002,"status":"FILLED","avgPrice":"63000.0",)"
                           R"("executedQty":"0.015","origQty":"0.015"})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(posts == 1, "空响应【不】重发下单（实际下单 " + std::to_string(posts) + " 次）");
        check(queries >= 1, "走的是查单恢复");
        check(r.ok, "查单确认已成交");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
