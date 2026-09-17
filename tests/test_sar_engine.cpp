// SAR 引擎订单路径测试。
//
// 决策逻辑在 test_sar.cpp 里已经穷举过，这里只测"决策变成订单"这一段——
// 也就是只有和交易所打交道才会遇到的那些脏事：零成交、部分成交、状态不明、
// reduceOnly 被拒、取整后归零。它们平时永不执行，出问题的那一刻却最要命。
//
// 重点是【反手方案A】的安全性：平仓没成功就绝不能开反向仓，否则原仓位还在、
// 又叠一个反向仓，净敞口翻倍且方向不明。这是方案A唯一的致命失败模式。
#include "core/sar_engine.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// ── 假交易所 ────────────────────────────────────────────────────────────────
class FakeClient : public ITradingClient {
public:
    struct Call { std::string side; double qty; bool reduce_only; };
    std::vector<Call> calls;

    double fill_price   = 100.0;
    bool   fail_next    = false;    // 下一笔直接失败
    bool   uncertain_next = false;  // 下一笔状态不明
    bool   zero_fill_next = false;  // 下一笔零成交（r.ok 但 executedQty=0）
    std::string error_next;         // 自定义错误串（如 -2022）
    double qty_step     = 0.001;    // 取整步长
    int    leverage_calls = 0;
    bool   leverage_ok  = true;

    OrderOutcome place_market_order(const std::string&, const std::string& side,
                                    double qty, bool reduce_only) override {
        calls.push_back({side, qty, reduce_only});
        OrderOutcome o;
        if (fail_next || uncertain_next) {
            o.ok = false;
            o.uncertain = uncertain_next;
            o.error = error_next.empty() ? "boom" : error_next;
            fail_next = uncertain_next = false;
            error_next.clear();
            return o;
        }
        o.ok = true;
        o.order_id = "M" + std::to_string(calls.size());
        o.avg_price = fill_price;
        o.executed_qty = zero_fill_next ? 0.0 : qty;
        zero_fill_next = false;
        return o;
    }
    double round_qty(const std::string&, double q) override {
        if (qty_step <= 0) return q;
        return std::floor(q / qty_step) * qty_step;
    }
    bool set_leverage(const std::string&, int) override {
        ++leverage_calls;
        return leverage_ok;
    }
    bool is_dual_mode() const override { return false; }
};

// 内联执行器 + 固定时钟，结果完全确定可复现
static std::chrono::steady_clock::time_point g_now{};

static EngineHost inline_host() {
    EngineHost h;
    h.submit = [](std::function<void()> fn) { fn(); };
    h.now_steady = [] { return g_now; };
    return h;
}
static void advance(int secs) { g_now += std::chrono::seconds(secs); }

static SarConfig mk_cfg(const std::string& sym = "TESTUSDT") {
    SarConfig c;
    c.symbol      = sym;
    c.budget_usdt = 1000;
    c.leverage    = 3;
    c.rule.atr_mult = 3.0;
    c.rule.reverse_needs_signal = false;   // 多数用例要确定性地触发反手
    return c;
}

// 喂一份新鲜信号：通道 [dn,up]，ATR=atr
static void feed(SarEngine& e, const std::string& id, double atr,
                 double up, double dn, int64_t bar = 1) {
    e.update_signal(id, atr, atr, true, up, dn, bar);
}

int main() {
    // ── 开仓：突破 → 市价单 → 止损线布在 成交价 - k*ATR ────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        check(!id.empty(), "add_bot 应返回 bot_id");
        check(eng.add_bot(mk_cfg()).empty(), "同品种重复添加应被拒");

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        check(cli->calls.size() == 1, "应只发一笔开仓单");
        check(cli->calls[0].side == "BUY" && !cli->calls[0].reduce_only,
              "上破应发 BUY 非 reduceOnly");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Long, "应记为持多");
        check(std::fabs(b.st.stop - 104.0) < 1e-6, "止损线应为 110-3*2=104");
        check(b.qty > 0, "应记录成交数量");
        check(cli->leverage_calls == 1, "开仓前应设置一次杠杆");
    }

    // ── 零成交防幽灵仓：r.ok 但 executedQty=0，绝不能入账 ────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->zero_fill_next = true;
        eng.tick("TESTUSDT", 110.0);

        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Flat, "零成交不得记为持仓");
        check(b.qty == 0, "零成交不得入账数量");
        check(!b.pending, "零成交后 pending 必须复位，否则该bot永久冻结");
    }

    // ── 开仓状态不明 → 停止该bot（开仓不幂等，盲目重试会双倍仓位）──────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->uncertain_next = true;
        eng.tick("TESTUSDT", 110.0);

        auto b = eng.get_bots()[0];
        check(b.state == SarBot::State::Stopped, "开仓状态不明应停止该bot");
        check(!b.pending, "停止时 pending 也必须复位");
    }

    // ── ATR 缺失不开仓（没有ATR就没有止损线）────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        eng.update_signal(id, 0, 0, true, 110, 90, 1);   // atr=0
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "ATR 缺失时不应下单");
    }

    // ── 盈利出场：平仓单为 reduceOnly，且不反手 ──────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);            // 开多 @110，线 104

        eng.tick("TESTUSDT", 130.0);            // 新高 → 线 124
        check(std::fabs(eng.get_bots()[0].st.stop - 124.0) < 1e-6, "线应上移到 124");

        cli->fill_price = 124.0;
        eng.tick("TESTUSDT", 124.0);            // 回撤触线，124 > 110 成本 → 盈利出场

        check(cli->calls.size() == 2, "应只有开仓+平仓两笔");
        check(cli->calls[1].side == "SELL" && cli->calls[1].reduce_only,
              "平多应发 reduceOnly SELL");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Flat, "平仓后应为空仓");
        check(b.trade_count == 1 && b.win_count == 1, "应记一笔盈利成交");
        check(b.realized_pnl > 0, "已实现盈亏应为正");
        check(!b.pending, "盈利出场后 pending 必须复位");
    }

    // ── 亏损出场 + 反手：两笔单，顺序正确 ───────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);            // 开多 @110，线 104

        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);            // 触线，104 < 110 → 亏损 → 反手

        check(cli->calls.size() == 3, "应为 开多 + 平多 + 开空 三笔");
        check(cli->calls[1].reduce_only && cli->calls[1].side == "SELL",
              "第2笔应是 reduceOnly 平多");
        check(!cli->calls[2].reduce_only && cli->calls[2].side == "SELL",
              "第3笔应是非 reduceOnly 开空");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Short, "反手后应持空");
        check(std::fabs(b.st.stop - 110.0) < 1e-6, "空头线应为 104+3*2=110");
        check(b.st.consec_reverses == 1, "连续反手计数应为1");
        check(b.realized_pnl < 0, "本笔应为亏损");
    }

    // ── 铁律：平仓失败时【绝不】开反向仓 ─────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fail_next = true;                  // 平仓单失败
        eng.tick("TESTUSDT", 104.0);

        check(cli->calls.size() == 2, "平仓失败后绝不能再发第3笔（开反向）");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Long, "平仓失败，仓位应原样保留为多");
        check(!b.pending, "平仓失败后 pending 必须复位以便下个tick重试");
        check(b.trade_count == 0, "平仓失败不得记成交");
    }

    // ── 平仓状态不明：reduceOnly 幂等，允许下个tick重试，且不得反手 ──────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->uncertain_next = true;
        eng.tick("TESTUSDT", 104.0);
        auto b = eng.get_bots()[0];
        check(cli->calls.size() == 2, "状态不明时不得反手");
        check(b.state == SarBot::State::Running, "平仓状态不明不应停bot（幂等可重试）");
        check(!b.pending, "应允许下个tick重试");

        cli->fill_price = 104.0;                // 重试成功
        eng.tick("TESTUSDT", 104.0);
        check(cli->calls.size() == 4, "重试应发平仓+反手开仓两笔");
        check(eng.get_bots()[0].st.pos == sar::Pos::Short, "重试成功后应已反手");
    }

    // ── reduceOnly 被拒(-2022)：交易所侧已无仓位 → 清本地并停止 ──────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fail_next = true;
        cli->error_next = "order would not reduce position [-2022]";
        eng.tick("TESTUSDT", 104.0);

        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Flat, "-2022 应清空本地仓位");
        check(b.qty == 0, "-2022 应清空数量");
        check(b.state == SarBot::State::Stopped, "-2022 应停止该bot待人工核对");
        check(cli->calls.size() == 2, "-2022 之后绝不能反手");
    }

    // ── 残量取整后归零：不得无限空转发0数量单 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1000.0;                 // 步长大到让任何残量都取整为0
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        SarEngine eng2(cli, inline_host());
        (void)eng2;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "开仓数量取整为0时就不该下单");
    }

    // ── pending 期间的 tick 必须被跳过 ──────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        eng.set_pending_for_test(id, true);
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "pending 中不得下单");
    }

    // ── 暂停的 bot 不交易 ────────────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        eng.stop_bot(id);
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "已暂停的 bot 不得下单");
    }

    // ── NaN 价格必须被拦在引擎入口 ───────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        eng.tick("TESTUSDT", std::nan(""));
        eng.tick("TESTUSDT", 0.0);
        eng.tick("TESTUSDT", -5.0);
        check(cli->calls.empty(), "NaN/0/负价格都不得触发下单");
    }

    // ── 信号过期：不开新仓，但已持仓的止损线仍然有效 ─────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.signal_max_age_sec = 60;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        advance(61);                            // 快照已超过 60 秒
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "信号过期时不得开新仓");
    }

    // ── 手动平仓 ─────────────────────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fill_price = 115.0;
        eng.close_bot(id);
        check(cli->calls.size() == 2, "手动平仓应发一笔 reduceOnly");
        check(cli->calls[1].reduce_only, "手动平仓必须是 reduceOnly");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Flat, "手动平仓后应为空仓");
        check(b.realized_pnl > 0, "115 平 110 的多头应为盈利");
        check(!b.pending, "手动平仓后 pending 必须复位");
    }

    if (g_fail == 0) {
        std::printf("OK: SAR 引擎订单路径测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
