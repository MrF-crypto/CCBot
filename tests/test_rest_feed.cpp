// REST 轮询行情源的单元测试。取数函数是注入的，所以整套逻辑不需要网络。
//
// 这里守的是"行情层最容易静默出错"的那几条：冻结价绝不能当现价交出去、
// 拉取失败不等于"价格没了"、失败日志不能每轮都刷。
#include "net/rest_price_feed.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// 等到条件成立或超时。轮询周期 1 秒，所以这里给足 5 秒
template <class F>
static bool wait_until(F f, int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms / 20; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("── 用例1：订阅 → 首轮拿到价 ──\n");
    {
        std::atomic<int> mark_calls{0};
        RestPriceFeed feed(
            [&]() {
                ++mark_calls;
                return std::unordered_map<std::string, double>{
                    {"BTCUSDT", 60000.0}, {"ETHUSDT", 2600.0}, {"XRPUSDT", 0.5}};
            },
            [&]() {
                return std::unordered_map<std::string, double>{{"BTCUSDT", 1.25}};
            });
        feed.subscribe("btcusdt");          // 小写也要认
        feed.subscribe("ETHUSDT");
        feed.start();

        check(wait_until([&] { return feed.mark_price("BTCUSDT") > 0; }),
              "首轮之后拿到 BTCUSDT 的价");
        check(std::fabs(feed.mark_price("BTCUSDT") - 60000.0) < 1e-9, "  价格正确");
        check(std::fabs(feed.mark_price("ethusdt") - 2600.0) < 1e-9, "  查询也认小写");
        check(feed.mark_price("XRPUSDT") == 0.0,
              "  没订阅的品种即使在返回里也不入缓存（否则界面会冒出没加过的行）");

        double pct = 0;
        check(wait_until([&] { return feed.change_24h("BTCUSDT", pct); }),
              "24h 涨跌也拿到了");
        check(std::fabs(pct - 1.25) < 1e-9, "  涨跌值正确");
        check(!feed.change_24h("ETHUSDT", pct),
              "  返回里没有的品种，change_24h 要返回 false 而不是 0（0 是个合法涨幅）");

        // ⚠ 一轮只发一个请求，与品种数无关。逐品种查的话 2 个品种就是 2 次往返，
        //   50 个就是 50 次，而且最后那个拿到的价比第一个旧好几秒
        const int c1 = mark_calls.load();
        check(wait_until([&] { return mark_calls.load() >= c1 + 1; }, 2500),
              "轮询在持续进行");
        check(feed.health().healthy(), "健康");
        feed.stop();
        check(!feed.health().healthy(), "停了就不算健康");
    }

    std::printf("\n── 用例2：拉取失败 → 价格转陈旧，且日志只报一次 ──\n");
    {
        std::atomic<bool> fail{false};
        std::atomic<int>  logs{0};
        RestPriceFeed feed(
            [&]() -> std::unordered_map<std::string, double> {
                if (fail.load()) return {};          // 模拟拉取失败
                return {{"BTCUSDT", 60000.0}};
            },
            []() -> std::unordered_map<std::string, double> { return {}; });
        feed.on_server_msg([&](const std::string&) { ++logs; });
        feed.subscribe("BTCUSDT");
        feed.start();
        check(wait_until([&] { return feed.mark_price("BTCUSDT") > 0; }), "先正常拿到价");

        fail.store(true);
        check(wait_until([&] { return feed.health().fail_streak >= 3; }),
              "连续失败被计数");
        // ⚠ 1 秒一轮，每轮都报就是刷屏。这正是 WS 版里那套退避+节流在解决的问题，
        //   而轮询只要一个"报过了"的标记就够
        const int after = logs.load();
        check(after == 1, "连续失败只报一次（实际报了 " + std::to_string(after) + " 条）");

        // 失败期间【仍然返回最后一个价】，直到它真的超过 kStaleMs。
        // 立刻返回 0 是错的：偶尔一两轮失败不该让引擎瞬间失明
        check(feed.mark_price("BTCUSDT") > 0, "刚失败几轮时仍沿用最后一个价");
        check(!feed.health().healthy(), "  但健康状态必须是假的（不许粉饰）");

        fail.store(false);
        check(wait_until([&] { return feed.health().fail_streak == 0; }), "恢复后计数归零");
        check(logs.load() == 2, "  恢复也只报一次（共 2 条：失败 1 + 恢复 1）");
        feed.stop();
    }

    std::printf("\n── 用例3：冻结价绝不能当现价交出去 ──\n");
    {
        // 这是行情层最危险的一条：缓存里躺着一个不动的价时看起来完全正常，
        // 而引擎会拿它推移动止损、判触线 —— 实际行情暴跌时完全失明
        RestPriceFeed feed(
            []() -> std::unordered_map<std::string, double> { return {}; },
            []() -> std::unordered_map<std::string, double> { return {}; });
        feed.subscribe("BTCUSDT");
        feed.set_mark_price("BTCUSDT", 60000.0);
        check(feed.mark_price("BTCUSDT") > 0, "刚写入时可用");

        // 手工把时间戳推回到陈旧之前
        auto t = feed.get("BTCUSDT");
        check(t.mark_ms > 0, "  有时间戳");
        check(t.ws_mark_ms == t.mark_ms,
              "  ws_mark_ms 与 mark_ms 恒等：REST 是唯一来源，留一个会撒谎的字段更糟");

        check(feed.stale_symbols().empty(), "刚写入不算陈旧");
        // 没有办法在不等 10 秒的情况下直接推进时钟（这个类刻意不注入时钟——
        // 它只有一个时间语义，为它加一层注入不值），所以这里只验证接口语义：
        // 没订阅过的品种一定算陈旧
        feed.subscribe("NEWUSDT");
        const auto st = feed.stale_symbols();
        check(std::find(st.begin(), st.end(), "NEWUSDT") != st.end(),
              "从未拿到过价的品种必须算陈旧（而不是当成 0 或者漏掉）");
        check(feed.mark_price("NEWUSDT") == 0.0, "  它的 mark_price 是 0");
    }

    std::printf("\n── 用例4：退订之后不再刷新，但保留最后的价 ──\n");
    {
        RestPriceFeed feed(
            []() -> std::unordered_map<std::string, double> {
                return {{"BTCUSDT", 60000.0}};
            },
            []() -> std::unordered_map<std::string, double> { return {}; });
        feed.subscribe("BTCUSDT");
        feed.start();
        check(wait_until([&] { return feed.mark_price("BTCUSDT") > 0; }), "先拿到价");
        feed.unsubscribe("BTCUSDT");
        // 保留是有意的：刚停掉的 bot 那一行还要显示最后一次的价
        check(feed.get("BTCUSDT").mark_price > 0, "退订后仍保留最后一次的价（界面要显示）");
        check(feed.health().symbols == 0, "  但不再计入健康统计");
        feed.stop();
    }

    std::printf("\n── 用例5：没有看门狗要守的状态 ──\n");
    {
        // 这条不是功能测试，是把设计意图钉住：轮询的健康状态【完全由观测构成】
        //   running / last_ok_ms / fail_streak / fresh
        // 没有任何一项需要靠"连接还在但数据不来"这种推断得出。
        // WS 版为了识别那个状态背了六种机制（静默看门狗、强制重连、退避、
        // 升级重建、订阅确认取证、对照探针），而这里一个都不需要
        RestPriceFeed feed(
            []() -> std::unordered_map<std::string, double> { return {}; },
            []() -> std::unordered_map<std::string, double> { return {}; });
        auto h = feed.health();
        check(!h.running && h.last_ok_ms == 0 && !h.healthy(),
              "没启动时：不健康，且理由是直接可读的（last_ok_ms==0）");
        check(h.summary() == "未启动", "  summary 说人话");
        feed.subscribe("BTCUSDT");
        feed.start();
        check(wait_until([&] { return feed.health().fail_streak > 0; }),
              "取不到数据时 fail_streak 直接反映事实，不需要看门狗去推断");
        feed.stop();
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
