// SAR 持久化与对账测试。
//
// 这两条路径守的是同一个风险：**引擎以为自己空仓，而交易所上的仓位还在**。
// 一旦发生，下一个突破信号会再开一笔，净敞口翻倍，且原来那笔没有任何止损线
// 守着。DCA 版丢状态只是少了摊薄记录，SAR 版丢状态是直接的裸敞口。
#include "headless/sar_state.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

static void write_file(const std::string& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

static SarConfig mk_cfg(const std::string& sym) {
    SarConfig c;
    c.symbol = sym;
    c.budget_usdt = 500;
    return c;
}

static SarBot mk_bot(const std::string& sym, sar::Pos pos,
                     double entry, double peak, double stop, double qty) {
    SarBot b;
    b.cfg = mk_cfg(sym);
    b.st.pos = pos;
    b.st.entry_price = entry;
    b.st.peak = peak;
    b.st.stop = stop;
    b.qty = qty;
    return b;
}

// 假交易所客户端：SarEngine 的构造要一个，但对账测试不下任何单
class NullClient : public ITradingClient {
public:
    OrderOutcome place_market_order(const std::string&, const std::string&,
                                    double, bool) override { return {}; }
    double round_qty(const std::string&, double q) override { return q; }
    bool   set_leverage(const std::string&, int) override { return true; }
    bool   is_dual_mode() const override { return false; }
};

static EngineHost inline_host() {
    EngineHost h;
    h.submit = [](std::function<void()> fn) { fn(); };
    return h;
}

int main() {
    const std::string path = "sar_state_test.json";

    // ── 往返精度：止损线【就是】出场价，截断等于每次重启挪一次出场价 ─────────
    {
        // 这些值刻意取到 double 的有效位数边界。ostringstream 默认 6 位有效数字，
        // 不设精度的话 94310.4912345678 会存成 94310.5
        auto b = mk_bot("BTCUSDT", sar::Pos::Long,
                        94310.4912345678, 97221.1234567891, 91234.5678901234,
                        0.0318294715);
        b.realized_pnl = 1234.5678901234;
        b.trade_count = 7; b.win_count = 3;
        b.st.consec_reverses = 1; b.st.cooldown_left = 2;

        save_sar_state(path, {b});
        auto got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1, "往返：应读回 1 个 bot");
        if (got.size() == 1) {
            const auto& g = got[0];
            check(g.st.entry_price == b.st.entry_price, "  开仓价必须逐位无损");
            check(g.st.peak == b.st.peak,               "  极值必须逐位无损");
            check(g.st.stop == b.st.stop,               "  止损线必须逐位无损");
            check(g.qty == b.qty,                        "  持仓量必须逐位无损");
            check(g.realized_pnl == b.realized_pnl,      "  已实现盈亏必须逐位无损");
            check(g.st.pos == sar::Pos::Long,            "  方向");
            check(g.trade_count == 7 && g.win_count == 3, "  成交统计");
            check(g.st.consec_reverses == 1,             "  连续反手计数");
            check(g.st.cooldown_left == 2,               "  剩余冷却");
        }
    }

    // ── 金字塔状态往返 ───────────────────────────────────────────────────────
    // adds_done 丢了会让重启后【多加几档】（引擎以为一档都没加过）；
    // last_add_price 丢了会让下一档的间距从 0 量起，立刻再加一档
    {
        auto b = mk_bot("BTCUSDT", sar::Pos::Long, 100.0, 105.0, 99.0, 3.0);
        b.st.adds_done      = 2;
        b.st.last_add_price = 103.456789012345;
        save_sar_state(path, {b});
        auto got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1, "金字塔状态：读回 1 个 bot");
        if (got.size() == 1) {
            check(got[0].st.adds_done == 2, "  adds_done 往返");
            check(got[0].st.last_add_price == b.st.last_add_price,
                  "  last_add_price 逐位无损（下一档的间距从它量起）");
        }
    }

    // ── 空头往返 ─────────────────────────────────────────────────────────────
    {
        auto b = mk_bot("ETHUSDT", sar::Pos::Short, 3000.5, 2800.25, 2950.75, 1.5);
        save_sar_state(path, {b});
        auto got = load_sar_state(path, {mk_cfg("ETHUSDT")});
        check(got.size() == 1 && got[0].st.pos == sar::Pos::Short, "空头方向往返");
        check(got.size() == 1 && got[0].st.stop == 2950.75, "空头止损线往返");
    }

    // ── 配置里删掉的品种，落盘状态必须丢弃 ───────────────────────────────────
    {
        save_sar_state(path, {mk_bot("BTCUSDT", sar::Pos::Long, 100, 100, 94, 1),
                              mk_bot("ETHUSDT", sar::Pos::Long, 200, 200, 188, 2)});
        auto got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1 && got[0].cfg.symbol == "BTCUSDT",
              "配置里删掉的品种，落盘状态应被丢弃");
    }

    // ── 参数用最新配置，不用落盘的 ───────────────────────────────────────────
    {
        auto b = mk_bot("BTCUSDT", sar::Pos::Long, 100, 100, 94, 1);
        b.cfg.rule.atr_mult = 99.0;          // 落盘里是个旧值
        save_sar_state(path, {b});
        auto fresh = mk_cfg("BTCUSDT");
        fresh.rule.atr_mult = 2.5;           // 用户改了配置文件
        auto got = load_sar_state(path, {fresh});
        check(got.size() == 1 && std::fabs(got[0].cfg.rule.atr_mult - 2.5) < 1e-9,
              "策略参数必须取自最新配置，而不是落盘快照");
    }

    // ── 半截状态防御：有方向却没有止损线 ─────────────────────────────────────
    {
        // stop=0 时多头的 `price <= stop` 永远不成立，仓位会一直裸着没人管——
        // 比"空仓"危险得多。应当当成空仓丢弃，交给对账去发现真实仓位
        write_file(path, "[{\"symbol\":\"BTCUSDT\",\"pos\":1,\"entry_price\":100,"
                         "\"peak\":100,\"stop\":0,\"qty\":1}]");
        auto got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1 && got[0].st.pos == sar::Pos::Flat,
              "止损线为0的持仓状态应被丢弃为空仓");

        write_file(path, "[{\"symbol\":\"BTCUSDT\",\"pos\":1,\"entry_price\":0,"
                         "\"peak\":100,\"stop\":94,\"qty\":1}]");
        got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1 && got[0].st.pos == sar::Pos::Flat,
              "开仓价为0的持仓状态应被丢弃为空仓");

        write_file(path, "[{\"symbol\":\"BTCUSDT\",\"pos\":1,\"entry_price\":100,"
                         "\"peak\":100,\"stop\":94,\"qty\":0}]");
        got = load_sar_state(path, {mk_cfg("BTCUSDT")});
        check(got.size() == 1 && got[0].st.pos == sar::Pos::Flat,
              "数量为0的持仓状态应被丢弃为空仓");
    }

    // ── 损坏/缺失文件不崩溃 ──────────────────────────────────────────────────
    {
        write_file(path, "{ 这不是 json");
        check(load_sar_state(path, {mk_cfg("BTCUSDT")}).empty(), "损坏 JSON 返回空");
        write_file(path, "");
        check(load_sar_state(path, {mk_cfg("BTCUSDT")}).empty(), "空文件返回空");
        std::remove(path.c_str());
        check(load_sar_state(path, {mk_cfg("BTCUSDT")}).empty(), "文件不存在返回空");
    }

    // ═══ 对账 ════════════════════════════════════════════════════════════════
    auto cli = std::make_shared<NullClient>();

    // ── 两边都空 = 一致 ──────────────────────────────────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.add_bot(mk_cfg("BTCUSDT"));
        check(eng.reconcile_positions({}).empty(), "两边都空仓应无不一致");
    }

    // ── 孤儿仓：本地空、交易所有 → 必须停 bot ────────────────────────────────
    {
        // 这是最危险的一种：不停的话，下一个突破信号会再开一笔，
        // 而交易所上那笔无人管理
        SarEngine eng(cli, inline_host());
        eng.add_bot(mk_cfg("BTCUSDT"));
        auto issues = eng.reconcile_positions({{"BTCUSDT", 1, 0.5, 100.0}});
        check(issues.size() == 1, "孤儿仓应报告 1 处不一致");
        check(eng.get_bots()[0].state == SarBot::State::Stopped,
              "孤儿仓必须停止该bot，否则会再开一笔造成双倍敞口");
    }

    // ── 本地有仓、交易所没有 → 清空并停止 ────────────────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 0.5));
        auto issues = eng.reconcile_positions({});
        check(issues.size() == 1, "本地有仓交易所没有应报告不一致");
        auto b = eng.get_bots()[0];
        check(b.st.pos == sar::Pos::Flat && b.qty == 0, "应清空本地仓位");
        check(b.state == SarBot::State::Stopped,
              "应停止该bot——分不清是人工平的还是被强平的，后者继续开仓是往坑里跳");
    }

    // ── 外部部分平仓 → 数量收敛，止损线与开仓价保留 ──────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 1.0));
        auto issues = eng.reconcile_positions({{"BTCUSDT", 1, 0.4, 100.0}});
        check(issues.size() == 1, "部分平仓应报告不一致");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - 0.4) < 1e-12, "数量应收敛到交易所值");
        check(std::fabs(b.st.stop - 104.0) < 1e-12, "止损线应保留（它仍然成立）");
        check(std::fabs(b.st.entry_price - 100.0) < 1e-12, "开仓价应保留");
        check(b.state == SarBot::State::Running, "部分平仓不该停bot");
    }

    // ── 交易所比本地多 → 只告警，不动本地 ────────────────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 0.5));
        auto issues = eng.reconcile_positions({{"BTCUSDT", 1, 2.0, 100.0}});
        check(issues.size() == 1, "交易所多出应报告不一致");
        auto b = eng.get_bots()[0];
        check(std::fabs(b.qty - 0.5) < 1e-12,
              "交易所多出时不得改动本地——那笔不是自己开的，止损线基准不成立");
        check(b.state == SarBot::State::Running, "仅告警，不停bot");
    }

    // ── 方向不一致 → 停 bot（任何自动收敛都是在猜）───────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 0.5));
        auto issues = eng.reconcile_positions({{"BTCUSDT", -1, 0.5, 100.0}});
        check(issues.size() == 1, "方向不一致应报告");
        check(eng.get_bots()[0].state == SarBot::State::Stopped, "方向不一致应停bot");
    }

    // ── 在途的 bot 必须跳过对账 ──────────────────────────────────────────────
    {
        // 订单可能已在交易所生效而本地还没入账，此刻比对必然误判——
        // 正在开仓的会被当成孤儿仓而停掉
        SarEngine eng(cli, inline_host());
        auto id = eng.add_bot(mk_cfg("BTCUSDT"));
        eng.set_pending_for_test(id, true);
        auto issues = eng.reconcile_positions({{"BTCUSDT", 1, 0.5, 100.0}});
        check(issues.empty(), "在途的 bot 必须跳过，否则正在开仓的会被误判为孤儿仓");
        check(eng.get_bots()[0].state == SarBot::State::Running, "  且不得被停掉");
    }

    // ── restore_bot：信号一律作废，但止损线保留 ──────────────────────────────
    {
        SarEngine eng(cli, inline_host());
        auto b = mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 0.5);
        b.sig_ok = true; b.atr = 2.0; b.dc_ok = true;
        b.bar_open_ms = 12345; b.last_counted_bar_ms = 12345;
        b.pending = true;
        eng.restore_bot(b);
        auto g = eng.get_bots()[0];
        check(!g.sig_ok && g.atr == 0 && !g.dc_ok,
              "落盘的信号必须作废重拉（ATR 可能是几小时前的）");
        check(std::fabs(g.st.stop - 104.0) < 1e-12,
              "止损线必须保留——它是重启后唯一还护着仓位的东西");
        check(!g.pending, "落盘的在途标记必须作废，否则该bot永久冻结");
        check(g.bar_open_ms == 0 && g.last_counted_bar_ms == 0,
              "K线计数跨重启失去意义，应清零");
    }

    // ── restore_bot 同品种重复应被拒 ─────────────────────────────────────────
    {
        SarEngine eng(cli, inline_host());
        eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 100, 110, 104, 0.5));
        check(eng.restore_bot(mk_bot("BTCUSDT", sar::Pos::Long, 1, 1, 1, 1)).empty(),
              "同品种重复恢复应被拒");
        check(eng.add_bot(mk_cfg("BTCUSDT")).empty(),
              "已恢复的品种再 add_bot 应被拒（否则会开出第二笔）");
    }

    std::remove(path.c_str());
    if (g_fail == 0) {
        std::printf("\nOK: SAR 持久化与对账测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "\n共 %d 条断言失败\n", g_fail);
    return 1;
}
