// DCA 的 ATR 移动止损测试。
//
// 这个功能的定义就是"勾选即冻结梯子"，所以最该守住的不是止损线算得对不对，
// 而是【武装之后绝不再补仓】——一旦漏了这一条，止损线和补仓位会同时生效，
// 而止损线总是更近，梯子永远只有第一层：一个 DCA bot 被伪装成单笔交易，
// 预算/层数/曲线全部失效，而界面上看不出任何异常。
#include "core/ccg_engine.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

// 假交易所：市价单永远全额成交在指定价
class FakeClient : public ITradingClient {
public:
    double next_fill_price = 0;
    int    market_orders   = 0;
    std::string last_side;
    bool   last_reduce_only = false;

    OrderOutcome place_market_order(const std::string&, const std::string& side,
                                    double qty, bool reduce_only) override {
        ++market_orders;
        last_side = side;
        last_reduce_only = reduce_only;
        OrderOutcome o;
        o.ok = true;
        o.order_id = "M" + std::to_string(market_orders);
        o.avg_price = next_fill_price;
        o.executed_qty = qty;
        return o;
    }
    double round_qty(const std::string&, double q) override { return q; }
    bool   set_leverage(const std::string&, int) override { return true; }
    bool   is_dual_mode() const override { return false; }
};

static EngineHost make_host(int64_t& now_ms) {
    EngineHost h;
    h.submit = [](std::function<void()> fn) { fn(); };
    h.now_wall = [&now_ms] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(now_ms));
    };
    h.now_steady = [&now_ms] {
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(now_ms));
    };
    return h;
}

static CcgConfig base_cfg() {
    CcgConfig c;
    c.symbol       = "BTCUSDT";
    c.direction    = CcgConfig::Direction::Long;
    c.strat_type   = CcgConfig::StratType::Flat;
    c.budget_usdt  = 1000.0;
    c.leverage     = 1;
    c.max_entries  = 4;
    c.entry_mode   = CcgConfig::EntryMode::Immediate;
    c.dynamic_band_mode = false;
    c.interval_pct = 10.0;
    c.trail_entry  = 0.0;      // 不要追踪建仓，跌到位立即补，便于构造
    c.tp_pct       = 5.0;
    c.trail_tp     = 2.0;
    c.use_htf_filter   = false;
    c.use_trend_filter = false;
    c.auto_restart     = false;
    c.cooldown_secs    = 0;
    return c;
}

int main() {
    // ── 基线：不开这个功能时，梯子照常补仓 ───────────────────────────────────
    {
        std::printf("\n── 基线：未勾选时梯子正常 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto id = eng.add_bot(base_cfg());

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        check(eng.get_bots()[0].entries.size() == 1, "首仓已建立");

        fc->next_fill_price = 90.0;
        now += 3000; eng.tick("BTCUSDT", 90.0);    // 跌 10% = 一个间隔
        check(eng.get_bots()[0].entries.size() == 2, "未勾选时应补到第2层");
    }

    // ── 核心：武装后绝不再补仓 ───────────────────────────────────────────────
    {
        std::printf("\n── 武装后梯子冻结 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.use_atr_trail    = true;
        c.atr_trail_mult   = 3.0;
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        check(eng.get_bots()[0].entries.size() == 1, "首仓已建立");
        check(!eng.get_bots()[0].atr_armed, "还没有 ATR，不该武装");

        // 没有 ATR 时【不武装】：梯子冻结却没有止损线是最差的状态，
        // 所以此时梯子应当照常工作
        fc->next_fill_price = 90.0;
        now += 3000; eng.tick("BTCUSDT", 90.0);
        check(eng.get_bots()[0].entries.size() == 2,
              "ATR 缺失时不武装，梯子应照常补仓（否则既不补也不保护）");

        // 喂入 ATR = 2.0，下一个 tick 武装
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 90.0);
        auto b = eng.get_bots()[0];
        check(b.atr_armed, "拿到 ATR 后应武装");
        check(std::fabs(b.atr_stop - 84.0) < 1e-9, "止损线应为 90 − 3×2 = 84");
        const int layers_at_arm = (int)b.entries.size();

        // 继续下跌穿过补仓位，但没到止损线 → 一层都不该加
        for (double p : {88.0, 86.0, 85.0}) {
            now += 3000; eng.tick("BTCUSDT", p);
        }
        check((int)eng.get_bots()[0].entries.size() == layers_at_arm,
              "武装后跌穿补仓位也绝不再补仓");
        check(eng.get_bots()[0].entries.size() < (size_t)c.max_entries,
              "  （确认此时确实还没满层，否则这条断言是空的）");
    }

    // ── 棘轮：止损线只上移不下移 ─────────────────────────────────────────────
    {
        std::printf("\n── 棘轮 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.use_atr_trail  = true;
        c.atr_trail_mult = 3.0;
        c.tp_pct         = 1e9;   // 关掉常规止盈，免得先被它平掉
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 100.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 94.0) < 1e-9, "初始线 94");

        now += 3000; eng.tick("BTCUSDT", 120.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 114.0) < 1e-9, "新高后线上移到 114");

        now += 3000; eng.tick("BTCUSDT", 116.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 114.0) < 1e-9, "回落时线不得下移");

        // ATR 放大也不能让线回退
        eng.update_atr(id, 10.0);
        now += 3000; eng.tick("BTCUSDT", 117.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 114.0) < 1e-9,
              "ATR 放大使候选线更低时，棘轮必须挡住");
    }

    // ── 触线出场 ─────────────────────────────────────────────────────────────
    {
        std::printf("\n── 触线平仓 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.use_atr_trail  = true;
        c.atr_trail_mult = 3.0;
        c.tp_pct         = 1e9;
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 120.0);      // 线 114
        const int before = fc->market_orders;

        fc->next_fill_price = 114.0;
        now += 3000; eng.tick("BTCUSDT", 114.0);
        check(fc->market_orders == before + 1, "触线应发一笔平仓单");
        check(fc->last_reduce_only, "平仓单必须是 reduceOnly");
        check(fc->last_side == "SELL", "多头平仓应为 SELL");
        check(eng.get_bots()[0].entries.empty(), "平仓后应无持仓");
    }

    // ── 取消勾选后梯子恢复 ───────────────────────────────────────────────────
    {
        std::printf("\n── 解除武装 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.use_atr_trail  = true;
        c.atr_trail_mult = 3.0;
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 100.0);
        check(eng.get_bots()[0].atr_armed, "已武装");

        c.use_atr_trail = false;
        eng.update_bot_cfg(id, c);
        now += 3000; eng.tick("BTCUSDT", 100.0);
        check(!eng.get_bots()[0].atr_armed, "取消勾选后应解除武装");

        fc->next_fill_price = 90.0;
        now += 3000; eng.tick("BTCUSDT", 90.0);
        check(eng.get_bots()[0].entries.size() == 2, "解除后梯子应恢复补仓");
    }

    // ── 空头镜像 ─────────────────────────────────────────────────────────────
    {
        std::printf("\n── 空头 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.direction      = CcgConfig::Direction::Short;
        c.use_atr_trail  = true;
        c.atr_trail_mult = 3.0;
        c.tp_pct         = 1e9;
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 100.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 106.0) < 1e-9,
              "空头初始线 = 100 + 3×2 = 106");

        now += 3000; eng.tick("BTCUSDT", 80.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 86.0) < 1e-9, "空头创新低后线下移");

        now += 3000; eng.tick("BTCUSDT", 84.0);
        check(std::fabs(eng.get_bots()[0].atr_stop - 86.0) < 1e-9, "空头反弹时线不得上移");
    }

    // ── 硬止损仍然优先 ───────────────────────────────────────────────────────
    {
        std::printf("\n── 硬止损优先级 ──\n");
        int64_t now = 1'700'000'000'000LL;
        auto fc = std::make_shared<FakeClient>();
        CcgEngine eng(fc, make_host(now));
        auto c = base_cfg();
        c.use_atr_trail  = true;
        c.atr_trail_mult = 3.0;
        c.stop_loss_pct  = 5.0;    // 均价下方 5%
        c.tp_pct         = 1e9;
        auto id = eng.add_bot(c);

        fc->next_fill_price = 100.0;
        eng.tick("BTCUSDT", 100.0);
        eng.update_atr(id, 2.0);
        now += 3000; eng.tick("BTCUSDT", 100.0);   // ATR 线 94，硬止损线 95

        fc->next_fill_price = 94.5;
        now += 3000; eng.tick("BTCUSDT", 94.5);    // 只触发硬止损（95），未到 ATR 线
        check(eng.get_bots()[0].entries.empty(), "硬止损应先于 ATR 线触发");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
