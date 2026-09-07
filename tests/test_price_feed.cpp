// 价格源的解析与陈旧保护测试。
//
// 存在的理由：这 201 行此前【零测试覆盖】，而它是整个系统的输入端——价格错了，
// 补仓、止盈、止损、三层拦截【同时】失效，而且没有任何一处会报错。
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

// 造一条 combined stream 的 bookTicker 报文
static std::string msg(const char* sym_lower, const char* SYM,
                       const char* b, const char* B, const char* a, const char* A) {
    return std::string("{\"stream\":\"") + sym_lower + "@bookTicker\",\"data\":{"
         + "\"e\":\"bookTicker\",\"u\":123456789,\"s\":\"" + SYM + "\","
         + "\"b\":\"" + b + "\",\"B\":\"" + B + "\","
         + "\"a\":\"" + a + "\",\"A\":\"" + A + "\","
         + "\"T\":1787156044876,\"E\":1787156044880}}";
}

// 标记价流报文（<symbol>@markPrice@1s）。p=标记价，i=指数价，r=资金费率
static std::string mark_msg(const char* sym_lower, const char* SYM, const char* p) {
    return std::string("{\"stream\":\"") + sym_lower + "@markPrice@1s\",\"data\":{"
         + "\"e\":\"markPriceUpdate\",\"E\":1787156044880,\"s\":\"" + SYM + "\","
         + "\"p\":\"" + p + "\",\"i\":\"95010.00\",\"P\":\"95011.00\","
         + "\"r\":\"0.00010000\",\"T\":1787156800000}}";
}

int main() {
    BookTickerStream ts(/*testnet=*/true);

    // ── ① 正常报文解析 ──────────────────────────────────────────────────────
    ts.on_message_for_test(msg("btcusdt", "BTCUSDT",
                               "94310.10", "1.5", "94310.90", "2.5"));
    {
        auto t = ts.get("BTCUSDT");
        check(t.valid, "正常报文解析为有效");
        check_near(t.bid, 94310.10, 1e-6, "  买一");
        check_near(t.ask, 94310.90, 1e-6, "  卖一");
        check_near(t.bid_qty, 1.5, 1e-9, "  买一量");
        check_near(t.ask_qty, 2.5, 1e-9, "  卖一量");
        // 最新价取中间价——不单独订阅 ticker 流，这是唯一的价格来源
        check_near(t.last_price, 94310.50, 1e-6, "  最新价 = 买卖中间价");
        check_near(ts.mid_price("BTCUSDT"), 94310.50, 1e-6, "  mid_price 一致");
    }

    // ── ② 极端量级：meme 币的微价，不能被精度吃掉 ───────────────────────────
    ts.on_message_for_test(msg("pepeusdt", "PEPEUSDT",
                               "0.00000123456", "1e9", "0.00000123556", "2e9"));
    {
        auto t = ts.get("PEPEUSDT");
        check(t.valid, "微价报文有效");
        check_near(t.last_price, 0.00000123506, 1e-15, "  微价中间价保留精度");
    }

    // ── ③ 非法报文一律不得污染缓存 ──────────────────────────────────────────
    // 交易所偶尔会推异常值，或网关返回半截内容。任何一条都不能让已有的
    // 好价格被覆盖成垃圾——引擎拿到 0 会跳过，拿到垃圾会照常决策
    {
        const double before = ts.mid_price("BTCUSDT");
        ts.on_message_for_test("{\"result\":null,\"id\":1}");            // 订阅响应
        ts.on_message_for_test("{\"stream\":\"btcusdt@bookTicker\",\"da"); // 半截
        ts.on_message_for_test("");                                       // 空
        ts.on_message_for_test("不是json");
        check_near(ts.mid_price("BTCUSDT"), before, 1e-9,
                   "订阅响应/半截/空/非JSON 都不覆盖已有价格");
    }

    // ── ④ 买一或卖一为 0 → 必须判为无效，而不是算出一个半价 ─────────────────
    // 中间价 = (bid+ask)/2，若只有一边有效，算出来是真实价格的一半——
    // 那会让引擎认为价格瞬间腰斩，触发深层补仓
    ts.on_message_for_test(msg("ethusdt", "ETHUSDT", "0", "0", "3200.5", "1.0"));
    {
        auto t = ts.get("ETHUSDT");
        check(!t.valid, "买一为 0 → 判为无效");
        check_near(t.last_price, 0.0, 1e-12, "  最新价置 0（而不是 1600.25 这种半价）");
        check_near(ts.mid_price("ETHUSDT"), 0.0, 1e-12, "  mid_price 返回 0，调用方会走REST兜底");
    }

    // ── ⑤ 陈旧保护：这是全套里最重要的一条 ──────────────────────────────────
    // WebSocket 半开时 valid 永远是 true、价格永远是那个冻结值。只有按
    // 「多久没收到新包」判定才拦得住。超时后必须返回 0，让调用方转 REST 标记价
    {
        auto fresh = ts.mid_price("BTCUSDT");
        check(fresh > 0, "刚收到的价格是新鲜的");

        ts.age_cache_for_test("BTCUSDT", 9000);     // 人为把包龄推到 9 秒
        check(ts.mid_price("BTCUSDT") > 0, "9 秒内仍视为新鲜");

        ts.age_cache_for_test("BTCUSDT", 11000);    // 推到 11 秒，超过 10 秒阈值
        check_near(ts.mid_price("BTCUSDT"), 0.0, 1e-12,
                   "超过 10 秒没有新包 → mid_price 返回 0（冻结价不得流入引擎）");
        // 但原始快照仍可读，界面要显示"延迟多少"
        check(ts.get("BTCUSDT").valid, "  get() 仍返回快照（界面据此显示延迟）");
    }

    // ── ⑥ 新包到达后立即恢复新鲜 ────────────────────────────────────────────
    ts.on_message_for_test(msg("btcusdt", "BTCUSDT", "95000.00", "1.0", "95001.00", "1.0"));
    check_near(ts.mid_price("BTCUSDT"), 95000.50, 1e-6, "新包到达 → 立即恢复并更新价格");

    // ── ⑦ 未订阅品种返回 0，不返回垃圾 ──────────────────────────────────────
    check_near(ts.mid_price("NOSUCHUSDT"), 0.0, 1e-12, "未知品种 mid_price 返回 0");
    check(!ts.get("NOSUCHUSDT").valid, "未知品种快照无效");

    // ── ⑧ 标记价：与 bookTicker 是两条流，共用一个缓存条目 ──────────────────
    // 这里最容易写错的是【互相覆盖】。bookTicker 是逐笔、markPrice 每秒一次，
    // 如果 bookTicker 分支用全新 Tick 覆盖缓存，标记价几乎每次都会被抹成 0。
    {
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "95012.34"));
        auto t = ts.get("BTCUSDT");
        check_near(t.mark_price, 95012.34, 1e-6, "标记价报文解析");
        check(t.mark_ms > 0, "  标记价收包时间已记录");
        check_near(t.last_price, 95000.50, 1e-6, "  没有污染中间价");
        check(t.bid > 0 && t.ask > 0, "  没有污染买一卖一");
    }
    {
        // 再来一包 bookTicker：标记价必须还在
        ts.on_message_for_test(msg("btcusdt", "BTCUSDT", "95100.00", "1.0", "95101.00", "1.0"));
        auto t = ts.get("BTCUSDT");
        check_near(t.mark_price, 95012.34, 1e-6,
                   "bookTicker 到达后标记价仍在（这条防的是互相覆盖）");
        check_near(t.last_price, 95100.50, 1e-6, "  中间价已更新");
    }
    {
        // 标记价【不能】刷新行情新鲜度：否则 bookTicker 断流时，
        // 每秒一次的标记价会把包龄一直压在低位，陈旧保护彻底失效
        ts.age_cache_for_test("BTCUSDT", 11000);
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "95020.00"));
        check_near(ts.mid_price("BTCUSDT"), 0.0, 1e-12,
                   "标记价不刷新 recv_ms：bookTicker 断流时陈旧保护仍然生效");
        check_near(ts.get("BTCUSDT").mark_price, 95020.00, 1e-6,
                   "  但标记价本身照常更新（它自己那条流是活的）");
    }
    {
        // 非法标记价不得覆盖已有的好值
        const double before = ts.get("BTCUSDT").mark_price;
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "0"));
        ts.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "-1"));
        check_near(ts.get("BTCUSDT").mark_price, before, 1e-9,
                   "标记价为 0/负数时不覆盖已有值");
    }
    {
        // 微价品种：标记价同样不能被精度吃掉
        ts.on_message_for_test(mark_msg("pepeusdt", "PEPEUSDT", "0.00000123499"));
        check_near(ts.get("PEPEUSDT").mark_price, 0.00000123499, 1e-15,
                   "微价标记价保留精度");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
