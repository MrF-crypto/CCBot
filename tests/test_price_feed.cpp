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

// 成交流报文（<symbol>@aggTrade）。a=聚合成交ID（逐一递增），p=价，q=量
static std::string agg_msg(const char* sym_lower, const char* SYM,
                           int64_t a, const char* p, const char* q) {
    return std::string("{\"stream\":\"") + sym_lower + "@aggTrade\",\"data\":{"
         + "\"e\":\"aggTrade\",\"E\":1787156044880,\"s\":\"" + SYM + "\","
         + "\"a\":" + std::to_string(a) + ",\"p\":\"" + p + "\",\"q\":\"" + q + "\","
         + "\"f\":100,\"l\":105,\"T\":1787156044870,\"m\":true}}";
}

// 盘口报文（<symbol>@bookTicker）。u=updateId（单调但不逐一递增），
// b/B=买一价/量，a/A=卖一价/量。
// ⚠ 这里的 "a" 是卖一【价】，而 aggTrade 的 "a" 是成交【ID】——同名不同义
static std::string book_msg(const char* sym_lower, const char* SYM, int64_t u,
                            const char* b, const char* B,
                            const char* a, const char* A) {
    return std::string("{\"stream\":\"") + sym_lower + "@bookTicker\",\"data\":{"
         + "\"e\":\"bookTicker\",\"u\":" + std::to_string(u) + ",\"E\":1787156044880,"
         + "\"T\":1787156044870,\"s\":\"" + SYM + "\","
         + "\"b\":\"" + b + "\",\"B\":\"" + B + "\","
         + "\"a\":\"" + a + "\",\"A\":\"" + A + "\"}}";
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

        // 首包会发一条"订阅生效"，这是有意的：它把"没连上/订阅没生效/数据在流"
        // 三种情况区分开——此前三者在日志里长得一模一样（都是什么都没有）
        ts3.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "100.0"));
        check(got.size() == 2 && got[1].find("首个数据包") != std::string::npos,
              "  首个数据包提示订阅生效");
        ts3.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "101.0"));
        ts3.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "102.0"));
        check(got.size() == 2, "  后续数据包不再重复提示（只报一次）");
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

    // ── ⑬ 静默看门狗的判定 ──────────────────────────────────────────────────
    // 这是本轮新增里最要紧的一条。此前 WS 半开之后【永远不会自己恢复】：
    // 陈旧保护让引擎拿到 0（安全），调用方转 REST 兜底拿到价（可用），
    // 于是 headless 的 stall 计数被清零（不告警）——三件事叠起来是
    // "永久降级且完全不可见"，而且没有任何东西会把那条死连接掀掉。
    //
    // 判定抽成纯函数就是为了在这里钉住：两个方向的误判都不报错。
    // 判太松 → 回到上面那个问题；判太紧 → 正常连接被反复打断，
    // 每次重连都要全量重订，反而更不稳。
    {
        using B = BookTickerStream;
        const int64_t T = 1000000;   // 任意基准，纯函数只看差值

        check(!B::should_kick(T + 99999, T, T, /*connected=*/false, 4),
              "未连接时不掀：重连是库的事，也没有东西可掀");
        check(!B::should_kick(T + 99999, T, T, true, /*stream_n=*/0),
              "一条流都没订时不掀：本来就不该有数据，空闲不是故障");
        check(!B::should_kick(T + 99999, 0, /*conn_since=*/0, true, 4),
              "连接时刻未知（刚构造/刚被掀过）时不掀，等下一轮");

        check(!B::should_kick(T + 5000, /*last_msg=*/0, T, true, 4),
              "刚连上 5 秒还没收到包 → 不掀（从连接建立起算，不是从 0 起算）");
        check(B::should_kick(T + 21000, /*last_msg=*/0, T, true, 4),
              "连上 21 秒一个包都没有 → 掀（订阅没生效的连接不该留着）");

        check(!B::should_kick(T + 19000, T + 5000, T, true, 4),
              "收过包且包龄 14 秒 → 不掀");
        check(B::should_kick(T + 31000, T + 5000, T, true, 4),
              "收过包但包龄 26 秒 → 掀（这就是半开连接的样子）");

        check(!B::should_kick(T + B::kSilenceMs, T, T, true, 4),
              "边界：正好 20 秒不掀（用 > 而不是 >=）");
        check(B::should_kick(T + B::kSilenceMs + 1, T, T, true, 4),
              "边界：超出 1ms 就掀");

        // kSilenceMs 必须比 kStaleMs 宽。两者判的不是一回事：
        // kStaleMs 是"某一个品种的价格旧了"，可能只是那条流抖了一下；
        // kSilenceMs 是"所有品种一个包都没有"。反过来的话，单品种抖动会
        // 触发整条连接重连，而重连要全量重订，代价大得多
        check(B::kSilenceMs > B::kStaleMs,
              "kSilenceMs 必须严格大于 kStaleMs（否则单品种抖动会掀整条连接）");
    }

    // ── ⑭ "连上了但订阅没生效"的提示 ────────────────────────────────────────
    // 和上一条分开是因为这两种故障的【修法完全不同】：没连上要查网络，
    // 连上了没数据要查订阅（流名拼错/超上限/被服务端拒）。
    // 混成一条提示的话，看到的人还是不知道该查哪边
    {
        using B = BookTickerStream;
        const int64_t T = 1000000;

        check(!B::should_warn_no_data(T + 99999, T + 100, T, true, 4),
              "本次连接收到过数据 → 订阅是生效的，不提示");
        check(B::should_warn_no_data(T + 20000, /*last_msg=*/T - 5000, T, true, 4),
              "上一次连接留下的包不算本次订阅生效（重连是最容易丢订阅的时刻）");
        check(!B::should_warn_no_data(T + 9000, 0, T, true, 4),
              "宽限期内（9 秒）不提示：订阅生效本身要一点时间");
        check(B::should_warn_no_data(T + 11000, 0, T, true, 4),
              "连上 11 秒还没有任何数据 → 提示订阅可能没生效");
        check(!B::should_warn_no_data(T + 99999, 0, T, false, 4),
              "未连接时不提示订阅问题（那是连接问题，修法不同）");
        check(!B::should_warn_no_data(T + 99999, 0, T, true, 0),
              "没订阅任何流时不提示");
    }

    // ── ⑮ 健康快照 ──────────────────────────────────────────────────────────
    // 存在的理由和看门狗同源：此前判断"行情链路好不好"只有 is_connected()，
    // 而它在半开时照样返回 true。现在把"引擎还能不能拿到新鲜价格"摊开成数字
    {
        BookTickerStream ts5(/*testnet=*/true);

        auto h0 = ts5.health();
        check(!h0.connected && h0.symbols == 0 && h0.streams == 0, "初始 Health 为空");
        check(!h0.healthy(), "  未连接 → 不健康（连接没起来就是不可用）");

        ts5.subscribe("BTCUSDT");
        ts5.subscribe("ETHUSDT");
        ts5.subscribe("SOLUSDT");
        auto h1 = ts5.health();
        check(h1.symbols == 3 && h1.streams == 6, "3 个品种 = 6 条流");
        check(h1.never == 3 && h1.fresh == 0 && h1.stale == 0,
              "  订阅了但一包未到 → 3 个记为无数据（而不是悄悄算作正常）");

        ts5.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "100.0"));
        ts5.on_message_for_test(mark_msg("ethusdt", "ETHUSDT", "200.0"));
        auto h2 = ts5.health();
        check(h2.fresh == 2 && h2.stale == 0 && h2.never == 1, "两个新鲜、一个仍无数据");
        check(h2.silence_ms >= 0, "  收包时间已记录");

        ts5.age_mark_for_test("ETHUSDT", 11000);
        auto h3 = ts5.health();
        check(h3.fresh == 1 && h3.stale == 1 && h3.never == 1, "ETH 超 10 秒 → 计入陈旧");
        {
            auto ss = ts5.stale_symbols();
            check(ss.size() == 1 && ss[0] == "ETHUSDT",
                  "  stale_symbols 指名道姓（告警正文里\"哪个断了\"比\"几个断了\"可查得多）");
        }

        // 这一条是整个 Health 设计的支点。
        // REST 兜底成功【不是】WS 活着的证据。若把它算进新鲜度，链路死了 Health
        // 依旧全绿，新加的看门狗和这个快照就同时白做了——正好回到要修的那个
        // "永久降级且不可见"
        ts5.set_mark_price("ETHUSDT", 201.0);
        auto h4 = ts5.health();
        check(h4.stale == 1 && h4.fresh == 1,
              "REST 兜底写回【不计入】Health 新鲜度（否则 WS 死了 Health 还全绿）");
        check(ts5.mark_price("ETHUSDT") > 0,
              "  但引擎照常拿到兜底价——\"能不能交易\"与\"链路好不好\"是两件事");

        ts5.set_mark_price("SOLUSDT", 50.0);
        check(ts5.health().never == 1,
              "  从未收到过 WS 包的品种，REST 写回后仍记为无数据");
    }

    // ── ⑯ 订阅合并队列 ──────────────────────────────────────────────────────
    // 币安合约 WS 限制【每秒最多 10 条入站消息】，超了直接断连。
    // send_subs 原本只合并"一个品种的两条流"，而界面上连续添加 15 个品种
    // 就是 15 条消息在几十毫秒内发出去——照样触线，而注释却写着已经防住了。
    // 现在订阅不即时发送，先入队，由 pump 线程按周期合并成一条发出
    {
        BookTickerStream ts6(/*testnet=*/true);
        check(ts6.pending_ctl_for_test() == 0, "初始无待发控制消息");

        ts6.subscribe("BTCUSDT");
        check(ts6.pending_ctl_for_test() == 2, "订阅先入队，不即时发送");
        ts6.subscribe("BTCUSDT");
        check(ts6.pending_ctl_for_test() == 2, "  重复订阅不重复入队");

        ts6.unsubscribe("BTCUSDT");
        check(ts6.pending_ctl_for_test() == 2 && ts6.stream_count_for_test() == 0,
              "还没发出就退订 → 转入退订队列，不会先发订阅再发退订");

        ts6.subscribe("BTCUSDT");
        check(ts6.pending_ctl_for_test() == 2 && ts6.stream_count_for_test() == 2,
              "退订后立即重订 → 回到订阅队列，两个队列不会同时含同一条流");
    }

    // ── ⑰ 挑连接的纯函数 ────────────────────────────────────────────────────
    // 此前没有这一步：超过币安单连接 200 条流的部分【静默】收不到行情，
    // 没有任何地方报错。现在超出就多开一条连接
    {
        using B = BookTickerStream;
        check(B::pick_conn({}, 2, 200) == 0,
              "还没有任何连接 → 返回 size()=0，即要新建第一条");
        check(B::pick_conn({0}, 2, 200) == 0, "空连接装得下");
        check(B::pick_conn({198}, 2, 200) == 0, "正好装满也算装得下（198+2=200）");
        check(B::pick_conn({199}, 2, 200) == 1, "装不下 → 返回 size()，要新建");
        check(B::pick_conn({200, 10}, 2, 200) == 1, "跳过满的那条，用第二条");
        check(B::pick_conn({200, 200}, 2, 200) == 2, "都满了 → 新建第三条");
        check(B::pick_conn({5}, 0, 200) == 1, "没有新流要加时不占用任何连接");
        // 刻意用首次适配而不是"最空优先"：品种通常批量加入，首次适配把它们
        // 紧密排在前面的连接上，连接数最少。均摊会让每条连接半满、白白多开，
        // 而每条连接都是一次握手、一份心跳、一个重连时的建连频次配额
        check(B::pick_conn({10, 0}, 2, 200) == 0,
              "首次适配而非最空优先：仍然填第一条（连接数最少）");
    }

    // ── ⑱ 分片：超过一条连接的容量就自动多开 ────────────────────────────────
    {
        BookTickerStream ts7(/*testnet=*/true);
        std::vector<std::string> notes;
        ts7.on_server_msg([&](const std::string& m) {
            if (m.find("新开第") != std::string::npos) notes.push_back(m);
        });

        for (int i = 0; i < 100; ++i)
            ts7.subscribe("SYM" + std::to_string(i) + "USDT");
        check(ts7.stream_count_for_test() == 200, "100 个品种 × 2 条流 = 200，正好一条连接");
        check(ts7.conn_count_for_test() == 1, "  只用了一条连接");
        check(notes.empty(), "  第一条连接不值得特意说一声");

        ts7.subscribe("ONEMOREUSDT");
        check(ts7.conn_count_for_test() == 2, "第 101 个品种 → 自动新开第二条连接");
        check(notes.size() == 1, "  并且说一声（解释容量是怎么长上去的）");
        {
            auto loads = ts7.conn_loads_for_test();
            check(loads.size() == 2 && loads[0] == 200 && loads[1] == 2,
                  "  负载 [200, 2]：首次适配，不均摊");
        }
        ts7.mark_connected_for_test(true);
        check(!ts7.health().over_cap,
              "不再有 over_cap —— 这正是分片要修的：此前超出的流静默收不到行情");
        check(ts7.health().conns == 2 && ts7.health().conns_up == 2, "Health 报出连接数");
    }

    // ── ⑲ Feeds：默认两条流，开了才有成交价与盘口 ───────────────────────────
    // 后两条默认关闭是有意的：开了就是每品种 4 条流，单连接能装的品种数直接
    // 砍半，而目前它们还没有消费者（趋势SAR 用的是标记价）
    {
        BookTickerStream::Feeds f;
        check(f.per_symbol() == 2, "默认只订两条流（与改造前一致，SAR 行为不变）");
        f.agg_trade = true;
        check(f.per_symbol() == 3, "开成交流 → 3 条");
        f.book_ticker = true;
        check(f.per_symbol() == 4, "再开盘口 → 4 条");

        BookTickerStream ts10(/*testnet=*/true, f);
        ts10.subscribe("BTCUSDT");
        check(ts10.stream_count_for_test() == 4, "订阅一个品种 = 4 条流");
        auto loads = ts10.conn_loads_for_test();
        check(loads.size() == 1 && loads[0] == 4,
              "一个品种的 4 条流全在同一条连接上——分开的话标记价可能在死连接上、"
              "成交价在活连接上，任何对齐都失去意义");
    }

    // ── ⑳ 成交流（@aggTrade）：三个价是三个口径，以及丢包检测 ───────────────
    {
        BookTickerStream::Feeds f; f.agg_trade = true; f.book_ticker = true;
        BookTickerStream ts8(/*testnet=*/true, f);
        ts8.subscribe("BTCUSDT");

        ts8.on_message_for_test(agg_msg("btcusdt", "BTCUSDT", 1000, "94000.5", "0.25"));
        {
            auto t = ts8.get("BTCUSDT");
            check_near(t.last_px,  94000.5, 1e-6, "成交价解析");
            check_near(t.last_qty, 0.25,    1e-9, "  成交量解析");
            check(t.agg_id == 1000 && t.agg_gaps == 0, "  成交ID记录，无断层");
        }

        // 三个价互不覆盖。混用会出事：拿标记价当成交价，回测赚的钱实盘会被
        // 点差吃掉；拿成交价算强平距离，插针时会误判
        ts8.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "94010.0"));
        {
            auto t = ts8.get("BTCUSDT");
            check_near(t.last_px,    94000.5, 1e-6, "标记价到达不覆盖成交价（两个口径）");
            check_near(t.mark_price, 94010.0, 1e-6, "  标记价自己也对");
        }

        // 聚合成交ID 逐一递增，所以断层就是丢包的铁证——这是唯一能证明
        // "WebSocket 悄悄漏了消息"的东西，别的地方查不出来
        ts8.on_message_for_test(agg_msg("btcusdt", "BTCUSDT", 1001, "94001.0", "0.1"));
        check(ts8.get("BTCUSDT").agg_gaps == 0, "ID 连续 → 不计断层");
        ts8.on_message_for_test(agg_msg("btcusdt", "BTCUSDT", 1005, "94002.0", "0.1"));
        check(ts8.get("BTCUSDT").agg_gaps == 1, "ID 跳号 → 计一次断层（确证丢包）");
        check(ts8.health().gaps == 1, "  Health 汇总断层数");

        ts8.on_message_for_test(agg_msg("btcusdt", "BTCUSDT", 1003, "1.0", "0.1"));
        check_near(ts8.get("BTCUSDT").last_px, 94002.0, 1e-6,
                   "乱序旧包被丢弃，不把更新的价盖回去（\"价格突然跳回去\"那类幽灵bug）");
        check(ts8.get("BTCUSDT").agg_gaps == 1, "  旧包不计断层");

        // ── 盘口（@bookTicker）────────────────────────────────────────────
        ts8.on_message_for_test(
            book_msg("btcusdt", "BTCUSDT", 500, "93999.0", "3.0", "94001.0", "5.0"));
        {
            double bid = 0, ask = 0;
            check(ts8.best_bid_ask("BTCUSDT", 5000, bid, ask), "盘口可读");
            check_near(bid, 93999.0, 1e-6, "  买一");
            check_near(ask, 94001.0, 1e-6, "  卖一");
            check_near(ts8.get("BTCUSDT").bid_qty, 3.0, 1e-9, "  买一量");
        }
        ts8.on_message_for_test(
            book_msg("btcusdt", "BTCUSDT", 501, "94005.0", "1.0", "94000.0", "1.0"));
        check_near(ts8.get("BTCUSDT").bid, 93999.0, 1e-6,
                   "买一≥卖一的交叉盘口被丢弃（收下它滑点会算出负数）");
        check(ts8.get("BTCUSDT").book_id == 500, "  被丢的包没有推进 updateId");

        ts8.on_message_for_test(
            book_msg("btcusdt", "BTCUSDT", 499, "93000.0", "1.0", "93001.0", "1.0"));
        check_near(ts8.get("BTCUSDT").bid, 93999.0, 1e-6, "updateId 倒退的旧包被丢弃");
        check(ts8.get("BTCUSDT").book_back == 1, "  计一次倒退");
        // book_id 只能检出倒退、检不出丢包（它单调递增但不逐一递增），
        // 和 agg_id 的语义不同，两者的计数不能混着读
        check(ts8.get("BTCUSDT").agg_gaps == 1, "  盘口倒退不混进成交断层计数");

        // ── 陈旧容忍度必须由调用方给 ──────────────────────────────────────
        ts8.age_trade_for_test("BTCUSDT", 5000);
        double px = 0;
        check(!ts8.last_trade("BTCUSDT", 1000, px),
              "成交价 5 秒前、调用方只给 1 秒容忍 → 不给（短线要的就是这个严格度）");
        check(ts8.last_trade("BTCUSDT", 10000, px),
              "  同一笔数据给 10 秒容忍 → 给（阈值该由调用方定，差三个数量级）");
        check(ts8.last_trade("BTCUSDT", -1, px), "  负数 = 不限年龄（界面显示用）");

        ts8.mark_connected_for_test(true);
        ts8.on_message_for_test(tick24_msg("btcusdt", "BTCUSDT", "3.0"));
        check(ts8.health().healthy(),
              "成交流沉默 5 秒不影响链路健康：冷门币几分钟不成交是正常的，"
              "\"流断了吗\"只能由每秒一包的 markPrice 回答");
    }

    // ── ㉑ 涨幅流单独死掉 ───────────────────────────────────────────────────
    // 它和标记价在同一条连接上，但币安可以只丢掉其中一条订阅。此前这种劣化
    // 完全看不见：markPrice 全绿 → Health 全绿 → 而高位拦截的唯一数据来源
    // 已经悄悄失效
    {
        BookTickerStream ts9(/*testnet=*/true);
        ts9.subscribe("BTCUSDT");
        ts9.on_message_for_test(mark_msg("btcusdt", "BTCUSDT", "100.0"));
        ts9.on_message_for_test(tick24_msg("btcusdt", "BTCUSDT", "5.0"));
        ts9.mark_connected_for_test(true);
        check(ts9.health().healthy(), "两条流都活着 → 健康");

        ts9.age_chg_for_test("BTCUSDT", 70000);
        const auto h = ts9.health();
        check(h.fresh == 1,     "标记价仍然新鲜");
        check(h.chg_stale == 1, "  但涨幅流已陈旧，单独统计出来");
        check(!h.healthy(),
              "  涨幅流失效就算不健康：高位拦截没有第二个数据来源");
        check(h.summary().find("涨幅异常") != std::string::npos, "  summary 里点出来");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
