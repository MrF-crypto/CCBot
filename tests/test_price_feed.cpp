// 价格源的解析与陈旧保护测试。
//
// 存在的理由：它是整个系统的输入端——价格错了，补仓、止盈、止损、三层拦截
// 【同时】失效，而且没有任何一处会报错。
//
// 最凶险的不是"价格算错"，是 WebSocket **半开**：连接还在、数据早就不来了，
// 缓存里躺着一个冻结的价格，看起来完全正常。引擎会拿着这个僵尸价格继续做决策，
// 在真实行情暴跌时完全失明。所以陈旧保护是这套测试的重点。
//
// 这里只测【纯函数部分】：报文解析与陈旧判定。建连接、重连、订阅那部分依赖真实
// 网络，不适合放进单元测试——但那部分出错会立刻表现为"完全没有价格"，
// 是显性故障；而解析和陈旧判定出错是隐性的，正是需要断言守住的部分。
#include "net/book_ticker_stream.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}
static void check_near(double got, double want, double tol, const std::string& what) {
    bool ok = std::fabs(got - want) <= tol;
    if (ok) std::printf("[ OK ]  %s\n", what.c_str());
    else    { std::printf("[FAIL]  %s  期望 %.10g，实际 %.10g\n", what.c_str(), want, got); ++g_fail; }
}

// 标记价流报文（<symbol>@markPrice@1s）。p=标记价，i=指数价，r=资金费率
static std::string mark_msg(const char* sym_lower, const char* SYM, const char* p) {
    return std::string("{\"stream\":\"") + sym_lower + "@markPrice@1s\",\"data\":{"
         + "\"e\":\"markPriceUpdate\",\"E\":1787156044880,\"s\":\"" + SYM + "\","
         + "\"p\":\"" + p + "\",\"i\":\"95010.00\",\"P\":\"95011.00\","
         + "\"r\":\"0.00010000\",\"T\":1787156800000}}";
}

// 24h 行情流报文（<symbol>@ticker）。P=24h滚动涨幅%，c=最新成交价
static std::string tick24_msg(const char* sym_lower, const char* SYM, const char* P) {
    return std::string("{\"stream\":\"") + sym_lower + "@ticker\",\"data\":{"
         + "\"e\":\"24hrTicker\",\"E\":1787156044880,\"s\":\"" + SYM + "\","
         + "\"p\":\"120.5\",\"P\":\"" + P + "\",\"c\":\"95000.00\","
         + "\"o\":\"94000.00\",\"h\":\"96000.00\",\"l\":\"93000.00\","
         + "\"v\":\"12345.6\",\"q\":\"1170000000\"}}";
}

int main() {
    BookTickerStream ts(/*testnet=*/true);

    // ── ① 标记价报文解析 ────────────────────────────────────────────────────
    ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "94310.50"));
    {
        auto t = ts.get("BTCUSDT");
        check_near(t.mark_price, 94310.50, 1e-6, "标记价解析");
        check(t.mark_ms > 0, "  收包时间已记录");
        check_near(ts.mark_price("BTCUSDT"), 94310.50, 1e-6, "  mark_price() 一致");
    }

    // ── ② 极端量级：meme 币的微价，不能被精度吃掉 ───────────────────────────
    ts.on_message_for_test(mark_msg("pepeusdt", "PEPEUSDT", "0.00000123456"));
    check_near(ts.get("PEPEUSDT").mark_price, 0.00000123456, 1e-15, "微价标记价保留精度");

    // ── ③ 非法报文一律不得污染缓存 ──────────────────────────────────────────
    // 交易所偶尔会推异常值，或网关返回半截内容。任何一条都不能让已有的
    // 好价格被覆盖成垃圾——引擎拿到 0 会跳过，拿到垃圾会照常决策
    {
        const double before = ts.mark_price("BTCUSDT");
        ts.on_message_for_test("{\"result\":null,\"id\":1}");                 // 订阅响应
        ts.on_message_for_test("{\"stream\":\"btcusdt@markPrice@1s\",\"da");  // 半截
        ts.on_message_for_test("");                                            // 空
        ts.on_message_for_test("不是json");
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "0"));           // 零价
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "-1"));          // 负价
        check_near(ts.mark_price("BTCUSDT"), before, 1e-9,
                   "订阅响应/半截/空/非JSON/零价/负价 都不覆盖已有价格");
    }

    // ── ④ 陈旧保护：这是全套里最重要的一条 ──────────────────────────────────
    // WebSocket 半开时价格永远是那个冻结值。只有按「多久没收到新包」判定才拦得住。
    // 超时后必须返回 0，让调用方转 REST 兜底
    {
        check(ts.mark_price("BTCUSDT") > 0, "刚收到的价格是新鲜的");

        ts.age_mark_for_test("BTCUSDT", 9000);      // 人为把包龄推到 9 秒
        check(ts.mark_price("BTCUSDT") > 0, "9 秒内仍视为新鲜");

        ts.age_mark_for_test("BTCUSDT", 11000);     // 推到 11 秒，超过 10 秒阈值
        check_near(ts.mark_price("BTCUSDT"), 0.0, 1e-12,
                   "超过 10 秒没有新包 → mark_price 返回 0（冻结价不得流入引擎）");
        // 但原始快照仍可读，界面要显示"延迟多少"
        check(ts.get("BTCUSDT").mark_price > 0, "  get() 仍返回快照（界面据此显示延迟）");
    }

    // ── ⑤ 新包到达后立即恢复新鲜 ────────────────────────────────────────────
    ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "95000.50"));
    check_near(ts.mark_price("BTCUSDT"), 95000.50, 1e-6, "新包到达 → 立即恢复并更新价格");

    // ── ⑥ 未订阅品种返回 0，不返回垃圾 ──────────────────────────────────────
    check_near(ts.mark_price("NOSUCHUSDT"), 0.0, 1e-12, "未知品种 mark_price 返回 0");
    check_near(ts.get("NOSUCHUSDT").mark_price, 0.0, 1e-12, "未知品种快照为空");

    // ── ⑦ 两条流共用一个缓存条目，互不覆盖 ──────────────────────────────────
    // 这里最容易写错的是【互相覆盖】：若某条流的分支用全新 Tick 覆盖缓存，
    // 另一条流的数据会被抹成 0
    {
        ts.on_message_for_test(tick24_msg("btcusdt", "BTCUSDT", "12.345"));
        auto t = ts.get("BTCUSDT");
        check_near(t.chg_24h, 12.345, 1e-9, "24h 涨幅解析");
        check_near(t.mark_price, 95000.50, 1e-6, "  没有污染标记价");

        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "95111.00"));
        auto t2 = ts.get("BTCUSDT");
        check_near(t2.mark_price, 95111.00, 1e-6, "标记价已更新");
        check_near(t2.chg_24h, 12.345, 1e-9, "  24h 涨幅仍在（防的是互相覆盖）");
    }

    // ── ⑧ 涨幅流【不能】刷新标记价的新鲜度 ──────────────────────────────────
    // 否则 markPrice 断流时，每秒一次的涨幅流会把包龄一直压在低位，
    // 陈旧保护被"续命"，引擎继续用冻结价——正是这套测试最初要防的场景
    {
        ts.age_mark_for_test("BTCUSDT", 11000);
        ts.on_message_for_test(tick24_msg("btcusdt", "BTCUSDT", "13.0"));
        check_near(ts.mark_price("BTCUSDT"), 0.0, 1e-12,
                   "涨幅流不刷新 mark_ms：markPrice 断流时陈旧保护仍然生效");
        double pct = 0;
        check(ts.change_24h("BTCUSDT", pct) && std::fabs(pct - 13.0) < 1e-9,
              "  但涨幅本身照常更新（它自己那条流是活的）");
    }

    // ── ⑨ 涨幅的合法值包含 0 和负数 ─────────────────────────────────────────
    // 涨幅不像价格，不能用返回值 0 表示"无数据"——做空镜像要用负值
    {
        double pct = 0;
        check(!ts.change_24h("NOSUCHUSDT", pct), "未收到 @ticker 时 change_24h 返回 false");

        ts.on_message_for_test(tick24_msg("ethusdt", "ETHUSDT", "-8.75"));
        check(ts.change_24h("ETHUSDT", pct) && std::fabs(pct + 8.75) < 1e-9,
              "负涨幅（跌幅）正确接收——做空镜像要用");

        ts.on_message_for_test(tick24_msg("ethusdt", "ETHUSDT", "0"));
        check(ts.change_24h("ETHUSDT", pct) && std::fabs(pct) < 1e-12,
              "涨幅恰好为 0 仍算有效（不能用 >0 判合法）");
    }

    // ── ⑩ REST 兜底的标记价必须写得回缓存 ───────────────────────────────────
    // v4.0.9 的实际故障：markPrice 流没数据时，调用方转 REST 把价格喂给了引擎，
    // 但没写回缓存，而界面读的正是缓存——于是"策略照常跑、标记价列一直空着"。
    // 引擎有兜底、界面没有，这种半边修复比完全没修更难查
    {
        BookTickerStream ts2(/*testnet=*/true);
        check_near(ts2.mark_price("BTCUSDT"), 0.0, 1e-12, "初始无标记价");
        ts2.set_mark_price("BTCUSDT", 95123.45);
        check_near(ts2.mark_price("BTCUSDT"), 95123.45, 1e-6,
                   "REST 写回后 mark_price() 可读（引擎路径）");
        check_near(ts2.get("BTCUSDT").mark_price, 95123.45, 1e-6,
                   "  get() 也可读（界面路径，v4.0.9 漏的就是这条）");
        ts2.set_mark_price("BTCUSDT", 0);
        check_near(ts2.get("BTCUSDT").mark_price, 95123.45, 1e-6,
                   "  写回 0 不覆盖已有值");
    }

    // ── ⑪ 服务端非数据消息必须能被上报 ──────────────────────────────────────
    // 订阅被拒返回 {"code":...,"msg":...}，没有 "stream" 字段，v4.0.9 之前被
    // 消息入口第一行直接丢弃——某条流没订上时界面只是空白，没有任何线索
    {
        BookTickerStream ts3(/*testnet=*/true);
        std::vector<std::string> got;
        ts3.on_server_msg([&](const std::string& m) { got.push_back(m); });

        ts3.on_message_for_test("{\"result\":null,\"id\":1}");
        check(got.empty(), "正常订阅确认不打扰用户");

        ts3.on_message_for_test("{\"code\":2,\"msg\":\"Invalid request: invalid stream\"}");
        check(got.size() == 1 && got[0].find("Invalid request") != std::string::npos,
              "订阅被拒会上报（v4.0.9 查不出根因的直接原因）");

        ts3.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "100.0"));
        check(got.size() == 1, "  数据包不触发服务端消息回调");
    }

    // ── ⑫ 订阅生命周期：退订必须真的收缩 ────────────────────────────────────
    // unsubscribe() 在 v4.0.10 之前【从未被调用】，streams_ 只增不减，
    // 重连时全量重订，反复增删品种会一路累积到币安合约 200 条流的上限，
    // 超出后静默失效。每品种 2 条流
    {
        BookTickerStream ts4(/*testnet=*/true);
        check(ts4.stream_count_for_test() == 0, "初始无订阅");
        ts4.subscribe("BTCUSDT");
        check(ts4.stream_count_for_test() == 2, "订阅一个品种 = 2 条流");
        ts4.subscribe("BTCUSDT");
        check(ts4.stream_count_for_test() == 2, "  重复订阅不增加");
        ts4.subscribe("ETHUSDT");
        check(ts4.stream_count_for_test() == 4, "第二个品种 → 4 条流");
        ts4.unsubscribe("BTCUSDT");
        check(ts4.stream_count_for_test() == 2, "退订后真的收缩（这条防的是流泄漏）");
        ts4.unsubscribe("NOSUCHUSDT");
        check(ts4.stream_count_for_test() == 2, "  退订未订阅的品种是安全的 no-op");
        ts4.unsubscribe("ETHUSDT");
        check(ts4.stream_count_for_test() == 0, "全部退订后归零");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
