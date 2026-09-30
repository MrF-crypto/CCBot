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

    std::printf("── 用例11：硬止损的错误分类（可重试 vs 白等）──\n");
    {
        // 重试阶梯要跑十三次、三十多秒，而这段时间里仓位是【没有进程外保护】的。
        // 所以"这个错还有没有救"这个判断的代价是不对称的：
        //   误判成可重试 → 白裸三十多秒，然后照样兜底平仓
        //   误判成没救   → 把一次能恢复的网络抖动直接变成"平掉刚开的仓"
        // 于是只有【确定无望】的码才标不可重试。这个用例钉住两边各一个代表。
        auto probe = [](const char* body) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m == "POST") { out.body = body; return true; }
                return false;
            });
            return tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
        };

        // -4120「这个端点不收这个单型」：重试十三次结果完全一样。
        // 实测（2026-09-29，$210 的普通合约账户）13 次全 -4120，历时 36 秒
        auto r1 = probe(R"({"code":-4120,"msg":"Order type not supported for this )"
                        R"(endpoint. Please use the Algo Order API endpoints instead."})");
        check(!r1.ok(), "-4120 是失败");
        check(!r1.retryable, "-4120 不可重试：端点/单型不匹配，白等三十多秒");
        check(r1.error.find("账户模式") != std::string::npos,
              "  并提示去核对账户模式（它是手工选的，选错时只有条件单会露馅）");

        // -4120 的定向回退：币安自己说"请用 Algo Order API"，那就按它说的
        // 换条件单端点再试一次。成功了就说明账户模式设错了，必须报出来——
        // 否则每次开仓都白走一次被拒 + 一次回退，而中间仓位没有进程外保护
        {
            TradingClient tc(test_cfg());
            int fapi_posts = 0, cond_posts = 0;
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m != "POST") return false;
                if (path.find("conditional") != std::string::npos) {
                    ++cond_posts;
                    out.body = R"({"strategyId":778899,"strategyStatus":"NEW"})";
                } else {
                    ++fapi_posts;
                    out.body = R"({"code":-4120,"msg":"Order type not supported for )"
                               R"(this endpoint. Please use the Algo Order API endpoints instead."})";
                }
                return true;
            });
            auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
            check(fapi_posts == 1, "普通端点先试一次");
            check(cond_posts == 1, "  被 -4120 拒后，换条件单端点再试一次（只一次）");
            check(r.ok(), "  回退成功即算挂上");
            check(r.order_id == "778899",
                  "  单号取 strategyId（条件单端点不给 orderId，取错会变成"
                  "「挂上了但以为没挂上」——那是最坏的一种）");
            check(r.via_cond_fallback,
                  "  必须标出这是回退挂上的，好让上层提示改账户模式");
        }
        {
            // 回退也失败时，保留【首发的 -4120】而不是条件单端点那个更陌生的码：
            // 上层要按"端点不对"处置，换个码只会误导人
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m != "POST") return false;
                out.body = (path.find("conditional") != std::string::npos)
                    ? R"({"code":-2015,"msg":"Invalid API-key, IP, or permissions."})"
                    : R"({"code":-4120,"msg":"Order type not supported for this endpoint."})";
                return true;
            });
            auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
            check(!r.ok(), "回退也失败 ⇒ 整体失败");
            check(r.error.find("-4120") != std::string::npos,
                  "  保留首发的 -4120，不要换成条件单端点那个陌生码");
            check(!r.retryable, "  仍然按「重试无意义」处置");
            check(!r.via_cond_fallback, "  没挂上就不该标成回退成功");
        }

        // ── 写法变体阶梯：HTTP 400 时自己退到更保守的参数组合 ────────────────
        // 实测里市价单成功而同一端点的 STOP_MARKET 拿 400，差别只在参数里，
        // 而靠读代码猜了四轮都没定位到。所以让它自己按"功能最全 → 最保守"试一遍，
        // 每个变体都是同等保护力的同一张单（同一触发价、同样平掉整个仓位），
        // 只是把可选增强逐个摘掉
        {
            TradingClient tc(test_cfg());
            std::vector<std::string> sent;
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string& params, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m != "POST") return false;
                sent.push_back(params);
                // 模拟"带 priceProtect 就 400"：这正是要让它自己绕过去的那类
                if (params.find("priceProtect") != std::string::npos) {
                    out.code = 400;
                    out.body = "<!DOCTYPE html><html><body>bad request</body></html>";
                } else {
                    out.code = 200;
                    out.body = R"({"orderId":4242,"status":"NEW"})";
                }
                return true;
            });

            auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
            check(r.ok(), "带 priceProtect 被 400 拒后，自动退到不带它的写法并挂上");
            check(sent.size() == 2, "  只多试一次（第一个成功就停）");
            check(sent[0].find("priceProtect") != std::string::npos, "  第一发是完整写法");
            check(sent[1].find("priceProtect") == std::string::npos, "  第二发摘掉了 priceProtect");
            check(sent[1].find("closePosition=true") != std::string::npos,
                  "  但 closePosition 必须还在 —— 摘的只能是可选增强，不能是保护本身");
            check(sent[1].find("stopPrice=60000") != std::string::npos,
                  "  触发价也必须还在");
            check(sent[1].find("workingType=MARK_PRICE") != std::string::npos,
                  "  workingType 还没到要退的那一档（标记价触发优先保留）");
            check(!r.note.empty(), "  换了写法要留一句说明给上层打日志");

            // 记住成功的那个：下一次直接用，稳态仍是一次请求
            sent.clear();
            auto r2 = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
            check(r2.ok() && sent.size() == 1,
                  "第二次只发一次请求（记住了能用的写法，不再白试被拒的那个）");
            check(sent[0].find("priceProtect") == std::string::npos, "  直接用能用的那个");
            check(r2.note.empty(), "  没有变化就不要重复打说明（否则每次挂单都刷一条）");
        }
        {
            // 非参数类失败不该触发换写法：空响应是网络问题，换参数毫无意义，
            // 而白试三遍等于把一次网络抖动变成三倍的挂单延迟
            TradingClient tc(test_cfg());
            int posts = 0;
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m == "POST") { ++posts; out.body = ""; return true; }
                return false;
            });
            auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY");
            check(!r.ok() && r.retryable, "空响应仍是可重试的失败");
            check(posts == 1, "  但不得为它轮换写法：那是网络问题，不是参数问题");
        }

        // -1003 限流：过一会儿就好了，正是阶梯存在的理由
        auto r2 = probe(R"({"code":-1003,"msg":"Too many requests."})");
        check(!r2.ok() && r2.retryable, "-1003 限流可重试：这正是重试阶梯的用途");

        // HTML 错误页：请求没到 API。看起来该直接判死，但 Cloudflare 的 502/503
        // 也是 HTML，而那种真能重试过去 —— 保持可重试，只把诊断说清楚
        auto r3 = probe("<!DOCTYPE html><html><head></head><body>error</body></html>");
        check(!r3.ok() && r3.retryable, "HTML 响应仍可重试（502/503 也是 HTML）");
        check(r3.error.find("HTML") != std::string::npos,
              "  但要明说收到的是网页、请求没到币安");
        // 要钉的是"HTML 自己的换行不得漏进日志"——那会把一条日志撑成几十行。
        // 错误串本身用了两行做排版（端点/参数/响应体分行），那是有意的，
        // 所以不能简单断言"整串无换行"，要断言【响应体那一段】无换行
        {
            const auto p = r3.error.find("响应体: ");
            check(p != std::string::npos, "  错误里要有「响应体」这一段");
            if (p != std::string::npos)
                check(r3.error.find('\n', p) == std::string::npos,
                      "  且响应体段内必须压平换行（HTML 原样打出来会撑成几十行）");
        }
        check(r3.error.find("实际发出的参数") != std::string::npos,
              "  还要带上实际发出的参数：靠读代码猜是哪个参数不对已经绕了好几轮");
        check(r3.error.find("signature") == std::string::npos,
              "  但绝不能带 signature —— 那是用 API secret 算的 HMAC，"
              "写进日志/工单等于泄露一次可重放的凭据");

        // ── 状态码必须出现在错误里，且按码给出不同的成因 ──────────────────
        // 这是把成因分开的唯一依据：实测里同一个端点 5 秒内一次成功（市价单）、
        // 一次拿到 HTML（条件单），光看 body 完全解释不了。
        // 四种码的修法南辕北辙：404 是代码缺陷、403/451 是环境、429 等等就好、
        // 200+HTML 是中间件冒充
        auto probe_code = [](const char* body, long code) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m == "POST") { out.body = body; out.code = code; return true; }
                return false;
            });
            return tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY").error;
        };
        const char* kHtml = "<!DOCTYPE html><html><body>err</body></html>";
        check(probe_code(kHtml, 404).find("404") != std::string::npos &&
              probe_code(kHtml, 404).find("代码缺陷") != std::string::npos,
              "404 ⇒ 说清是【我们的】端点拼错了，别让人去查网络");
        check(probe_code(kHtml, 451).find("地域") != std::string::npos,
              "451 ⇒ 地域拦截，换节点");
        check(probe_code(kHtml, 429).find("限流") != std::string::npos,
              "429 ⇒ 限流，等一等会好");
        check(probe_code(kHtml, 200).find("中间件") != std::string::npos,
              "200 却是 HTML ⇒ 不可能出自币安 API，是中间件冒充");
        check(probe_code(kHtml, 200).find("/fapi/v1/order") != std::string::npos,
              "  并带上实际用的端点路径（省掉一轮「你到底发去哪了」）");
    }

    std::printf("── 用例12：全市场标记价（WS 断流时兜底价的新鲜度全靠它）──\n");
    {
        // 逐品种查是 N 次往返，一轮耗时随品种数线性增长，最后那个品种拿到的价
        // 已经比第一个旧了好几秒。全取是 1 次往返、所有品种共享同一时间戳。
        // 这条路径只在 WS 断流时跑 —— 也就是最需要它对的时候，平时不执行
        auto probe = [](const char* body) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string&, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("premiumIndex") != std::string::npos) {
                    out.body = body; return true;
                }
                return false;
            });
            return tc.fetch_all_mark_prices();
        };

        auto m = probe(R"([{"symbol":"BTCUSDT","markPrice":"83610.20","lastFundingRate":"0.0001"},)"
                       R"({"symbol":"ETHUSDT","markPrice":"2685.0987"},)"
                       R"({"symbol":"XLMUSDT","markPrice":"0.23220000"}])");
        check(m.size() == 3, "三个品种一次取回");
        check(m.count("BTCUSDT") && std::fabs(m["BTCUSDT"] - 83610.20) < 1e-6, "  BTC 价格");
        check(m.count("ETHUSDT") && std::fabs(m["ETHUSDT"] - 2685.0987) < 1e-9,
              "  ETH 价格：小数位不得被截（止损线就是出场价）");
        check(m.count("XLMUSDT") && std::fabs(m["XLMUSDT"] - 0.2322) < 1e-9, "  微价品种");

        // 币安出错时回的是对象而不是数组。必须返回空表让调用方保留旧价，
        // 绝不能因为一次拉取失败就把价格清掉（那会让移动止损失去基准）
        check(probe(R"({"code":-1121,"msg":"Invalid symbol."})").empty(),
              "错误对象（而非数组）⇒ 空表，调用方保留旧价");
        check(probe("").empty(),            "空响应 ⇒ 空表");
        check(probe("not json at all").empty(), "非 JSON ⇒ 空表，不抛异常");

        // 坏条目要被跳过而不是让整批失败：一个新上币种字段缺失不该拖垮其余品种
        auto mixed = probe(R"([{"symbol":"BTCUSDT","markPrice":"83610.20"},)"
                           R"({"symbol":"BADUSDT"},)"
                           R"({"markPrice":"1.0"},)"
                           R"({"symbol":"ZEROUSDT","markPrice":"0"},)"
                           R"({"symbol":"NANUSDT","markPrice":"abc"},)"
                           R"({"symbol":"ETHUSDT","markPrice":"2685.10"}])");
        check(mixed.size() == 2, "坏条目逐个跳过，好的照常返回（缺字段/零价/非数字）");
        check(mixed.count("BTCUSDT") && mixed.count("ETHUSDT"), "  两个好品种都在");
        check(!mixed.count("ZEROUSDT"), "  价格为 0 不算有效价");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
