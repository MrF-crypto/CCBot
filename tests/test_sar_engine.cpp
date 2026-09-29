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

    // ── 交易所侧灾难止损单 ──────────────────────────────────────────────────
    struct StopCall { std::string kind; double price; std::string id; };
    std::vector<StopCall> stop_log;
    bool   ds_place_fails = false;
    int    ds_seq = 0;
    std::string place_disaster_stop(const std::string&, double stop_price,
                                    const std::string&) override {
        if (ds_place_fails) { stop_log.push_back({"place_fail", stop_price, ""}); return ""; }
        std::string id = "DS" + std::to_string(++ds_seq);
        stop_log.push_back({"place", stop_price, id});
        return id;
    }
    bool cancel_disaster_stop(const std::string&, const std::string& id) override {
        stop_log.push_back({"cancel", 0, id});
        return true;
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

    // ── 裸K线模式：完整下单路径 ──────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.mode = sar::Mode::BarPattern;
        cfg.rule.swing_bars = 3;
        cfg.rule.reverse_needs_signal = false;
        auto id = eng.add_bot(cfg);

        // 只喂K线，【完全不喂 ATR】——裸K线模式的止损不该依赖它
        eng.update_bars(id, true, false, 94.0, 106.0, 1);
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);

        check(cli->calls.size() == 1, "裸K线：阳线收盘应开多（无需 ATR）");
        check(cli->calls[0].side == "BUY", "  方向为 BUY");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Long, "  应记为持多");
        check(std::fabs(b.st.stop - 94.0) < 1e-9,
              "  止损线 = 前 3 根最低价 94，不是 k×ATR");

        eng.update_bars(id, true, false, 97.0, 110.0, 2);
        eng.tick("TESTUSDT", 105.0);
        check(std::fabs(eng.get_bots()[0].st.stop - 97.0) < 1e-9, "  止损线跟到 97");

        eng.update_bars(id, true, false, 90.0, 110.0, 3);
        eng.tick("TESTUSDT", 104.0);
        check(std::fabs(eng.get_bots()[0].st.stop - 97.0) < 1e-9,
              "  摆动低点下移时止损线必须钉住（棘轮）");

        const int before = (int)cli->calls.size();
        cli->fill_price = 97.0;
        eng.update_bars(id, false, true, 90.0, 110.0, 4);
        eng.tick("TESTUSDT", 97.0);
        check((int)cli->calls.size() > before, "  触线应下平仓单");
        check(cli->calls[before].reduce_only, "  平仓单是 reduceOnly");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.mode = sar::Mode::BarPattern;
        auto id = eng.add_bot(cfg);
        eng.update_bars(id, false, false, 94.0, 106.0, 1);
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.empty(), "裸K线：十字星不得开仓");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.mode = sar::Mode::BarPattern;
        auto id = eng.add_bot(cfg);
        eng.update_bars(id, true, false, 0, 0, 1);
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.empty(), "裸K线：摆动数据缺失不得开仓");
    }
    {
        // 等风险下单在裸K线模式下必须按【真实止损距离】算，不是 k×ATR。
        // 这是 plan_qty 改签名的全部理由
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.mode = sar::Mode::BarPattern;
        cfg.rule.swing_bars = 3;
        cfg.rule.atr_mult   = 3.0;
        cfg.size_mode   = SarConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 1000000.0;
        auto id = eng.add_bot(cfg);

        eng.update_bars(id, true, false, 94.0, 106.0, 1);
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);
        check(cli->calls.size() == 1, "裸K线 + 等风险：应能下单");
        check(std::fabs(cli->calls[0].qty - 100.0 / 6.0) < 1e-6,
              "  数量 = risk / |现价-止损线|，不是 risk/(k*ATR)");
        check(std::fabs(cli->calls[0].qty * 6.0 - 100.0) < 1e-6,
              "  单次止损亏损恰为 risk_usdt");
    }

    // ── 按 ATR 等风险下单 ────────────────────────────────────────────────────
    {
        // 同样的 risk_usdt，ATR 大的品种应当拿到更小的名义——这正是"铺开品种
        // 分散风险"能成立的前提。固定名义下，高波动品种会独占全部风险
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;              // 不让取整掩盖差异
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = SarConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;           // 每次止损愿亏 100U
        cfg.budget_usdt = 1'000'000.0;     // 名义上限设很高，先看纯公式
        cfg.rule.atr_mult = 3.0;
        auto id = eng.add_bot(cfg);

        feed(eng, id, 2.0, 110, 90);       // ATR=2.0，价 110 ⇒ 止损距离 6.0
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(cli->calls.size() == 1, "等风险模式应能下单");
        // 数量 = risk / (k×ATR) = 100 / 6 = 16.667
        check(std::fabs(cli->calls[0].qty - 100.0 / 6.0) < 1e-6,
              "数量应为 risk/(k×ATR)");
        // 止损被打时亏的钱 = qty × 止损距离 = 100U，与 ATR 无关
        check(std::fabs(cli->calls[0].qty * 6.0 - 100.0) < 1e-6,
              "  单次止损亏损应恰为 risk_usdt");
    }
    {
        // ATR 翻倍 ⇒ 名义减半
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = SarConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 1'000'000.0;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 4.0, 110, 90);       // ATR 翻倍
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(std::fabs(cli->calls[0].qty - 100.0 / 12.0) < 1e-6,
              "ATR 翻倍时数量应减半");
    }
    {
        // 名义上限必须兜住：ATR 趋近 0 时公式会算出荒谬的大仓位
        auto cli = std::make_shared<FakeClient>();
        cli->qty_step = 1e-8;
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.size_mode   = SarConfig::SizeMode::RiskBased;
        cfg.risk_usdt   = 100.0;
        cfg.budget_usdt = 500.0;           // 名义上限
        auto id = eng.add_bot(cfg);
        feed(eng, id, 0.001, 110, 90);     // ATR 极小 ⇒ 公式给出天量
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(std::fabs(cli->calls[0].qty - 500.0 / 110.0) < 1e-6,
              "ATR 极小时必须被 budget_usdt 封顶，否则一个品种能吃掉整个账户");
    }

    // ── 金字塔加仓 ───────────────────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.pyramid_max_adds = 2;
        cfg.rule.pyramid_step_atr = 0.5;   // ATR=2.0 ⇒ 每涨 1.0 加一档
        auto id = eng.add_bot(cfg);

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 开多 @110，线 104
        check(cli->calls.size() == 1, "首档已开");

        eng.tick("TESTUSDT", 110.5);       // 只涨 0.5，不够一档
        check(cli->calls.size() == 1, "未达加仓间距不得加仓");

        cli->fill_price = 111.0;
        eng.tick("TESTUSDT", 111.0);       // 涨 1.0 = 0.5×ATR ⇒ 加第1档
        check(cli->calls.size() == 2, "达到间距应加仓");
        check(!cli->calls[1].reduce_only && cli->calls[1].side == "BUY",
              "加仓单应是同向非 reduceOnly");
        auto b = eng.get_bots()[0];
        check(b.st.adds_done == 1, "加仓档数应为1");
        // 加权均价。固定名义模式下两档的【数量不同】（同样 1000U，110 买到的
        // 比 111 多），所以不是简单中点——按实际成交量算期望值
        {
            const double q1 = cli->calls[0].qty, q2 = cli->calls[1].qty;
            const double want = (110.0 * q1 + 111.0 * q2) / (q1 + q2);
            check(std::fabs(b.st.entry_price - want) < 1e-9,
                  "entry_price 必须更新为加权均价，否则盈亏判定会用错基准");
            check(b.st.entry_price > 110.0 && b.st.entry_price < 111.0,
                  "  加权均价应落在两档成交价之间");
        }

        cli->fill_price = 112.0;
        eng.tick("TESTUSDT", 112.0);       // 再涨 1.0 ⇒ 加第2档
        check(cli->calls.size() == 3, "应加到第2档");
        check(eng.get_bots()[0].st.adds_done == 2, "加仓档数应为2");

        cli->fill_price = 113.0;
        eng.tick("TESTUSDT", 113.0);       // 已达上限
        check(cli->calls.size() == 3, "达到 pyramid_max_adds 后不得再加");
    }
    {
        // 触线与加仓同时满足时，【出场优先】——否则会先加一档再立刻被平掉
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.pyramid_max_adds = 3;
        cfg.rule.pyramid_step_atr = 0.5;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);       // 线 104
        const int before = (int)cli->calls.size();
        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);       // 触线
        check((int)cli->calls.size() > before, "触线应产生平仓/反手单");
        check(cli->calls[before].reduce_only, "  且第一笔必须是平仓，不是加仓");
    }
    {
        // 关闭时（默认 0）完全不加仓
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());   // pyramid_max_adds 默认 0
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        for (double p : {112.0, 115.0, 120.0, 130.0}) eng.tick("TESTUSDT", p);
        check(cli->calls.size() == 1, "未开启金字塔时不得加仓");
    }
    {
        // 反手开出来的仓位，加仓档数必须从 0 重新算
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.rule.pyramid_max_adds = 2;
        cfg.rule.pyramid_step_atr = 0.5;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        cli->fill_price = 111.0;
        eng.tick("TESTUSDT", 111.0);       // 加了一档
        check(eng.get_bots()[0].st.adds_done == 1, "加了一档");

        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);       // 触线亏损 → 反手开空
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Short, "应已反手");
        check(b.st.adds_done == 0, "反手后加仓档数必须归零");
        check(std::fabs(b.st.last_add_price - 104.0) < 1e-6,
              "  加仓基准价应为新仓成交价");
    }

    // ═══ 对账 ═══════════════════════════════════════════════════════════════
    // 这一整段以前没有任何测试（ExchangePos 在 tests/ 里一次都没出现过），
    // 而它是 SAR 里最危险的一段：判错就停 bot，而 bot 一停那笔仓位就没人管了。
    using EP = SarEngine::ExchangePos;

    // 建一个持多的 bot，返回 (引擎, id)。多个用例要用，提出来
    auto mk_long_bot = [](std::shared_ptr<FakeClient> cli, SarEngine& eng) {
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
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q = eng.get_bots()[0].qty;

        // 反向腿【排在前面】，正是会触发误判的顺序
        std::vector<EP> ex = {
            {"TESTUSDT", -1, 5.0, 120.0},   // 空腿（别的东西开的）
            {"TESTUSDT",  1, q,   110.0},   // 我们自己的多腿
        };
        auto issues = eng.reconcile_positions(ex);
        check(issues.empty(), "同向腿存在且数量一致时，反向腿不得报成方向不一致");
        check(eng.get_bots()[0].state == SarBot::State::Running,
              "  bot 必须继续运行——误停等于把仓位变成裸敞口");
        check(eng.get_bots()[0].st.pos == sar::Pos::Long, "  仓位状态不得被改动");
    }

    // ── 真正的方向冲突仍必须停 bot（修完不能把这条一起放过去）────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);

        std::vector<EP> ex = { {"TESTUSDT", -1, 5.0, 120.0} };   // 只有反向
        auto issues = eng.reconcile_positions(ex);
        check(issues.size() == 1, "本地持多而交易所只有空仓，必须报一条");
        check(issues[0].find("方向") != std::string::npos, "  应判为方向不一致");
        check(eng.get_bots()[0].state == SarBot::State::Stopped, "  必须停止该bot");
        check(eng.get_bots()[0].st.pos == sar::Pos::Long,
              "  方向冲突时不清本地状态：清了就没有证据可核对");
    }

    // ── 外部已平：两边都没有同向仓，也没有反向仓 ────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);

        auto issues = eng.reconcile_positions({});   // 交易所空空
        check(issues.size() == 1, "本地有仓交易所没有，应报一条");
        check(issues[0].find("外部已平") != std::string::npos, "  应判为外部已平");
        check(eng.get_bots()[0].st.pos == sar::Pos::Flat, "  应清空本地仓位");
        check(eng.get_bots()[0].qty == 0, "  数量应归零");
        check(eng.get_bots()[0].state == SarBot::State::Stopped,
              "  应停止：分不清是人工平的还是被强平的，续跑是往坑里跳");
    }

    // ── 孤儿仓：本地空仓而交易所有仓，必须停 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        eng.add_bot(mk_cfg());                        // 加了但没开仓

        std::vector<EP> ex = { {"TESTUSDT", 1, 3.0, 100.0} };
        auto issues = eng.reconcile_positions(ex);
        check(issues.size() == 1, "本地空仓而交易所有仓，应报一条");
        check(issues[0].find("孤儿") != std::string::npos ||
              issues[0].find("本地无跟踪") != std::string::npos, "  应判为孤儿仓");
        check(eng.get_bots()[0].state == SarBot::State::Stopped,
              "  必须停止：不停的话下一个信号会再开一笔，净敞口翻倍");
    }

    // ── 外部部分平仓：收敛数量，但保留开仓价与止损线 ────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0   = eng.get_bots()[0].qty;
        const double stop = eng.get_bots()[0].st.stop;

        std::vector<EP> ex = { {"TESTUSDT", 1, q0 / 2, 110.0} };
        auto issues = eng.reconcile_positions(ex);
        check(issues.size() == 1, "外部部分平仓应报一条");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - q0 / 2) < 1e-12, "  数量应收敛到交易所值");
        check(std::fabs(b.st.stop - stop) < 1e-12, "  止损线必须保留：它仍然成立");
        check(b.state == SarBot::State::Running, "  部分平仓不该停 bot");
    }

    // ── 交易所多于本地：只告警，不动本地状态 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0 = eng.get_bots()[0].qty;

        std::vector<EP> ex = { {"TESTUSDT", 1, q0 * 2, 110.0} };
        auto issues = eng.reconcile_positions(ex);
        check(issues.size() == 1, "交易所多于本地应报一条");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - q0) < 1e-12,
              "  本地数量不得改动：按交易所接管等于让止损线去管一笔不是自己开的仓");
        check(b.state == SarBot::State::Running, "  仅告警，不停 bot");
    }

    // ── 在途的 bot 一律跳过 ─────────────────────────────────────────────────
    // 订单可能已在交易所生效而本地还没入账，此刻比对必然误判——而误判方向恰好
    // 最坏：正在平仓的会被当成"外部已平"
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = mk_long_bot(cli, eng);
        eng.set_pending_for_test(id, true);

        auto issues = eng.reconcile_positions({});   // 交易所看起来空
        check(issues.empty(), "pending 的 bot 必须跳过对账");
        check(eng.get_bots()[0].st.pos == sar::Pos::Long, "  仓位不得被清空");
        check(eng.get_bots()[0].state == SarBot::State::Running, "  不得被停止");
    }

    // ── 别的品种的仓位与本 bot 无关 ─────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        mk_long_bot(cli, eng);
        const double q0 = eng.get_bots()[0].qty;

        std::vector<EP> ex = {
            {"OTHERUSDT", -1, 9.0, 50.0},
            {"TESTUSDT",   1, q0,  110.0},
        };
        check(eng.reconcile_positions(ex).empty(), "不相关品种的仓位不得影响本 bot");
    }

    // ═══ 交易所侧灾难止损（v4.7.0）════════════════════════════════════════════
    // 它是 SAR 唯一的进程外保护：本地止损线活在进程里，程序崩了就什么都不剩。
    // 这一整套里最容易写错、且错了看不出来的是【缓冲方向】——挂反了的话交易所会
    // 抢在本地之前触发，把正常止损变成"外部平仓 → 对账停 bot"
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
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

    // ── 棘轮小幅移动不重挂：否则每 tick 一次撤挂会吃光限流额度 ──────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        const int after_open = cli->ds_count("place");

        // 价格小涨 → 极值推高一点 → 止损线上移一点（远小于 0.5% 阈值）
        eng.tick("TESTUSDT", 110.2);
        check(cli->ds_count("place") == after_open,
              "止损线小幅移动不得重挂（阈值内）");

        // 大涨 → 止损线上移超过阈值 → 应该撤旧挂新
        eng.tick("TESTUSDT", 125.0);       // 极值 125 ⇒ 线 = 125-6 = 119
        check(cli->ds_count("place") == after_open + 1, "线移动超阈值应重挂一次");
        check(cli->ds_count("cancel") >= 1, "  重挂前必须先撤旧单（closePosition 同向只能一张）");
        check(std::fabs(cli->ds_last_price() - 119.0 * 0.99) < 1e-9,
              "  新挂单价跟着新止损线走");
    }

    // ── 平仓后必须撤单：不撤会留在挂单列表里，下一轮开仓再挂被币安拒 ─────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        cfg.rule.allow_reverse = false;        // 只平不反手，便于观察
        auto id = eng.add_bot(cfg);
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        const int cancels_before = cli->ds_count("cancel");

        cli->fill_price = 104.0;
        eng.tick("TESTUSDT", 104.0);       // 触线平仓
        check(eng.get_bots()[0].st.pos == sar::Pos::Flat, "已平仓");
        check(cli->ds_count("cancel") == cancels_before + 1, "平仓后必须撤掉灾难止损单");
        check(eng.get_bots()[0].disaster_stop_id.empty(), "  本地单号必须清掉");
    }

    // ── 挂单失败必须留痕，且下次线移动会重试 ────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto cfg = mk_cfg();
        cfg.use_disaster_stop = true;
        auto id = eng.add_bot(cfg);
        cli->ds_place_fails = true;

        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        check(cli->ds_count("place_fail") == 1, "挂单失败应被记录");
        check(eng.get_bots()[0].disaster_stop_id.empty(),
              "  失败时不得留下假单号（否则下次会拿它去撤一张不存在的单）");
        check(eng.get_bots()[0].st.pos == sar::Pos::Long,
              "  ⚠ 挂单失败【不影响持仓】：本地止损线照常守着，只是少了进程外保护");

        cli->ds_place_fails = false;
        eng.tick("TESTUSDT", 125.0);       // 线大幅上移 → 重试
        check(cli->ds_count("place") == 1, "线移动时应自动重试挂单");
    }

    // ── 没开开关就一个请求都不发 ────────────────────────────────────────────
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());   // use_disaster_stop 默认 false
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);
        eng.tick("TESTUSDT", 125.0);
        check(cli->stop_log.empty(), "未开启时全程不得有任何挂单/撤单调用");
    }

    // ── 账户级闸门：总保证金上限 ────────────────────────────────────────────
    // 这道闸在网格DCA 移除时从 CcgEngine 搬了过来。为什么趋势策略更需要它：
    // 铺开多个品种是这套策略有效的前提（单品种胜率本来就低，靠分散摊平），
    // 于是 N 个品种 × 各自的 budget 很容易超过账户权益。而"每笔都带止损"
    // 只保证【单笔】亏损有界，不保证同时被打的十笔加起来有界
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
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
                check(x.qty == 0 && x.st.pos == sar::Pos::Flat, "  被拦的 bot 保持空仓");
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
        SarEngine eng(cli, inline_host());
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
        SarEngine eng(cli, inline_host());
        auto c = mk_cfg();
        c.rule.allow_reverse = false;       // 先隔离出"纯平仓"这一条路径
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
        check(cli->calls.back().reduce_only, "  且是 reduceOnly 平仓单");
        check(eng.get_bots()[0].qty == 0, "  仓位已清空");
    }

    // ── 闸门也不拦【反手的开仓腿】────────────────────────────────────────────
    // 反手走 submit_close 内部那条路，不经过 tick 里的闸门。这是有意的：
    // 反手是"这一笔已经结束、趋势翻了"的延续，净敞口大致不变（换个方向而不是
    // 叠一层）。拦掉它只会留下一个方向已经证伪的空仓状态
    {
        auto cli = std::make_shared<FakeClient>();
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg());     // allow_reverse=true, 不要求反向信号
        feed(eng, id, 2.0, 110, 90);
        cli->fill_price = 110.0;
        eng.tick("TESTUSDT", 110.0);

        eng.set_max_total_margin(0.01);
        const size_t n1 = cli->calls.size();
        cli->fill_price = 100.0;
        eng.tick("TESTUSDT", 100.0);         // 亏损止损 → 反手
        check(cli->calls.size() == n1 + 2, "平仓 + 反向开仓两笔都发了出去");
        check(cli->calls[n1].reduce_only,      "  第一笔是 reduceOnly 平仓");
        check(!cli->calls[n1 + 1].reduce_only, "  第二笔是反向开仓");
        check(eng.get_bots()[0].st.pos == sar::Pos::Short, "  已反手为空头");
    }

    if (g_fail == 0) {
        std::printf("OK: SAR 引擎订单路径测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
