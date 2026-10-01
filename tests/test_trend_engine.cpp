// SAR 引擎订单路径测试。
//
// 决策逻辑在 test_trend.cpp 里已经穷举过，这里只测"决策变成订单"这一段——
// 也就是只有和交易所打交道才会遇到的那些脏事：零成交、部分成交、状态不明、
// reduceOnly 被拒、取整后归零。它们平时永不执行，出问题的那一刻却最要命。
//
// 重点是【反手方案A】的安全性：平仓没成功就绝不能开反向仓，否则原仓位还在、
// 又叠一个反向仓，净敞口翻倍且方向不明。这是方案A唯一的致命失败模式。
#include "core/trend_engine.h"
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

    // 取第 n 笔下单；越界时返回一个空 Call 而不是 UB。
    // 用例的惯用写法是先 check(calls.size()==1) 再读 calls[0]——第一条失败时
    // 第二条就是越界访问，整个进程段错误，后面几百条用例的结果全部丢失。
    // 我在加"盘中即时入场"用例时正好踩到：只看见一条 FAIL 然后什么都没有了
    const Call& call_at(size_t n) const {
        static const Call kNone{"", 0, false};
        return n < calls.size() ? calls[n] : kNone;
    }
    const Call& last_call() const { return call_at(calls.empty() ? 0 : calls.size() - 1); }

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

    // ── 交易所侧灾难止损单 ──────────────────────────────────────────────────
    struct StopCall { std::string kind; double price; std::string id; };
    std::vector<StopCall> stop_log;
    bool   ds_place_fails = false;
    int    ds_seq = 0;
    // 失败时给的 retryable 由 ds_fail_retryable 控制：重试阶梯的用例要能
    // 分别构造"可重试"与"参数错误直接放弃"两条路
    bool ds_fail_retryable = true;
    StopPlacement place_disaster_stop(const std::string&, double stop_price,
                                      const std::string&, double) override {
        if (ds_place_fails) {
            stop_log.push_back({"place_fail", stop_price, ""});
            StopPlacement p; p.error = "boom"; p.retryable = ds_fail_retryable;
            return p;
        }
        std::string id = "DS" + std::to_string(++ds_seq);
        stop_log.push_back({"place", stop_price, id});
        StopPlacement p; p.order_id = id;
        return p;
    }
    bool cancel_disaster_stop(const std::string&, const std::string& id) override {
        stop_log.push_back({"cancel", 0, id});
        return true;
    }
    // 硬止损单是否还在活跃委托里。三种状态都要能构造：
    //   ds_live_known=false              → 查不到（必须走保守路径）
    //   ds_live_known=true,  live=true   → 还挂着（不是它平的）
    //   ds_live_known=true,  live=false  → 已经不在了（它触发了）
    bool ds_live_known = false;
    bool ds_live       = true;
    int  ds_live_calls = 0;
    bool is_disaster_stop_live(const std::string&, const std::string&, bool* ok) override {
        ++ds_live_calls;
        if (ok) *ok = ds_live_known;
        return ds_live;
    }
    int ds_count(const std::string& kind) const {
        int n = 0;
        for (const auto& c : stop_log) if (c.kind == kind) ++n;
        return n;
    }
    double ds_last_price() const {
        for (auto it = stop_log.rbegin(); it != stop_log.rend(); ++it)
            if (it->kind == "place") return it->price;
        return 0;
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

// 对账的测试入口：先把时钟推过"仓位刚变过"的宽限期，再按【刚拉到的】快照
// （age=0）比对。绝大多数用例关心的是配对逻辑本身，不是新鲜度守卫——
// 守卫本身另有专门的用例（见"刚成交的仓位不得被旧快照判成外部已平"）
static std::vector<std::string> reconcile(
        TrendEngine& e, const std::vector<TrendEngine::ExchangePos>& ex) {
    advance(10);
    return e.reconcile_positions(ex, std::chrono::milliseconds(0));
}

static TrendConfig mk_cfg(const std::string& sym = "TESTUSDT") {
    TrendConfig c;
    c.symbol      = sym;
    c.budget_usdt = 1000;
    c.leverage    = 3;
    c.rule.atr_mult = 3.0;
    // 多数用例要确定性地触发反手。默认是 None（不反手），这里显式开成立即反手
    c.rule.reverse = trend::ReverseMode::Immediate;
    return c;
}

// 多品种用例里按品种取 bot。get_bots() 的顺序是 map 的顺序（按 bot_id），
// 靠下标去猜是哪个 bot 迟早会错
static TrendBot bot_of(TrendEngine& e, const std::string& sym) {
    for (const auto& b : e.get_bots())
        if (b.cfg.symbol == sym) return b;
    return {};
}

// ③ 纯裸K 的默认配置：等收盘突破 + 3 根摆动止损 + 立即反手
static TrendConfig mk_bare_cfg(const std::string& sym = "TESTUSDT") {
    TrendConfig c = mk_cfg(sym);
    c.rule.strategy   = trend::Strategy::BareK;
    c.rule.bare_entry = trend::BareEntry::BreakPrevBar;
    c.rule.swing_bars = 3;
    return c;
}

// ③ 一份"收盘突破前高"（= 多头信号）的K线快照。
// prev_close 取在 prev_high 之上即构成突破；摆动极值由参数给
static TrendEngine::BarSnap bars_break_up(double lo, double hi, int64_t bar) {
    TrendEngine::BarSnap s;
    s.open = 100.0;
    s.prev_close = 112.0; s.prev_high = 110.0; s.prev_low = 100.0;
    s.swing_low = lo; s.swing_high = hi;
    s.bar_open_ms = bar;
    return s;
}
// ③ 收盘跌破前低（= 空头信号）
static TrendEngine::BarSnap bars_break_dn(double lo, double hi, int64_t bar) {
    TrendEngine::BarSnap s = bars_break_up(lo, hi, bar);
    s.prev_close = 98.0;
    return s;
}

// ② PSAR 的K线快照：只有前两根的高低点是必需的
static TrendEngine::BarSnap bars_psar(double ph, double pl,
                                      double p2h, double p2l, int64_t bar) {
    TrendEngine::BarSnap s;
    s.prev_high = ph; s.prev_low = pl;
    s.prev2_high = p2h; s.prev2_low = p2l;
    s.bar_open_ms = bar;
    return s;
}

// 喂一份新鲜信号：通道 [dn,up]，ATR=atr
static void feed(TrendEngine& e, const std::string& id, double atr,
                 double up, double dn, int64_t bar = 1) {
    e.update_signal(id, atr, atr, true, up, dn, bar);
}

int main() {
    // 关掉 stdout 缓冲。管道里 printf 是全缓冲的，进程崩溃时整段输出会一起丢掉——
    // 于是"崩在哪一条用例"这个唯一有用的信息就没了
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ── 开仓：突破 → 市价单 → 止损线布在 成交价 - k*ATR ────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        check(!id.empty(), "add_bot 应返回 bot_id");
        check(eng.add_bot(mk_cfg()).empty(), "同品种重复添加应被拒");

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        check(cli->calls.size() == 1, "应只发一笔开仓单");
        check(cli->call_at(0).side == "BUY" && !cli->call_at(0).reduce_only,
              "上破应发 BUY 非 reduceOnly");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Long, "应记为持多");
        check(std::fabs(b.st.stop - 104.0) < 1e-6, "止损线应为 110-3*2=104");
        check(b.qty > 0, "应记录成交数量");
        check(cli->leverage_calls == 1, "开仓前应设置一次杠杆");
    }

    // ── 零成交防幽灵仓：r.ok 但 executedQty=0，绝不能入账 ────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->zero_fill_next = true;
        eng.tick("TESTUSDT", 110.0);

        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Flat, "零成交不得记为持仓");
        check(b.qty == 0, "零成交不得入账数量");
        check(!b.pending, "零成交后 pending 必须复位，否则该bot永久冻结");
    }

    // ── 开仓状态不明 → 停止该bot（开仓不幂等，盲目重试会双倍仓位）──────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->uncertain_next = true;
        eng.tick("TESTUSDT", 110.0);

        auto b = eng.get_bots()[0];
        check(b.state == TrendBot::State::Stopped, "开仓状态不明应停止该bot");
        check(!b.pending, "停止时 pending 也必须复位");
    }

    // ── ATR 缺失不开仓（没有ATR就没有止损线）────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        eng.update_signal(id, 0, 0, true, 110, 90, 1);   // atr=0
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "ATR 缺失时不应下单");
    }

    // ── 盈利出场：平仓单为 reduceOnly，且不反手 ──────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);            // 开多 @110，线 104

        eng.tick("TESTUSDT", 130.0);            // 新高 → 线 124
        check(std::fabs(eng.get_bots()[0].st.stop - 124.0) < 1e-6, "线应上移到 124");

        cli->fill_price = 124.0;
        eng.tick("TESTUSDT", 124.0);            // 回撤触线，124 > 110 成本 → 盈利出场

        check(cli->calls.size() == 2, "应只有开仓+平仓两笔");
        check(cli->call_at(1).side == "SELL" && cli->call_at(1).reduce_only,
              "平多应发 reduceOnly SELL");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Flat, "平仓后应为空仓");
        check(b.trade_count == 1 && b.win_count == 1, "应记一笔盈利成交");
        check(b.realized_pnl > 0, "已实现盈亏应为正");
        check(!b.pending, "盈利出场后 pending 必须复位");
    }

    // ── 亏损出场 + 反手：两笔单，顺序正确 ───────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);            // 开多 @110，线 104

        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);            // 触线，104 < 110 → 亏损 → 反手

        check(cli->calls.size() == 3, "应为 开多 + 平多 + 开空 三笔");
        check(cli->call_at(1).reduce_only && cli->call_at(1).side == "SELL",
              "第2笔应是 reduceOnly 平多");
        check(!cli->call_at(2).reduce_only && cli->call_at(2).side == "SELL",
              "第3笔应是非 reduceOnly 开空");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Short, "反手后应持空");
        check(std::fabs(b.st.stop - 110.0) < 1e-6, "空头线应为 104+3*2=110");
        check(b.st.consec_reverses == 1, "连续反手计数应为1");
        check(b.realized_pnl < 0, "本笔应为亏损");
    }

    // ── 铁律：平仓失败时【绝不】开反向仓 ─────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fail_next = true;                  // 平仓单失败
        eng.tick("TESTUSDT", 104.0);

        check(cli->calls.size() == 2, "平仓失败后绝不能再发第3笔（开反向）");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Long, "平仓失败，仓位应原样保留为多");
        check(!b.pending, "平仓失败后 pending 必须复位以便下个tick重试");
        check(b.trade_count == 0, "平仓失败不得记成交");
    }

    // ── 平仓状态不明：reduceOnly 幂等，允许下个tick重试，且不得反手 ──────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->uncertain_next = true;
        eng.tick("TESTUSDT", 104.0);
        auto b = eng.get_bots()[0];
        check(cli->calls.size() == 2, "状态不明时不得反手");
        check(b.state == TrendBot::State::Running, "平仓状态不明不应停bot（幂等可重试）");
        check(!b.pending, "应允许下个tick重试");

        cli->fill_price = 104.0;                // 重试成功
        eng.tick("TESTUSDT", 104.0);
        check(cli->calls.size() == 4, "重试应发平仓+反手开仓两笔");
        check(eng.get_bots()[0].st.pos == trend::Pos::Short, "重试成功后应已反手");
    }

    // ── reduceOnly 被拒(-2022)：交易所侧已无仓位 → 清本地并停止 ──────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fail_next = true;
        cli->error_next = "order would not reduce position [-2022]";
        eng.tick("TESTUSDT", 104.0);

        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Flat, "-2022 应清空本地仓位");
        check(b.qty == 0, "-2022 应清空数量");
        check(b.state == TrendBot::State::Stopped, "-2022 应停止该bot待人工核对");
        check(cli->calls.size() == 2, "-2022 之后绝不能反手");
    }

    // ── -2022 但是【我们自己的硬止损触发了】⇒ 计划内出场，bot 必须继续跑 ─────
    //
    // 这是实盘最劝退的一处（2026-10-01）：
    //   08:50:17 QNTUSDT 开空 @288.69 止损线=292.36
    //   08:50:17 硬止损已挂 @292.9447          ← 只高出 0.20%
    //   08:56:07 平仓被拒(-2022) ⇒ 清空本地状态并【停止该bot】
    // 交易所那张单用【标记价】连续触发，本地止损线是 3 秒采样 —— 价格快速穿过时
    // 交易所几乎必然抢先。于是一次完全正常的止损出场，变成了要人工点"继续"。
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;   // 只看出场，不看反手
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);                   // 开多，止损线 104
        check(cli->ds_count("place") == 1, "硬止损已挂（前提成立）");

        // 交易所那张单已经不在活跃委托里 ⇒ 它触发了
        cli->ds_live_known = true;
        cli->ds_live       = false;
        cli->fail_next  = true;
        cli->error_next = "order would not reduce position [-2022]";
        eng.tick("TESTUSDT", 104.0);

        auto b = eng.get_bots()[0];
        check(cli->ds_live_calls >= 1, "必须去核对那张单还在不在");
        check(b.st.pos == trend::Pos::Flat && b.qty == 0, "仓位状态应清空（它确实没了）");
        check(b.state == TrendBot::State::Running,
              "⚠ bot 必须【继续运行】—— 这是计划内的止损出场，和本地止损线触发"
              "没有任何区别，唯一差别是谁先动手。停掉等于让用户去点「继续」");
        check(b.trade_count == 1, "  必须记成一笔交易，不能把这次出场漏掉账");
        // 触发价 = 104 × (1 − 0.2%) = 103.792；用它当成交价的近似
        check(b.realized_pnl < 0, "  多头在 103.792 出场是亏的，盈亏要记进去");
        check(cli->ds_count("cancel") == 0,
              "  不得去撤那张单 —— 它已经成交了，撤它只会白报一个错");
    }

    // ── -2022 且那张单【还挂着】⇒ 不是它平的，仍须停下来让人核对 ─────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->ds_live_known = true;
        cli->ds_live       = true;    // 还挂着 ⇒ 平仓的不是它
        cli->fail_next  = true;
        cli->error_next = "order would not reduce position [-2022]";
        eng.tick("TESTUSDT", 104.0);

        auto b = eng.get_bots()[0];
        check(b.state == TrendBot::State::Stopped,
              "那张单还在 ⇒ 是人工平的或被强平，必须停下来让人核对");
        check(b.trade_count == 0, "  成因不明，不该凭空记一笔盈亏");
    }

    // ── -2022 且【查不到】活跃委托 ⇒ 不敢认，走保守路径 ──────────────────────
    // 拉取失败时活跃列表是空的，"这张单不在里面"会成立 —— 若不判 ok，
    // 任何一次 -2022 都会被认成"我们的止损触发了"，真正的强平就被放过去了。
    // 又是"没拿到当成没有"那个老毛病，这里钉住它
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->ds_live_known = false;   // 查不到
        cli->ds_live       = false;   // 返回值故意是"不在了"，但 ok=false
        cli->fail_next  = true;
        cli->error_next = "order would not reduce position [-2022]";
        eng.tick("TESTUSDT", 104.0);

        auto b = eng.get_bots()[0];
        check(b.state == TrendBot::State::Stopped,
              "⚠ 查不到就不许认成「我们的止损触发了」—— 否则强平会被当成计划内出场");
        check(b.trade_count == 0, "  同样不该记账");
    }

    // ── 残量取整后归零：不得无限空转发0数量单 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1000.0;                 // 步长大到让任何残量都取整为0
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        TrendEngine eng2(cli, inline_host());
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
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        eng.set_pending_for_test(id, true);
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "pending 中不得下单");
    }

    // ── 暂停的 bot 不交易 ────────────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        eng.stop_bot(id);
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.empty(), "已暂停的 bot 不得下单");
    }

    // ── NaN 价格必须被拦在引擎入口 ───────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
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
        TrendEngine eng(cli, inline_host());
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
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        cli->fill_price = 115.0;
        eng.close_bot(id);
        check(cli->calls.size() == 2, "手动平仓应发一笔 reduceOnly");
        check(cli->call_at(1).reduce_only, "手动平仓必须是 reduceOnly");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Flat, "手动平仓后应为空仓");
        check(b.realized_pnl > 0, "115 平 110 的多头应为盈利");
        check(!b.pending, "手动平仓后 pending 必须复位");
    }

    // ── ③ 纯裸K（等收盘突破）：完整下单路径 ─────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        auto id = eng.add_bot(cfg);

        // 只喂K线，【完全不喂 ATR】——裸K 的止损不该依赖它
        eng.update_bars(id, bars_break_up(94.0, 106.0, 1));
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);

        check(cli->calls.size() == 1, "裸K：收盘突破前高应开多（无需 ATR）");
        check(cli->call_at(0).side == "BUY", "  方向为 BUY");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Long, "  应记为持多");
        check(std::fabs(b.st.stop - 94.0) < 1e-9,
              "  止损线 = 前 3 根最低价 94，不是 k×ATR");

        eng.update_bars(id, bars_break_up(97.0, 110.0, 2));
        eng.tick("TESTUSDT", 105.0);
        check(std::fabs(eng.get_bots()[0].st.stop - 97.0) < 1e-9, "  止损线跟到 97");

        eng.update_bars(id, bars_break_up(90.0, 110.0, 3));
        eng.tick("TESTUSDT", 104.0);
        check(std::fabs(eng.get_bots()[0].st.stop - 97.0) < 1e-9,
              "  摆动低点下移时止损线必须钉住（棘轮）");

        const int before = (int)cli->calls.size();
        cli->fill_price = 97.0;
        eng.update_bars(id, bars_break_dn(90.0, 110.0, 4));
        eng.tick("TESTUSDT", 97.0);
        check((int)cli->calls.size() > before, "  触线应下平仓单");
        check(cli->call_at(before).reduce_only, "  平仓单是 reduceOnly");
        // ⚠ 回归：反手路径上曾有一道无条件的 atr<=0 闸门（只有海龟时留下的）。
        //   裸K 与 PSAR 根本不喂 ATR，那道闸会让它们【永远反不了手】，
        //   而日志只有一行"ATR缺失未反手"、界面一切正常
        check(eng.get_bots()[0].st.pos == trend::Pos::Short,
              "  亏损触线 + 立即反手：必须真的反手（裸K 全程没有 ATR）");
    }
    {
        // 收盘价夹在前一根高低之间 = 没有突破 = 不开仓。
        // 这正是新规则比旧"阳线就做多"严格的地方
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        auto id = eng.add_bot(cfg);
        TrendEngine::BarSnap s = bars_break_up(94.0, 106.0, 1);
        s.prev_close = 105.0;              // 夹在 prev_low..prev_high 之间
        eng.update_bars(id, s);
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.empty(), "裸K：收盘未突破前一根高低，不得开仓");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        auto id = eng.add_bot(cfg);
        TrendEngine::BarSnap s = bars_break_up(0, 0, 1);   // 摆动极值缺失
        eng.update_bars(id, s);
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.empty(), "裸K：摆动数据缺失不得开仓");
    }
    {
        // ③ 盘中即时入场：实时价 vs 本根开盘价，不等收盘
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        cfg.rule.bare_entry = trend::BareEntry::Immediate;
        auto id = eng.add_bot(cfg);

        TrendEngine::BarSnap s;
        s.open = 100.0;
        s.swing_low = 94.0; s.swing_high = 106.0;
        s.bar_open_ms = 1;
        eng.update_bars(id, s);

        cli->fill_price = 101.0;
        eng.tick("TESTUSDT", 101.0);
        check(cli->calls.size() == 1, "盘中即时：实时价 > 开盘价 ⇒ 开多");
        check(cli->call_at(0).side == "BUY", "  方向为 BUY");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        cfg.rule.bare_entry = trend::BareEntry::Immediate;
        auto id = eng.add_bot(cfg);
        TrendEngine::BarSnap s;
        s.open = 100.0;
        s.swing_low = 94.0; s.swing_high = 106.0;
        s.bar_open_ms = 1;
        eng.update_bars(id, s);
        cli->fill_price = 99.0;
        eng.tick("TESTUSDT", 99.0);
        check(cli->calls.size() == 1, "盘中即时：实时价 < 开盘价 ⇒ 开空");
        check(cli->call_at(0).side == "SELL", "  方向为 SELL");
    }
    {
        // 每根K线最多开一次：护栏开着时，同一根里被止损后不得重新入场。
        // 这是 Immediate 模式唯一的抖动刹车，也是最容易被"平仓即复位"抹掉的
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        cfg.rule.bare_entry   = trend::BareEntry::Immediate;
        cfg.rule.once_per_bar = true;
        cfg.rule.reverse      = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);

        TrendEngine::BarSnap s;
        s.open = 100.0;
        s.swing_low = 99.0; s.swing_high = 106.0;   // 止损线贴得很近，好打掉
        s.bar_open_ms = 1;
        eng.update_bars(id, s);

        cli->fill_price = 101.0;
        eng.tick("TESTUSDT", 101.0);
        check(cli->calls.size() == 1, "本根第一次开仓");

        cli->fill_price = 99.0;
        eng.tick("TESTUSDT", 99.0);                 // 触 99 止损
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "  已被止损平掉");
        const size_t after_close = cli->calls.size();

        eng.tick("TESTUSDT", 101.0);                // 同一根里价格又拉回开盘价之上
        check(cli->calls.size() == after_close,
              "  护栏生效：同一根K线里不得重新入场");

        s.bar_open_ms = 2;                          // 跨到下一根
        eng.update_bars(id, s);
        eng.tick("TESTUSDT", 101.0);
        check(cli->calls.size() == after_close + 1, "  新K线到来后恢复开仓");
    }
    {
        // 固定单笔风险在裸K下必须按【真实止损距离】算，不是 k×ATR。
        // 这是 plan_qty 改签名的全部理由
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        cfg.rule.atr_mult = 3.0;           // 故意留着，验证它【不】参与计算
        cfg.size_mode   = TrendConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 1000000.0;
        auto id = eng.add_bot(cfg);

        eng.update_bars(id, bars_break_up(94.0, 106.0, 1));
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.size() == 1, "裸K + 固定单笔风险：应能下单");
        check(std::fabs(cli->call_at(0).qty - 100.0 / 6.0) < 1e-6,
              "  数量 = risk / |现价-止损线|，不是 risk/(k*ATR)");
        check(std::fabs(cli->call_at(0).qty * 6.0 - 100.0) < 1e-6,
              "  单次止损亏损恰为 risk_usdt");
    }

    // ── ② 抛物线SAR：完整下单路径 ───────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.strategy = trend::Strategy::ParabolicSar;
        cfg.rule.reverse  = trend::ReverseMode::Immediate;
        auto id = eng.add_bot(cfg);

        // 不喂 ATR、不喂通道——PSAR 只要前两根的高低点
        eng.update_bars(id, bars_psar(110.0, 94.0, 111.0, 93.0, 1));
        cli->fill_price = 111.0;
        eng.tick("TESTUSDT", 111.0);       // 站上前一根最高价 110
        check(cli->calls.size() == 1, "PSAR：上破前一根最高价应开多（无需 ATR/通道）");
        check(cli->call_at(0).side == "BUY", "  方向为 BUY");
        auto b = eng.get_bots()[0];
        check(b.st.pos == trend::Pos::Long, "  应记为持多");
        check(std::fabs(b.st.stop - 94.0) < 1e-9, "  初始 SAR = 前一根最低价 94");
        check(std::fabs(b.st.af - cfg.rule.af_start) < 1e-12, "  AF 从 af_start 起算");

        // 创新高 ⇒ AF 递增，SAR 跟上
        eng.update_bars(id, bars_psar(115.0, 100.0, 114.0, 99.0, 2));
        eng.tick("TESTUSDT", 120.0);
        b = eng.get_bots()[0];
        check(std::fabs(b.st.af - (cfg.rule.af_start + cfg.rule.af_step)) < 1e-12,
              "  刷新 EP ⇒ AF += af_step");
        check(b.st.stop > 94.0, "  SAR 应已上移");

        // 触及 SAR ⇒ 平仓并【立即反向】，这是 stop-and-reverse 的定义
        const size_t before = cli->calls.size();
        const double line = b.st.stop;
        cli->fill_price = line;
        eng.tick("TESTUSDT", line);
        check(cli->calls.size() >= before + 1, "  触及 SAR 应发单");
        check(cli->call_at(before).reduce_only, "  先平仓（reduceOnly）");
        check(eng.get_bots()[0].st.pos == trend::Pos::Short,
              "  并立即反手做空——PSAR 永远持有一个方向");
    }
    {
        // PSAR 的连续反手上限：震荡市里它是唯一的刹车
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.strategy = trend::Strategy::ParabolicSar;
        cfg.rule.reverse  = trend::ReverseMode::Immediate;
        cfg.rule.max_consecutive_reverses = 1;
        cfg.rule.cooldown_bars = 5;
        auto id = eng.add_bot(cfg);

        eng.update_bars(id, bars_psar(110.0, 94.0, 111.0, 93.0, 1));
        cli->fill_price = 111.0;
        eng.tick("TESTUSDT", 111.0);
        check(eng.get_bots()[0].st.pos == trend::Pos::Long, "先开一笔多");

        cli->fill_price = 94.0;
        eng.tick("TESTUSDT", 94.0);        // 触 SAR
        check(eng.get_bots()[0].st.pos == trend::Pos::Short, "第 1 次翻转：转空");
        check(eng.get_bots()[0].st.consec_reverses == 1, "  连续反手计数 = 1");

        const double sar = eng.get_bots()[0].st.stop;
        cli->fill_price = sar;
        eng.tick("TESTUSDT", sar);
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat,
              "达到上限后只平不反——PSAR 也必须有刹车");
        check(eng.get_bots()[0].st.cooldown_left == 5, "  并进入冷却");
    }

    // ── 按 ATR 等风险下单 ────────────────────────────────────────────────────
    {
        // 同样的 risk_usdt，ATR 大的品种应当拿到更小的名义——这正是"铺开品种
        // 分散风险"能成立的前提。固定名义下，高波动品种会独占全部风险
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;              // 不让取整掩盖差异
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = TrendConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;           // 每次止损愿亏 100U
        cfg.budget_usdt = 1'000'000.0;     // 名义上限设很高，先看纯公式
        cfg.rule.atr_mult = 3.0;
        auto id = eng.add_bot(cfg);

        feed(eng, id, 2.0, 110, 90);       // ATR=2.0，价 110 ⇒ 止损距离 6.0
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.size() == 1, "等风险模式应能下单");
        // 数量 = risk / (k×ATR) = 100 / 6 = 16.667
        check(std::fabs(cli->call_at(0).qty - 100.0 / 6.0) < 1e-6,
              "数量应为 risk/(k×ATR)");
        // 止损被打时亏的钱 = qty × 止损距离 = 100U，与 ATR 无关
        check(std::fabs(cli->call_at(0).qty * 6.0 - 100.0) < 1e-6,
              "  单次止损亏损应恰为 risk_usdt");
    }
    {
        // ATR 翻倍 ⇒ 名义减半
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = TrendConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 1'000'000.0;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 4.0, 110, 90);       // ATR 翻倍
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(std::fabs(cli->call_at(0).qty - 100.0 / 12.0) < 1e-6,
              "ATR 翻倍时数量应减半");
    }
    {
        // 名义上限必须兜住：ATR 趋近 0 时公式会算出荒谬的大仓位
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = TrendConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 500.0;           // 名义上限
        auto id = eng.add_bot(cfg);
        feed(eng, id, 0.001, 110, 90);     // ATR 极小 ⇒ 公式给出天量
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(std::fabs(cli->call_at(0).qty - 500.0 / 110.0) < 1e-6,
              "ATR 极小时必须被 budget_usdt 封顶，否则一个品种能吃掉整个账户");
    }


    // ═══ 对账 ═══════════════════════════════════════════════════════════════
    // 这一整段以前没有任何测试（ExchangePos 在 tests/ 里一次都没出现过），
    // 而它是 SAR 里最危险的一段：判错就停 bot，而 bot 一停那笔仓位就没人管了。
    using EP = TrendEngine::ExchangePos;

    // 建一个持多的 bot，返回 (引擎, id)。多个用例要用，提出来
    auto mk_long_bot = [](std::shared_ptr<FakeClient> cli, TrendEngine& eng) {
        auto id = eng.add_bot(mk_cfg());
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        return id;
    };

    // ── 回归：双向模式下反向腿不得被误判成"方向不一致" ──────────────────────
    // 实盘日志里的 "SAR对账: ZECUSDT 本地方向(多)与交易所(空)不一致，已停止该bot"
    // 就是这条：快照里同品种有多空两条（pos_cache_ 以 symbol+"_L"/"_S" 为键），
    // 而原来的配对只按品种取【第一条命中】，unordered_map 顺序不定，取到空腿就
    // 报冲突。同向那条明明在，数量也对
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q = eng.get_bots()[0].qty;

        // 反向腿【排在前面】，正是会触发误判的顺序
        std::vector<EP> ex = {
            {"TESTUSDT", -1, 5.0, 120.0},   // 空腿（别的东西开的）
            {"TESTUSDT",  1, q,   110.0},   // 我们自己的多腿
        };
        auto issues = reconcile(eng,ex);
        check(issues.size() == 1, "同向腿存在且数量一致时，不得报成方向不一致");
        check(issues[0].find("方向") == std::string::npos,
              "  报的不能是方向冲突");
        // 但那条反向腿【本身】必须说出来：同品种只剩一套策略之后它没有任何
        // 归属，旧注释把它委托给了已经删掉的 DCA 孤儿仓核查，等于没人管
        check(issues[0].find("没有跟踪") != std::string::npos,
              "  而是明确报出「有一条本程序没有跟踪的反向腿」");
        check(eng.get_bots()[0].state == TrendBot::State::Running,
              "  bot 必须继续运行——误停等于把仓位变成裸敞口");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long, "  仓位状态不得被改动");

        // 同一条腿不重复报：对账每分钟一轮，刷屏会把真正该看的告警冲散
        auto again = reconcile(eng,ex);
        check(again.empty(), "  同一条反向腿不得每轮重报");

        // 腿消失后再出现，要重新报——去重不能变成永久静音
        reconcile(eng, {{"TESTUSDT", 1, q, 110.0}});
        auto back = reconcile(eng,ex);
        check(back.size() == 1 && back[0].find("没有跟踪") != std::string::npos,
              "  反向腿消失后再出现，必须重新报");
    }

    // ── 刚成交的仓位不得被【旧快照】判成"外部已平"（v5.9.4 实盘事故）────────
    //
    // 事故原文（同一秒内的两行）：
    //   00:20:42 QNTUSDT 开空 qty=0.3 @$303.18
    //   00:20:42 对账: 本地有仓位但交易所没有（外部已平/被强平），已停止该bot
    //   00:21:02 硬止损挂上时仓位已不在，撤掉这张孤儿单
    // GUI 的周期对账复用每 tick 刷新的 pos_cache_，那份快照最旧可达 30 秒，
    // 于是"快照拍摄时这笔仓位还不存在"被读成了"交易所没有这笔仓位"。
    // 后果不是少报一条，而是交易所上留下一笔【没有止损、没有 bot 管】的真仓位。
    //
    // pending 那道守卫拦不住：订单已经入账，pending 此刻已经是 false
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = false;    // 这条只关心对账
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);      // 开多，此刻刚成交

        const auto b0 = eng.get_bots()[0];
        check(b0.st.pos == trend::Pos::Long && b0.qty > 0, "已开仓（前提成立）");
        check(!b0.pending, "  且已入账——pending 已复位，那道守卫拦不住（前提成立）");

        // 30 秒前拍的快照：里面当然没有这笔 3 秒前才开出来的仓位
        advance(3);
        auto stale = eng.reconcile_positions({}, std::chrono::milliseconds(30000));
        check(stale.empty(),
              "⚠ 快照比这笔仓位还老时，不得判定「交易所没有这个仓位」");
        const auto b1 = eng.get_bots()[0];
        check(b1.state == TrendBot::State::Running, "  bot 必须继续运行");
        check(b1.st.pos == trend::Pos::Long && b1.qty > 0,
              "  本地跟踪不得被清——清掉就等于放任一笔真实仓位裸奔");

        // 同样的空快照，但确实是【现在】拍的：那才真的是"外部已平"，必须报
        advance(10);
        auto fresh = eng.reconcile_positions({}, std::chrono::milliseconds(0));
        check(fresh.size() == 1,
              "  快照足够新时，「交易所确实没有」仍必须照报——宽限不能变成漏报");
        // 新鲜度守卫放行之后，走的是"两轮确认"那条路：第一轮只记存疑
        advance(10);
        auto confirm = eng.reconcile_positions({}, std::chrono::milliseconds(0));
        check(confirm.size() == 1, "  第二轮确认");
        check(eng.get_bots()[0].state == TrendBot::State::Stopped, "  并停掉该bot");
    }

    // ── 反过来：刚平掉的仓位不得被旧快照判成"孤儿仓" ────────────────────────
    // 旧快照里那笔仓位还在，而本地已经平了 —— 按字面比对就是"交易所有仓位但
    // 本地无跟踪"，于是停掉一个其实完全正常的 bot
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = false;
        // mk_cfg 默认是"立即反手"，触线会直接开成空单而不是空仓——
        // 这条用例要的是【平掉变空仓】，所以显式关掉反手
        cfg.rule.reverse = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 开多
        advance(10);
        const double q = eng.get_bots()[0].qty;

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);       // 触线平掉
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "已平仓（前提成立）");

        advance(2);
        auto stale = eng.reconcile_positions({{"TESTUSDT", 1, q, 110.0}},
                                             std::chrono::milliseconds(30000));
        check(stale.empty(), "⚠ 旧快照里仓位还在，不得报成孤儿仓并停 bot");
        check(eng.get_bots()[0].state == TrendBot::State::Running,
              "  bot 必须继续运行");
    }

    // ── 真正的方向冲突仍必须停 bot（修完不能把这条一起放过去）────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);

        std::vector<EP> ex = { {"TESTUSDT", -1, 5.0, 120.0} };   // 只有反向
        auto issues = reconcile(eng,ex);
        check(issues.size() == 1, "本地持多而交易所只有空仓，必须报一条");
        check(issues[0].find("方向") != std::string::npos, "  应判为方向不一致");
        check(eng.get_bots()[0].state == TrendBot::State::Stopped, "  必须停止该bot");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long,
              "  方向冲突时不清本地状态：清了就没有证据可核对");

        // ⚠ 但【只报一次】。不去重的话对账每分钟一轮就每分钟重报一遍，而且
        //   永远不会消失——实测日志里 4 分钟刷了 30 行，把同期真正该看的硬
        //   止损告警全冲散了。
        //   注意去重的判据是【不一致的种类】，不是"bot 已停止"：v5.9.4 用的是
        //   后者（整个跳过已停止的 bot），代价是一笔真实仓位对对账永久隐形
        auto again = reconcile(eng,ex);
        check(again.empty(), "  同一类不一致不得在后续每一轮对账里重复报");
        auto again2 = reconcile(eng,ex);
        check(again2.empty(), "  再对一次也还是不报");

        // 但恢复之后要重新纳入对账：不一致还在，就得重新报出来并再停一次
        eng.resume_bot(eng.get_bots()[0].bot_id);
        auto after_resume = reconcile(eng,ex);
        check(after_resume.size() == 1, "  恢复后重新纳入对账，不一致仍要报");
        check(eng.get_bots()[0].state == TrendBot::State::Stopped, "  并再次停止");
    }

    // ── 外部已平：两边都没有同向仓，也没有反向仓 ────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);

        // ⚠ 第一轮【只记存疑，不动任何状态】。清本地状态是不可逆的：抹掉开仓价、
        //   止损线、数量之后，界面上连"平仓"按钮都不再渲染（条件是 pos!=Flat
        //   && qty>0），于是程序刚刚忘掉的那笔真实仓位，用户在程序里没有任何
        //   办法处理。v5.9.4 实盘就是这样，只能去交易所人工收拾
        auto first = reconcile(eng,{});   // 交易所空空
        check(first.size() == 1, "第一轮应报一条");
        check(first[0].find("存疑") != std::string::npos, "  且只是【存疑】");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long,
              "  ⚠ 第一轮绝不能清本地仓位——清了用户就再也没法在程序里平它");
        check(eng.get_bots()[0].qty > 0, "  数量也不动");
        check(eng.get_bots()[0].state == TrendBot::State::Running,
              "  bot 继续运行：真还在就继续被正常管理，真没了下一拍平仓会撞 -2022 自行收敛");

        // 第二轮仍然查不到 ⇒ 确认，这才清
        auto second = reconcile(eng,{});
        check(second.size() == 1, "第二轮应报确认");
        check(second[0].find("外部已平") != std::string::npos, "  应判为外部已平");
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "  这时才清空本地仓位");
        check(eng.get_bots()[0].qty == 0, "  数量应归零");
        check(eng.get_bots()[0].state == TrendBot::State::Stopped,
              "  应停止：分不清是人工平的还是被强平的，续跑是往坑里跳");
    }

    // ── 存疑之后仓位又出现了：必须当成误报撤销，不能跨轮累积成确认 ──────────
    // 拉取抖动、快照陈旧都可能让某一轮查不到。若把互不相邻的两次"查不到"
    // 攒成一次确认，两轮确认这道防线就形同虚设
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q = eng.get_bots()[0].qty;

        auto first = reconcile(eng,{});
        check(first.size() == 1 && first[0].find("存疑") != std::string::npos,
              "第一轮记存疑（前提成立）");

        // 中间这一轮查到了 ⇒ 之前那次是误报
        auto ok = reconcile(eng, {{"TESTUSDT", 1, q, 110.0}});
        check(ok.empty(), "  查到了，无不一致");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long, "  仓位完好");

        // 再查不到，这只能算【第一次】，不许直接确认
        auto again = reconcile(eng,{});
        check(again.size() == 1 && again[0].find("存疑") != std::string::npos,
              "  ⚠ 不相邻的两次「查不到」不得攒成一次确认");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long,
              "  本地仓位仍不得被清");
    }

    // ── 孤儿仓：本地空仓而交易所有仓，必须停 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        eng.add_bot(mk_cfg());                        // 加了但没开仓

        std::vector<EP> ex = { {"TESTUSDT", 1, 3.0, 100.0} };
        auto issues = reconcile(eng,ex);
        check(issues.size() == 1, "本地空仓而交易所有仓，应报一条");
        check(issues[0].find("孤儿") != std::string::npos ||
              issues[0].find("本地无跟踪") != std::string::npos, "  应判为孤儿仓");
        check(eng.get_bots()[0].state == TrendBot::State::Stopped,
              "  必须停止：不停的话下一个信号会再开一笔，净敞口翻倍");
    }

    // ── 外部部分平仓：收敛数量，但保留开仓价与止损线 ────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0   = eng.get_bots()[0].qty;
        const double stop = eng.get_bots()[0].st.stop;

        std::vector<EP> ex = { {"TESTUSDT", 1, q0 / 2, 110.0} };
        auto issues = reconcile(eng,ex);
        check(issues.size() == 1, "外部部分平仓应报一条");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - q0 / 2) < 1e-12, "  数量应收敛到交易所值");
        check(std::fabs(b.st.stop - stop) < 1e-12, "  止损线必须保留：它仍然成立");
        check(b.state == TrendBot::State::Running, "  部分平仓不该停 bot");
    }

    // ── 交易所多于本地：只告警，不动本地状态 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0 = eng.get_bots()[0].qty;

        std::vector<EP> ex = { {"TESTUSDT", 1, q0 * 2, 110.0} };
        auto issues = reconcile(eng,ex);
        check(issues.size() == 1, "交易所多于本地应报一条");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - q0) < 1e-12,
              "  本地数量不得改动：按交易所接管等于让止损线去管一笔不是自己开的仓");
        check(b.state == TrendBot::State::Running, "  仅告警，不停 bot");
    }

    // ── 在途的 bot 一律跳过 ─────────────────────────────────────────────────
    // 订单可能已在交易所生效而本地还没入账，此刻比对必然误判——而误判方向恰好
    // 最坏：正在平仓的会被当成"外部已平"
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = mk_long_bot(cli, eng);
        eng.set_pending_for_test(id, true);

        auto issues = reconcile(eng,{});   // 交易所看起来空
        check(issues.empty(), "pending 的 bot 必须跳过对账");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long, "  仓位不得被清空");
        check(eng.get_bots()[0].state == TrendBot::State::Running, "  不得被停止");
    }

    // ── 别的品种的仓位与本 bot 无关 ─────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0 = eng.get_bots()[0].qty;

        std::vector<EP> ex = {
            {"OTHERUSDT", -1, 9.0, 50.0},
            {"TESTUSDT",   1, q0,  110.0},
        };
        check(reconcile(eng,ex).empty(), "不相关品种的仓位不得影响本 bot");
    }

    // ═══ 交易所侧灾难止损（v4.7.0）════════════════════════════════════════════
    // 它是 SAR 唯一的进程外保护：本地止损线活在进程里，程序崩了就什么都不剩。
    // 这一整套里最容易写错、且错了看不出来的是【缓冲方向】——挂反了的话交易所会
    // 抢在本地之前触发，把正常止损变成"外部平仓 → 对账停 bot"
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.disaster_stop_buffer_pct = 1.0;
        auto id = eng.add_bot(cfg);

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 开多，止损线 = 110 - 3*2 = 104

        check(cli->ds_count("place") == 1, "开仓成交即挂灾难止损（不等下一个 tick）");
        // 多头：挂在止损线【下方】。104 × (1-1%) = 102.96
        check(std::fabs(cli->ds_last_price() - 102.96) < 1e-9,
              "  多头挂单价 = 止损线 × (1−缓冲) = 102.96");
        check(cli->ds_last_price() < eng.get_bots()[0].st.stop,
              "  ⚠ 必须【低于】本地止损线——高于的话交易所会抢先触发，"
              "正常止损会变成外部平仓并停 bot");
    }

    // ── 固定语义：挂上之后【永不重挂】，无论止损线怎么走 ────────────────────
    // v5.0.1 之前这张单镜像移动止损，线每移动超 0.5% 就撤旧挂新。改成固定的
    // 理由是撤挂之间有个"旧单已撤、新单未挂"的空窗——而这张单的全部职责就是
    // "进程死了兜住"，空窗期正是最不该有它不在的时候
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.disaster_stop_buffer_pct = 1.0;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 开仓，初始线 = 110-3*2 = 104
        check(cli->ds_count("place") == 1, "开仓挂一次");
        const double placed_at = cli->ds_last_price();
        check(std::fabs(placed_at - 104.0 * 0.99) < 1e-9,
              "  触发价 = 初始止损线外扩 1%");

        // 一路大涨：极值推到 160，止损线从 104 棘轮到 154 —— 涨了近 50%
        for (double px : {112.0, 125.0, 140.0, 160.0}) {
            cli->fill_price = px;
            eng.tick("TESTUSDT", px);
        }
        const auto b = eng.get_bots()[0];
        check(b.st.stop > 150.0, "止损线确实棘轮上去了（前提成立）");
        check(cli->ds_count("place") == 1,  "止损线涨了 50% 也【只挂过这一次】");
        check(cli->ds_count("cancel") == 0, "  全程零撤单 ⇒ 零空窗");
        check(std::fabs(cli->ds_last_price() - placed_at) < 1e-12,
              "  交易所那张单仍停在开仓时的价位（这是它作为「最大风险兜底」的定位）");
        check(std::fabs(b.disaster_stop_price - placed_at) < 1e-12,
              "  本地记的触发价同样冻结在开仓时刻");
    }

    // ── 平仓后必须撤单：不撤会留在挂单列表里，下一轮开仓再挂被币安拒 ─────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;        // 只平不反手，便于观察
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        const int cancels_before = cli->ds_count("cancel");

        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);       // 触线平仓
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "已平仓");
        check(cli->ds_count("cancel") == cancels_before + 1, "平仓后必须撤掉灾难止损单");
        check(eng.get_bots()[0].disaster_stop_id.empty(), "  本地单号必须清掉");
    }

    // ── 重试阶梯：失败后按退避重试，中途成功就恢复 ──────────────────────────
    // 用一个小工具把"推进虚拟时钟 + 喂一拍价格"合起来：重试由 tick 驱动，
    // 而退避用的是 EngineHost 的单调钟，所以必须两个一起推
    auto pump = [](TrendEngine& e, FakeClient& c, int secs, double px) {
        advance(secs);
        c.fill_price = px;
        e.tick("TESTUSDT", px);
    };

    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails = true;

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 开仓 + 第 1 次挂单（失败）
        check(cli->ds_count("place_fail") == 1, "开仓即尝试挂单，失败被记录");
        check(eng.get_bots()[0].disaster_stop_id.empty(),
              "  失败时不得留下假单号（否则下次会拿它去撤一张不存在的单）");
        check(eng.get_bots()[0].ds_attempts == 1, "  失败次数已累计");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long,
              "  ⚠ 挂单失败【不影响持仓】：本地移动止损照常守着，只是没有进程外保护");

        // 退避没到就不该重试——否则十三次会在一瞬间烧完，而失败原因根本没变
        eng.tick("TESTUSDT", 110.0);
        check(cli->ds_count("place_fail") == 1, "退避未到，不得重试");

        pump(eng, *cli, 1, 110.0);
        check(cli->ds_count("place_fail") == 2, "退避到点后重试第 2 次");

        // 第 3 次失败会先告一次警（"正在重试"），但还不到兜底
        pump(eng, *cli, 1, 110.0);
        check(cli->ds_count("place_fail") == 3, "第 3 次");
        check(!eng.get_bots()[0].ds_unprotected,
              "  第 3 次只是提前告警，还不该标成\"无保护\"（那是第 10 次的语义）");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long, "  仓位还在");

        // 中途恢复：单子挂上，计数清零，标记清掉
        cli->ds_place_fails = false;
        pump(eng, *cli, 3, 110.0);
        check(cli->ds_count("place") == 1, "恢复后挂单成功");
        check(eng.get_bots()[0].ds_attempts == 0,    "  失败计数清零");
        check(!eng.get_bots()[0].ds_unprotected,      "  无保护标记清掉");
        check(!eng.get_bots()[0].disaster_stop_id.empty(), "  单号已记下");
    }

    // ── 重试耗尽 → 标红告警 → 兜底平仓 ──────────────────────────────────────
    // 这条是整套硬止损策略的核心：使用者把"哪怕断电断网也要有底"列成了硬要求，
    // 挂不上就是这个前提不成立。此时继续持仓等于默默降级成另一套风险模型
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;    // 兜底平仓不该触发反手，这里也隔离掉
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails = true;

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 第 1 次

        // 一路推到第 10 次：此时应标成"无交易所侧保护"，但仓位还在
        for (int i = 2; i <= TrendEngine::kDsBackoffEnd; ++i) pump(eng, *cli, 3, 110.0);
        check(cli->ds_count("place_fail") == TrendEngine::kDsBackoffEnd, "已失败 10 次");
        check(eng.get_bots()[0].ds_unprotected,
              "  第 10 次后必须标成\"此仓位无交易所侧保护\"（界面据此标红）");
        check(eng.get_bots()[0].st.pos == trend::Pos::Long,
              "  但此刻【还不平仓】——规格是再给 3 次机会");

        // 再推 3 次 → 第 13 次失败 → 兜底平仓
        const size_t calls_before = cli->calls.size();
        for (int i = TrendEngine::kDsBackoffEnd + 1; i <= TrendEngine::kDsMaxTries; ++i)
            pump(eng, *cli, 4, 110.0);
        check(cli->ds_count("place_fail") == TrendEngine::kDsMaxTries, "共尝试 13 次");
        check(cli->calls.size() == calls_before + 1, "  第 13 次失败后立即发平仓单");
        check(cli->last_call().reduce_only, "  且是 reduceOnly");
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "  仓位已平");
        check(eng.get_bots()[0].qty == 0, "  数量归零");
        check(eng.get_bots()[0].ds_fail_closes == 1, "  熔断计数 +1");
        check(eng.get_bots()[0].state == TrendBot::State::Running,
              "  第 1 次还不熔断，bot 继续等下一个入场信号");
        check(eng.get_bots()[0].ds_attempts == 0, "  仓位关闭 → 重试状态复位");
    }

    // ── 参数类错误不走完整阶梯：直接兜底 ────────────────────────────────────
    // 精度不对、触发价在错误的一侧这类错误，重试十三次结果一样，而那二十多秒
    // 里仓位一直没有进程外保护
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails    = true;
        cli->ds_fail_retryable = false;    // 模拟 -1111 / -2021 这类

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(cli->ds_count("place_fail") == 1, "只尝试了 1 次");
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat,
              "  重试无意义的错误 → 立即兜底平仓，不白等二十多秒");
    }

    // ── 熔断：连续 2 次因挂不上而平仓 → 停掉这个 bot ────────────────────────
    // 没有熔断的话，持续性故障会变成 开仓→挂不上→平仓→等信号→开仓→… 的循环，
    // 每轮付两次手续费，而单边行情里信号可能每根K线都来
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails    = true;
        cli->ds_fail_retryable = false;    // 走快速路径，省掉两轮 13 次

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 第 1 轮：开仓 → 挂不上 → 平仓
        check(eng.get_bots()[0].ds_fail_closes == 1, "第 1 轮计数 = 1");
        check(eng.get_bots()[0].state == TrendBot::State::Running, "  还没熔断");

        // 第 2 轮：新的突破信号 → 再开一笔 → 又挂不上 → 又平仓 → 熔断
        feed(eng, id, 2.0, 110, 90, 2);
        advance(5);
        eng.tick("TESTUSDT", 115.0);
        const auto b = eng.get_bots()[0];
        check(b.ds_fail_closes == 2, "第 2 轮计数 = 2");
        check(b.state == TrendBot::State::Stopped,
              "  连续 2 次 → 判定为系统性故障，停掉 bot，不再开新仓");
        check(b.st.pos == trend::Pos::Flat, "  且仓位是平的");
    }

    // ── 兜底平仓后不得在【同一根K线】里重开 ─────────────────────────────────
    // 实测日志（2026-09-29）：22:16:28 兜底平仓、22:16:31 就又开了一笔，间隔 3 秒。
    // 裸K·盘中即时 的入场条件（实时价 > 本根开盘价）在平仓后的下一拍照样成立，
    // 于是变成 开→挂不上→平→开 的循环，每轮两笔市价单手续费。
    // 兜底平仓的含义是"开仓前提不成立"，而那个前提 3 秒后不会变成立
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_bare_cfg();
        cfg.rule.bare_entry = trend::BareEntry::Immediate;
        cfg.rule.reverse    = trend::ReverseMode::None;
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails    = true;
        cli->ds_fail_retryable = false;   // 快速路径：一次失败就兜底

        TrendEngine::BarSnap s;
        s.open = 100.0; s.swing_low = 94.0; s.swing_high = 106.0; s.bar_open_ms = 1;
        eng.update_bars(id, s);
        cli->fill_price = 101.0;
        eng.tick("TESTUSDT", 101.0);      // 开仓 → 挂不上 → 兜底平仓
        check(eng.get_bots()[0].st.pos == trend::Pos::Flat, "兜底平仓已完成");
        check(eng.get_bots()[0].st.cooldown_left > 0, "  应进入冷却");
        const size_t after = cli->calls.size();

        // 同一根K线里价格仍在开盘价之上，入场条件照样成立
        eng.tick("TESTUSDT", 102.0);
        eng.tick("TESTUSDT", 103.0);
        check(cli->calls.size() == after, "  兜底平仓后不得在同一根K线里重开");

        // 跨到下一根：冷却耗尽，恢复开仓（冷却是刹车，不是停机）
        s.bar_open_ms = 2;
        eng.update_bars(id, s);
        eng.tick("TESTUSDT", 102.0);
        check(cli->calls.size() > after, "  跨到新K线后应恢复开仓");
    }

    // ── 账户级熔断：系统性故障不该让每个 bot 各烧满自己的额度 ─────────────────
    // 挂不上硬止损的原因基本都是系统性的（账户受限、网络到不了交易所、端点不对），
    // 而系统性故障对每个品种一视同仁。只有 per-bot 熔断时，9 个 bot 会各烧
    // kDsCircuitBreak 轮才停 —— 18 轮开平、36 笔白付手续费的市价单
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());

        // C 先开一笔【不挂硬止损】的仓，留到熔断之后验证出场不受影响
        auto cc = mk_cfg("CCCUSDT");
        cc.use_disaster_stop = false;
        cc.rule.reverse = trend::ReverseMode::None;
        auto ic = eng.add_bot(cc);
        feed(eng, ic, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("CCCUSDT", 110.0);
        check(bot_of(eng, "CCCUSDT").qty > 0, "C 先正常开一笔（不挂硬止损）");

        auto ca = mk_cfg("AAAUSDT");
        ca.use_disaster_stop = true; ca.rule.reverse = trend::ReverseMode::None;
        auto cb = mk_cfg("BBBUSDT");
        cb.use_disaster_stop = true; cb.rule.reverse = trend::ReverseMode::None;
        auto ia = eng.add_bot(ca);
        auto ib = eng.add_bot(cb);
        cli->ds_place_fails    = true;
        cli->ds_fail_retryable = false;

        feed(eng, ia, 2.0, 110, 90);
        eng.tick("AAAUSDT", 110.0);       // A：开→挂不上→平（累计 1）
        feed(eng, ib, 2.0, 110, 90);
        eng.tick("BBBUSDT", 110.0);       // B：开→挂不上→平（累计 2 ⇒ 账户级熔断）
        check(bot_of(eng, "AAAUSDT").st.pos == trend::Pos::Flat, "A 已兜底平仓");
        check(bot_of(eng, "BBBUSDT").st.pos == trend::Pos::Flat, "B 已兜底平仓");

        // 换新K线让 A 的冷却过去，此时拦住它的应该是【账户级闸门】
        const size_t before = cli->calls.size();
        feed(eng, ia, 2.0, 110, 90, 2);
        advance(5);
        eng.tick("AAAUSDT", 115.0);
        check(cli->calls.size() == before,
              "账户级熔断后不得再开新仓（哪怕这个 bot 自己还没到 per-bot 上限）");
        check(bot_of(eng, "AAAUSDT").last_decision.find("账户级熔断")
                  != std::string::npos,
              "  拦截原因要写进 last_decision，界面上看得到");

        // ⚠ 最危险的失败模式：闸门拦住出场。拦住平仓等于把该止损的仓位困在原地，
        //   闸门就从风控变成了风险源
        cli->fill_price = 100.0;
        eng.tick("CCCUSDT", 100.0);       // 跌破 C 的止损线 104
        check(cli->calls.size() > before, "  但【出场】照常发单，绝不能被闸门拦住");
        check(cli->last_call().reduce_only, "  且是 reduceOnly 平仓单");
        check(bot_of(eng, "CCCUSDT").st.pos == trend::Pos::Flat, "  C 已正常止损出场");
    }

    // ── 保护单核对：交易所上那张单没了要能发现并重挂 ─────────────────────────
    // 本地只要 disaster_stop_id 非空就认为受保护、再也不重挂（见 try_place_hard_stop
    // 的第一道守卫），而对账只比仓位。于是"单子被手动撤了 / 落盘里是过期单号"
    // 会变成静默失去保护：界面显示已挂、日志一片安静，交易所上什么都没有
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        const std::string ds_id = eng.get_bots()[0].disaster_stop_id;
        check(!ds_id.empty(), "先正常挂上一张保护单");

        // 它还在 ⇒ 什么都不该动
        auto none = eng.reconcile_stop_orders({ds_id});
        check(none.empty(), "单子还在活跃列表里 ⇒ 不报也不动");
        check(eng.get_bots()[0].disaster_stop_id == ds_id, "  单号原样保留");

        // 它不在了 ⇒ 清掉本地记录并标红，让每 tick 的重挂逻辑接手
        auto gone = eng.reconcile_stop_orders({"99999"});
        check(gone.size() == 1, "单子不在活跃列表里 ⇒ 报一条");
        check(eng.get_bots()[0].disaster_stop_id.empty(),
              "  清掉本地单号 —— 不清的话 try_place_hard_stop 第一道守卫就 return，"
              "永远不会重挂");
        check(eng.get_bots()[0].disaster_stop_price == 0, "  触发价一并清掉");
        check(eng.get_bots()[0].ds_unprotected,
              "  标成无保护：此刻确实没有进程外保护，界面该标红");
        check(eng.get_bots()[0].ds_attempts == 0,
              "  重试次数复位 —— 这是全新的挂单机会，不该带着旧计数几次就触发兜底平仓");

        // 下一拍应当真的重挂一张
        const int before = cli->ds_count("place");
        eng.tick("TESTUSDT", 111.0);
        check(cli->ds_count("place") > before, "  下一拍重新挂一张");
        check(!eng.get_bots()[0].disaster_stop_id.empty(), "  并拿到新单号");
    }
    {
        // 不该动的几种情形。这一层的危害全在【误判】上：错清一次就会白撤白挂，
        // 而 closePosition 同方向只允许一张，运气不好新的还挂不上
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.reverse = trend::ReverseMode::None;
        auto id = eng.add_bot(cfg);

        // 空仓：没有仓位就没有要保护的东西
        check(eng.reconcile_stop_orders({}).empty(), "空仓时不报");

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        eng.set_pending_for_test(id, true);
        check(eng.reconcile_stop_orders({}).empty(),
              "在途(pending)时不报：订单可能已在交易所生效而本地还没入账");
        eng.set_pending_for_test(id, false);
    }
    {
        // 没开这个功能的 bot 不该被核对 —— 它本来就没有保护单
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = false;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(eng.reconcile_stop_orders({}).empty(),
              "未开启委托止损的 bot 不参与核对");
    }

    // ── 没开开关就一个请求都不发，但【必须说一声】────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        std::vector<std::string> logs;
        eng.set_log_cb([&](const std::string& m) { logs.push_back(m); });
        auto id = eng.add_bot(mk_cfg());   // use_disaster_stop 默认 false
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        eng.tick("TESTUSDT", 125.0);
        check(cli->stop_log.empty(), "未开启时全程不得有任何挂单/撤单调用");

        // ⚠ 实测（三小时实盘）：一个从落盘恢复的 bot 开了 5 笔仓，每一笔都没有
        //   交易所侧保护，而日志一个字都没提 —— 同一份日志里另一个品种每笔都打
        //   "硬止损已挂"，所以看日志的人只会觉得挺好。
        //   整套设计的前提是"断电断网也有底"，没有这个底的仓位不该是安静的
        int notices = 0;
        for (const auto& m : logs)
            if (m.find("未开启") != std::string::npos) ++notices;
        check(notices == 1, "开仓时必须提醒一次「没开委托止损，这个仓位没有底」");

        // 每个 bot 只说一次：每笔都说会变噪音
        const size_t before = logs.size();
        feed(eng, id, 2.0, 130, 90, 5);
        eng.tick("TESTUSDT", 131.0);
        int again = 0;
        for (size_t i = before; i < logs.size(); ++i)
            if (logs[i].find("未开启") != std::string::npos) ++again;
        check(again == 0, "  但只说一次，后续开仓不再重复");
    }

    // ── 账户级闸门：总保证金上限 ────────────────────────────────────────────
    // 这道闸在网格DCA 移除时从 CcgEngine 搬了过来。为什么趋势策略更需要它：
    // 铺开多个品种是这套策略有效的前提（单品种胜率本来就低，靠分散摊平），
    // 于是 N 个品种 × 各自的 budget 很容易超过账户权益。而"每笔都带止损"
    // 只保证【单笔】亏损有界，不保证同时被打的十笔加起来有界
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        eng.set_max_total_margin(400);      // budget 1000 / 杠杆 3 ≈ 333 每笔

        auto a = eng.add_bot(mk_cfg("AAAUSDT"));
        auto b = eng.add_bot(mk_cfg("BBBUSDT"));

        feed(eng, a, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("AAAUSDT", 110.0);
        check(eng.get_bots().size() == 2, "两个 bot 都在");
        {
            bool opened = false;
            for (const auto& x : eng.get_bots())
                if (x.cfg.symbol == "AAAUSDT" && x.qty > 0) opened = true;
            check(opened, "第一笔在上限内，正常开仓");
        }

        const size_t calls_before = cli->calls.size();
        feed(eng, b, 2.0, 110, 90);
        eng.tick("BBBUSDT", 110.0);
        check(cli->calls.size() == calls_before,
              "第二笔会让总保证金超限 → 一张单都不该发出去");
        for (const auto& x : eng.get_bots())
            if (x.cfg.symbol == "BBBUSDT") {
                check(x.qty == 0 && x.st.pos == trend::Pos::Flat, "  被拦的 bot 保持空仓");
                check(x.last_decision.find("账户总保证金") != std::string::npos,
                      "  拦截原因写进 last_decision（界面能看到，不只在日志里）");
            }

        // 放开上限后必须立刻能开——闸门是条件判断，不是一次性的闩
        eng.set_max_total_margin(0);
        feed(eng, b, 2.0, 110, 90, 2);
        eng.tick("BBBUSDT", 110.0);
        check(cli->calls.size() > calls_before, "上限放开后同一个 bot 立刻能开仓");
    }

    // ── 账户级闸门：同时持仓品种数上限 ──────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        eng.set_max_open_positions(1);

        auto a = eng.add_bot(mk_cfg("AAAUSDT"));
        auto b = eng.add_bot(mk_cfg("BBBUSDT"));
        feed(eng, a, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("AAAUSDT", 110.0);

        const size_t n1 = cli->calls.size();
        feed(eng, b, 2.0, 110, 90);
        eng.tick("BBBUSDT", 110.0);
        check(cli->calls.size() == n1, "已达并发持仓上限 → 第二个品种不开");
        for (const auto& x : eng.get_bots())
            if (x.cfg.symbol == "BBBUSDT")
                check(x.last_decision.find("品种数") != std::string::npos,
                      "  拦截原因说明是品种数而不是保证金");
    }

    // ── 闸门【绝不】拦出场 ──────────────────────────────────────────────────
    // 这条是这道闸最危险的失败模式：拦住平仓等于把一笔该止损的仓位困在原地，
    // 闸门就从风控变成了风险源。用"上限设成 0.01（比任何一笔都小）"来构造
    // 最严的闸，然后验证已有仓位照样能正常止损出场
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto c = mk_cfg();
        c.rule.reverse = trend::ReverseMode::None;       // 先隔离出"纯平仓"这一条路径
        auto id = eng.add_bot(c);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(eng.get_bots()[0].qty > 0, "先正常开一笔");

        eng.set_max_total_margin(0.01);     // 此后任何开仓都会被拦
        eng.set_max_open_positions(1);

        const size_t n1 = cli->calls.size();
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);        // 跌破 104 的止损线
        check(cli->calls.size() == n1 + 1, "止损出场照常发单，没有被闸门拦住");
        check(cli->last_call().reduce_only, "  且是 reduceOnly 平仓单");
        check(eng.get_bots()[0].qty == 0, "  仓位已清空");
    }

    // ── 闸门也不拦【反手的开仓腿】────────────────────────────────────────────
    // 反手走 submit_close 内部那条路，不经过 tick 里的闸门。这是有意的：
    // 反手是"这一笔已经结束、趋势翻了"的延续，净敞口大致不变（换个方向而不是
    // 叠一层）。拦掉它只会留下一个方向已经证伪的空仓状态
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());     // reverse=immediate（无条件反手）
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        eng.set_max_total_margin(0.01);
        const size_t n1 = cli->calls.size();
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);         // 亏损止损 → 反手
        check(cli->calls.size() == n1 + 2, "平仓 + 反向开仓两笔都发了出去");
        check(cli->call_at(n1).reduce_only,      "  第一笔是 reduceOnly 平仓");
        check(!cli->call_at(n1 + 1).reduce_only, "  第二笔是反向开仓");
        check(eng.get_bots()[0].st.pos == trend::Pos::Short, "  已反手为空头");
    }

    if (g_fail == 0) {
        std::printf("OK: SAR 引擎订单路径测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
