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
// ⚠ <cmath> 必须显式写：MSVC 的标准库会传递包含它，libstdc++ 不会，
//   于是 std::fabs 在本机编得过、推上去 GCC 直接拒。本机那道
//   clang --driver-mode=g++ 扫描也抓不到 —— 它用的是 MSVC 的头文件
#include <cmath>
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

    std::printf("── 用例11：硬止损走 algoOrder 端点（币安 2025-12-09 起强制）──\n");
    {
        // 币安 2025-11-06 公告、2025-12-09 强制：STOP_MARKET 这类条件单不再接受
        // /fapi/v1/order，必须走 /fapi/v1/algoOrder。老端点上发会回 -4120。
        // 这个用例把新端点的四条硬规格钉住 —— 每一条错了都是静默后果
        std::string seen_path, seen_params;
        TradingClient tc(test_cfg());
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) {
                out.body = kExchangeInfo; return true;
            }
            if (m != "POST") return false;
            seen_path = path; seen_params = params;
            // 回执按真实形状：algoId + triggerPrice
            out.body = R"({"algoId":2146760,"clientAlgoId":"x","algoType":"CONDITIONAL",)"
                       R"("orderType":"STOP_MARKET","symbol":"BTCUSDT","side":"SELL",)"
                       R"("triggerPrice":"60000.00","algoStatus":"NEW"})";
            return true;
        });
        auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY", 0.015);

        // ⚠ 两边路径不对称，别照着一边推另一边：
        //   普通合约 /fapi/v1/algoOrder（驼峰、无斜杠）
        //   统一账户 /papi/v1/um/algo/order（有斜杠）
        //   我按对称猜错过一次，实测回 HTTP 404
        check(seen_path == "/fapi/v1/algoOrder",
              "① 普通合约的端点必须【正好】是 /fapi/v1/algoOrder"
              "（老的 /fapi/v1/order 会回 -4120）");
        check(seen_params.find("algoType=CONDITIONAL") != std::string::npos,
              "② algoType=CONDITIONAL 是必填项，漏了整条请求非法");
        check(seen_params.find("triggerPrice=60000") != std::string::npos,
              "③ 触发价参数名是 triggerPrice");
        check(seen_params.find("stopPrice") == std::string::npos,
              "   而【绝不能】用 stopPrice —— 新端点对它不报错，但那张单永远不会触发");
        check(seen_params.find("workingType=MARK_PRICE") != std::string::npos,
              "④ workingType 必须显式写 MARK_PRICE（新端点默认 CONTRACT_PRICE，"
              "而插针打的就是成交价）");
        check(r.ok() && r.order_id == "2146760", "单号取 algoId");
    }
    {
        // ⚠ 这一条是整件事里最危险的失败模式的防线：
        //   触发价参数名传错时币安【不报错】、照常给 algoId，而那张单永远不触发。
        //   只看 algoId 的话我们会打出"硬止损已挂"，而仓位其实完全没有底 ——
        //   静默的假保护比挂不上危险得多（挂不上至少会走兜底平仓）
        int cancels = 0;
        TradingClient tc(test_cfg());
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) {
                out.body = kExchangeInfo; return true;
            }
            if (m == "DELETE") {
                ++cancels;
                check(params.find("algoId=99") != std::string::npos,
                      "  撤单用 algoId（普通撤单端点 + orderId 撤不掉条件单）");
                out.body = R"({"algoId":99})";
                return true;
            }
            if (m != "POST") return false;
            // 接受了，但回执里的触发价是 0 —— 正是"传错参数名"的表现
            out.body = R"({"algoId":99,"triggerPrice":"0","algoStatus":"NEW"})";
            return true;
        });
        auto r = tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY", 0.015);
        check(!r.ok(), "回执触发价不对 ⇒ 必须当【挂单失败】，不能当成功");
        check(r.error.find("不会触发") != std::string::npos, "  错误要说清它不会触发");
        check(!r.retryable, "  且不可重试：参数名/格式问题，重试结果一样");
        check(cancels == 1,
              "  而且必须把那张单撤掉 —— 留着就是一张永不触发的孤儿单，"
              "而 closePosition 同方向只允许一张，下次开仓就挂不上了");
    }
    {
        // 错误分类：只有【确定无望】的才标不可重试。判错的代价不对称 ——
        // 误判可重试只是白等三十多秒然后照样兜底；误判不可重试会把一次能恢复的
        // 网络抖动直接变成"平掉刚开的仓"
        auto probe = [](const char* body, long code) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string& m, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = kExchangeInfo; return true;
                }
                if (m == "POST") { out.body = body; out.code = code; return true; }
                return false;
            });
            return tc.place_disaster_stop("BTCUSDT", 60000.0, "BUY", 0.015);
        };
        auto a = probe(R"({"code":-4120,"msg":"Order type not supported for this endpoint."})", 400);
        check(!a.ok() && !a.retryable,
              "-4120 不可重试（端点/单型不匹配，重试十三次结果一样）");
        auto b = probe(R"({"code":-1003,"msg":"Too many requests."})", 429);
        check(!b.ok() && b.retryable, "-1003 限流【可】重试，这正是重试阶梯的用途");
        auto c = probe("<!DOCTYPE html><html><body>err</body></html>", 502);
        check(!c.ok() && c.retryable,
              "HTML 响应仍可重试（Cloudflare 的 502/503 也是 HTML）");
        check(c.error.find("502") != std::string::npos, "  错误里要带上 HTTP 状态码");
        check(c.error.find("algoOrder") != std::string::npos, "  以及实际用的端点");
        // ⚠ 404 必须明说是【我们的】端点写错了。实测救了一次：统一账户的路径
        //   是 /papi/v1/um/algo/order（有斜杠），我按普通合约那边的驼峰猜成
        //   /papi/v1/um/algoOrder，回了 404。没有这句解释就会被当成又一次网络问题
        auto e = probe("<!DOCTYPE html><html><body>not found</body></html>", 404);
        check(e.error.find("404") != std::string::npos, "404 要带上状态码");
        check(e.error.find("代码缺陷") != std::string::npos,
              "  并明说是我们的端点写错了，别让人再去查网络");
        check(c.error.find('\n') == std::string::npos,
              "  且压平换行：HTML 原样打出来会把一条日志撑成几十行");
        auto d = probe("", 0);
        check(!d.ok() && d.retryable, "空响应可重试（可能其实挂上了，重挂会被拒）");
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

    std::printf("── 用例13：持仓快照的 ok 语义（对账的输入，错了会清掉真实持仓）──\n");
    {
        // 对账把"交易所没有这个仓位"处置成【清掉本地跟踪并停掉 bot】。所以这份
        // 快照必须能区分"确实没仓"和"这次没拉到" —— 混淆的后果是一笔真实持仓
        // 变成无人看管的裸敞口，而且发生在启动、刚恢复完仓位之后
        auto probe = [](const char* body, bool* ok_out) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string&, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("positionRisk") != std::string::npos) { out.body = body; return true; }
                return false;
            });
            return tc.fetch_positions(ok_out);
        };
        bool ok = true;
        auto v = probe("", &ok);
        check(!ok && v.empty(), "空响应 ⇒ ok=false（不是「交易所没有仓位」）");

        ok = true;
        v = probe(R"({"code":-1121,"msg":"Invalid symbol."})", &ok);
        check(!ok, "错误对象（而非数组）⇒ ok=false");

        ok = false;
        v = probe("[]", &ok);
        check(ok && v.empty(), "合法的空数组 ⇒ ok=true 且无仓位（这才是真的没仓）");

        ok = false;
        v = probe(R"([{"symbol":"BTCUSDT","positionAmt":"0.015","entryPrice":"60000",)"
                  R"("markPrice":"60100","unRealizedProfit":"1.5","liquidationPrice":"0",)"
                  R"("leverage":"3"}])", &ok);
        check(ok && v.size() == 1, "正常一条 ⇒ ok=true");
        if (v.size() == 1) {
            check(v[0].direction == 1 && std::fabs(v[0].qty - 0.015) < 1e-9, "  多头 0.015");
        }

        ok = false;
        v = probe(R"([{"symbol":"BTCUSDT","positionAmt":"-0.02","entryPrice":"60000",)"
                  R"("markPrice":"59000","unRealizedProfit":"20","liquidationPrice":"0",)"
                  R"("leverage":"3"}])", &ok);
        check(ok && v.size() == 1 && v[0].direction == -1, "负数量 ⇒ 空头");

        // ⚠ 单条解析失败必须把【整份快照】标成不可信，而不是静默少一条：
        //   少的那一条在对账眼里就是"交易所没有这个仓位" ⇒ 清掉本地跟踪
        ok = true;
        v = probe(R"([{"symbol":"BTCUSDT","positionAmt":"0.015","entryPrice":"60000"},)"
                  R"({"symbol":"ETHUSDT","positionAmt":"abc","entryPrice":"2600"}])", &ok);
        check(!ok, "有一条 positionAmt 解析不出来 ⇒ 整份快照 ok=false");
        ok = true;
        v = probe(R"([{"symbol":"BTCUSDT","positionAmt":"0.015"},)"
                  R"({"symbol":"ETHUSDT","entryPrice":"2600"}])", &ok);
        check(!ok, "有一条缺 positionAmt ⇒ 同样不可信（缺字段和解析失败等价）");
    }

    std::printf("── 用例14：品种校验必须分清「不存在」和「没查成」──\n");
    {
        // 混起来的后果实测过：网络抖一下，一个完全正确的品种被界面报成
        // "在币安 USDT-M 合约上不存在，请检查拼写" —— 而拼写本来就是对的。
        // 判据：币安对不存在的品种回的是合法 JSON（-1121），
        // 空响应 / 非 JSON 才是没查成
        auto probe = [](const char* body) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string&, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("exchangeInfo") != std::string::npos) {
                    out.body = body; return true;
                }
                return false;
            });
            return tc.get_symbol_info("BTCUSDT");
        };

        auto ok = probe(kExchangeInfo);
        check(ok.valid && !ok.lookup_failed, "正常应答：valid 且没有标成没查成");

        // 币安对不存在的品种：合法 JSON 的错误对象 ⇒ 查成了，它真的不存在
        auto gone = probe(R"({"code":-1121,"msg":"Invalid symbol."})");
        check(!gone.valid, "不存在的品种：valid=false");
        check(!gone.lookup_failed,
              "  但【不是】没查成 —— 这种才该报「请检查拼写」");

        // 网络失败的两种形态
        auto empty = probe("");
        check(!empty.valid && empty.lookup_failed, "空响应：标成没查成，不得说品种不存在");
        auto html = probe("<!DOCTYPE html><html><body>gateway error</body></html>");
        check(!html.valid && html.lookup_failed,
              "网关 HTML 页：同样是没查成（这台机器上这是常态）");

        // 缺 symbols 数组但 JSON 合法：算查成了、确实没有
        auto weird = probe(R"({"serverTime":1700000000000})");
        check(!weird.valid && !weird.lookup_failed,
              "合法 JSON 但没有 symbols：算查成了，不是网络问题");
    }

    std::printf("── 用例15：活跃条件单列表（核对保护单还在不在的输入）──\n");
    {
        // 与持仓快照同一个道理：空列表既可能是"确实没有活跃单"，也可能是这次
        // 没查成。混起来的后果是一次网络抖动让【所有】仓位的保护单被判成已消失，
        // 于是白撤白挂一轮，而 closePosition 同方向只允许一张，新的还可能挂不上
        auto probe = [](const char* body, bool* ok_out) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string&, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("AlgoOrders") != std::string::npos ||
                    path.find("algo") != std::string::npos) { out.body = body; return true; }
                return false;
            });
            return tc.fetch_open_algo_ids(ok_out);
        };
        bool ok = true;
        auto v = probe("", &ok);
        check(!ok && v.empty(), "空响应 ⇒ ok=false（不是「没有活跃单」）");

        ok = true;
        v = probe(R"({"code":-1130,"msg":"Invalid parameter."})", &ok);
        check(!ok, "错误对象（而非数组）⇒ ok=false");

        ok = false;
        v = probe("[]", &ok);
        check(ok && v.empty(), "合法空数组 ⇒ ok=true 且确实没有活跃单");

        ok = false;
        v = probe(R"([{"algoId":2146760,"symbol":"BTCUSDT","algoStatus":"NEW"},)"
                  R"({"algoId":889900,"symbol":"ETHUSDT","algoStatus":"NEW"}])", &ok);
        check(ok && v.size() == 2, "两条活跃单");
        check(v.count("2146760") && v.count("889900"), "  id 都在（按字符串比对）");

        // 单条缺 algoId ⇒ 整份快照不可信：少一条 id 就会让那张真实存在的
        // 保护单被判成"已消失"，白撤白挂
        ok = true;
        v = probe(R"([{"algoId":2146760,"symbol":"BTCUSDT"},{"symbol":"ETHUSDT"}])", &ok);
        check(!ok, "有一条缺 algoId ⇒ 整份快照 ok=false");
    }

    std::printf("── 用例16：持仓模式不能把「没查到」当成「单向」──\n");
    {
        // dual_mode_ 决定【每一张订单】要不要带 positionSide。在双开账户上漏带
        // 会被币安以 -4061 拒单，而 -4061 在硬止损那边是【不可重试】的码 ——
        // 直接走兜底平仓。所以这个猜测的代价是真金白银。
        // 实测日志里同一个账户在不同次启动上报出过两种模式，而用户没改过设置
        auto probe = [](const char* body, bool* ok_out) {
            TradingClient tc(test_cfg());
            tc.set_test_hook([&](const std::string&, const std::string& path,
                                 const std::string&, TradingClient::FakeReply& out) {
                if (path.find("positionSide/dual") != std::string::npos) {
                    out.body = body; return true;
                }
                return false;
            });
            // 先成功读到"双向"，再看失败时会不会把它冲回 false
            bool warm = false;
            tc.set_test_hook([&, body](const std::string&, const std::string& path,
                                       const std::string&, TradingClient::FakeReply& out) {
                if (path.find("positionSide/dual") == std::string::npos) return false;
                out.body = warm ? body : R"({"dualSidePosition":true})";
                return true;
            });
            bool first_ok = false;
            const bool d1 = tc.fetch_position_mode(&first_ok);
            warm = true;
            const bool d2 = tc.fetch_position_mode(ok_out);
            return std::make_pair(d1 && first_ok, d2);
        };

        bool ok = true;
        auto r = probe("", &ok);
        check(r.first, "先成功读到「双向持仓」");
        check(!ok, "  之后拉取失败 ⇒ ok=false");
        check(r.second, "  且【沿用】上次的双向判断，不得冲回默认的 false —— "
                        "冲回去等于把「没查到」变成「单向持仓」这个断言");

        ok = true;
        r = probe("not json", &ok);
        check(!ok && r.second, "非 JSON 同理：ok=false 且沿用上次的值");

        ok = true;
        r = probe(R"({"serverTime":1700000000000})", &ok);
        check(!ok && r.second,
              "字段缺失也算没查到 —— 取不到时值会留 false，而那正好与「单向」无法区分");

        ok = false;
        r = probe(R"({"dualSidePosition":false})", &ok);
        check(ok && !r.second, "真的读到 false ⇒ ok=true 且如实返回单向");
    }

    std::printf("\n── 用例17：-4061 ⇒ 重新探测持仓模式并自愈 ──\n");
    // dual_mode_ 只在【连接那一刻】探一次，此后整个进程都不再更新。账户的持仓
    // 模式一旦在连接之后被改掉（币安要求改模式时无持仓无挂单，所以通常发生在
    // 人工清理完仓位之后），本进程每一笔单都会被 -4061 拒到重启为止。
    // 实盘 2026-10-01：连接时探到双向，02:15 起平仓/开仓全部 -4061。
    {
        TradingClient tc(test_cfg());
        // 先把 dual_mode_ 探成【双向】，模拟连接那一刻的状态
        tc.set_test_hook([&](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("positionSide/dual") != std::string::npos) {
                out.body = R"({"dualSidePosition":true})"; return true;
            }
            return false;
        });
        bool ok0 = false;
        check(tc.fetch_position_mode(&ok0) && ok0, "连接时探到双向持仓（前提成立）");

        // 现在账户被改成【单向】：带 positionSide 的单一律 -4061
        int posts = 0, probes = 0;
        std::string last_params;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string& params, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (path.find("positionSide/dual") != std::string::npos) {
                ++probes;
                out.body = R"({"dualSidePosition":false})";   // 已经改成单向了
                return true;
            }
            if (m == "POST") {
                ++posts;
                last_params = params;
                if (params.find("positionSide=") != std::string::npos) {
                    out.body = R"({"code":-4061,"msg":"Order's position side does not match user's setting."})";
                } else {
                    out.body = R"({"orderId":1234,"status":"FILLED","avgPrice":"63000",)"
                               R"("executedQty":"0.015","origQty":"0.015"})";
                }
                return true;
            }
            return false;
        });

        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(r.ok, "⚠ -4061 之后必须自愈成功，而不是把这笔单判死");
        check(probes >= 1, "  必须重新探测过持仓模式");
        check(posts == 2, "  且只重发一次（不是无限重试）");
        check(last_params.find("positionSide=") == std::string::npos,
              "  重发时不再带 positionSide —— 说明用的是探回来的新模式");
        check(!tc.is_dual_mode(), "  dual_mode_ 已更新为单向");
    }

    std::printf("\n── 用例18：-4061 但模式没变 ⇒ 不重发，并把结论写进错误 ──\n");
    // 模式确实没变时再重发一次只是白烧一次限流额度，而且会掩盖真正的原因。
    // 更重要的是要把"已经查过、不是这个原因"写进错误里，否则下一个人还会
    // 从"是不是模式变了"重新猜一遍
    {
        TradingClient tc(test_cfg());
        int posts = 0;
        tc.set_test_hook([&](const std::string& m, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("exchangeInfo") != std::string::npos) { out.body = kExchangeInfo; return true; }
            if (path.find("positionSide/dual") != std::string::npos) {
                out.body = R"({"dualSidePosition":false})";   // 一直是单向，没变过
                return true;
            }
            if (m == "POST") {
                ++posts;
                out.body = R"({"code":-4061,"msg":"Order's position side does not match user's setting."})";
                return true;
            }
            return false;
        });
        auto r = tc.place_market("BTCUSDT", "BUY", 0.015, false);
        check(!r.ok, "仍然失败");
        check(posts == 1, "  模式没变就不该重发");
        check(r.error.find("与下单时一致") != std::string::npos,
              "  错误里要写明「已经查过、不是模式变更导致」");
    }

    // ⚠ place_market 里还有一道 `if (r.uncertain) return r;`，没有对应用例：
    //   uncertain 只由"空响应/非 JSON + 查单也确认不了"那条分支产生，那时
    //   error 里不会有 -4061（压根没解析出 code），所以这两个条件【构造不出来】。
    //   留着它是纯防御：开仓不幂等，万一将来有别的路径同时置上这两者，
    //   重发会变成双倍仓位。不为一个构造不出的场景写假用例。

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
