// 状态持久化的往返测试（headless 侧）。
//
// 存在的理由：这 433 行（headless_state + headless_config）此前【零测试覆盖】，
// 而它决定的是"重启之后仓位对不对"。落盘/读回哪怕错一个字段，后果都是
// 本地记的仓位与交易所脱节——而这种错误不会立刻报错，会安静地一直错下去，
// 直到某次止盈算错价、或者对账把仓位判成孤儿。
//
// 判据取【逐字段往返相等】而不是"能读回来就行"：
// 少存一个字段、精度被截断、类型转换丢信息，都必须在这里现形。
#include "headless/headless_state.h"

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
// 相对误差判据：价格量级从 1e-8（meme币）到 1e5（BTC）跨 13 个数量级，
// 绝对误差没有意义
static void check_rel(double got, double want, double tol_rel, const std::string& what) {
    double denom = std::fabs(want) > 1e-12 ? std::fabs(want) : 1.0;
    double rel = std::fabs(got - want) / denom;
    bool ok = rel <= tol_rel;
    if (ok) std::printf("[ OK ]  %s\n", what.c_str());
    else    std::printf("[FAIL]  %s  期望 %.12g，实际 %.12g（相对误差 %.3g）\n",
                        what.c_str(), want, got, rel);
    if (!ok) ++g_fail;
}

static CcgConfig make_cfg(const std::string& sym, CcgConfig::Direction d) {
    CcgConfig c;
    c.symbol    = sym;
    c.direction = d;
    return c;
}

int main() {
    const std::string path = "test_state_roundtrip.json";
    std::remove(path.c_str());

    // ── 构造一个"很难存对"的 bot：价格量级大、有效位多、字段全部非默认 ──────
    CcgBot b;
    b.bot_id           = "BTCUSDT_L_7";
    b.cfg              = make_cfg("BTCUSDT", CcgConfig::Direction::Long);
    b.state            = CcgBot::State::Cooldown;
    // ⚠ 这三个必须【互相自洽】地构造：成本 = 均价 × 数量。
    //   各自随手编一个数的话，第③组自洽性断言测的就是"我编的数不自洽"，
    //   而不是"序列化有没有破坏自洽" —— 第一版就是这么写错的
    b.avg_price        = 94310.4912345678;    // 12 位有效数字，BTC 真实量级
    b.total_qty        = 0.0318294715;
    b.total_cost       = b.avg_price * b.total_qty;
    b.current_price    = 93877.1234567;
    b.last_entry_price = 93012.8765432;
    b.dca_extreme      = 92455.5555555;
    b.tp_extreme       = 96123.9876543;
    b.interval_hit     = true;
    b.tp_reached       = true;
    b.ind_dipped       = true;
    b.realized_pnl     = 1234.56789012;
    b.cycle_count      = 17;
    // 满层健康度：漏存的话重启后统计静默归零，而它是品种健康的唯一判据
    b.full_layer_secs  = 123456;
    b.alive_secs       = 987654;
    b.cooldown_until   = std::chrono::system_clock::time_point(
                             std::chrono::milliseconds(1787156044876LL));
    b.disaster_stop_id    = "9876543210";
    b.disaster_stop_price = 66017.3456789;

    for (int i = 0; i < 3; ++i) {
        CcgEntry e;
        e.level     = i;
        e.price     = 94000.1234567 - i * 777.7654321;
        e.qty       = 0.0106098238 + i * 0.0001234567;
        e.cost_usdt = e.price * e.qty;
        e.order_id  = "ORD" + std::to_string(1000000000LL + i);
        e.time      = std::chrono::system_clock::time_point(
                          std::chrono::milliseconds(1787156000000LL + i * 60000));
        b.entries.push_back(e);
    }

    std::vector<CcgConfig> cfgs = { b.cfg };

    save_headless_state(path, { b });
    auto loaded = load_headless_state(path, cfgs);

    check(loaded.size() == 1, "读回 1 个 bot");
    if (loaded.empty()) { std::printf("\n无法继续\n"); return 1; }
    const CcgBot& r = loaded[0];

    // ── ① 标量字段逐个往返 ──────────────────────────────────────────────────
    // 容差取 1e-12：double 本身有 15~17 位有效数字，一个诚实的序列化应该能
    // 无损往返。放宽到 1e-6 就等于默许"存个大概"，而这正是要抓的问题
    const double TOL = 1e-12;
    check(r.state == CcgBot::State::Cooldown,          "state 往返");
    check_rel(r.avg_price,        b.avg_price,        TOL, "avg_price 往返");
    check_rel(r.total_qty,        b.total_qty,        TOL, "total_qty 往返");
    check_rel(r.total_cost,       b.total_cost,       TOL, "total_cost 往返");
    check_rel(r.current_price,    b.current_price,    TOL, "current_price 往返");
    check_rel(r.last_entry_price, b.last_entry_price, TOL, "last_entry_price 往返");
    check_rel(r.dca_extreme,      b.dca_extreme,      TOL, "dca_extreme 往返");
    check_rel(r.tp_extreme,       b.tp_extreme,       TOL, "tp_extreme 往返");
    check_rel(r.realized_pnl,     b.realized_pnl,     TOL, "realized_pnl 往返");
    check_rel(r.disaster_stop_price, b.disaster_stop_price, TOL, "disaster_stop_price 往返");
    check(r.interval_hit == true,  "interval_hit 往返");
    check(r.tp_reached   == true,  "tp_reached 往返");
    check(r.ind_dipped   == true,  "ind_dipped 往返");
    check(r.cycle_count  == 17,    "cycle_count 往返");
    check(r.full_layer_secs == 123456, "full_layer_secs 往返");
    check(r.alive_secs      == 987654, "alive_secs 往返");
    // 比值也要对：两个字段各自往返对但配错了同样是错的
    check(std::fabs(r.full_layer_pct() - 100.0*123456/987654) < 1e-9,
          "  满层占比据此算出的值一致");
    check(r.disaster_stop_id == "9876543210", "disaster_stop_id 往返");
    check(r.cooldown_until == b.cooldown_until, "cooldown_until 往返（毫秒级精确）");

    // ── ② 加仓记录逐条往返 ──────────────────────────────────────────────────
    check(r.entries.size() == 3, "加仓记录条数往返");
    if (r.entries.size() == 3) {
        for (size_t i = 0; i < 3; ++i) {
            const auto& g = r.entries[i]; const auto& w = b.entries[i];
            std::string tag = "  第" + std::to_string(i + 1) + "层 ";
            check(g.level == w.level,        tag + "level");
            check_rel(g.price,     w.price,     TOL, tag + "price");
            check_rel(g.qty,       w.qty,       TOL, tag + "qty");
            check_rel(g.cost_usdt, w.cost_usdt, TOL, tag + "cost_usdt");
            check(g.order_id == w.order_id,  tag + "order_id");
            check(g.time == w.time,          tag + "time（毫秒级精确）");
        }
    }

    // ── ③ 自洽性：均价必须与 成本/数量 对得上 ───────────────────────────────
    // 这三个字段是【各自独立】存取的，任何一个被截断都会让它们互相矛盾——
    // 而引擎下游的止盈线、保底线、回本价全都建立在它们自洽的前提上
    if (r.total_qty > 0) {
        double implied = r.total_cost / r.total_qty;
        check_rel(implied, r.avg_price, 1e-9,
                  "读回后 均价 == 成本/数量（三字段互相自洽）");
    }

    // ── ④ 极端量级：meme 币的微价 与 大数量 ─────────────────────────────────
    {
        CcgBot m;
        m.cfg = make_cfg("PEPEUSDT", CcgConfig::Direction::Long);
        m.avg_price  = 0.00000123456789;
        m.total_qty  = 987654321.125;
        m.total_cost = m.avg_price * m.total_qty;
        std::vector<CcgConfig> mc = { m.cfg };
        const std::string p2 = "test_state_meme.json";
        save_headless_state(p2, { m });
        auto lm = load_headless_state(p2, mc);
        check(lm.size() == 1, "极端量级：读回 1 个 bot");
        if (!lm.empty()) {
            check_rel(lm[0].avg_price,  m.avg_price,  TOL, "  微价 avg_price 往返");
            check_rel(lm[0].total_qty,  m.total_qty,  TOL, "  大数量 total_qty 往返");
            check_rel(lm[0].total_cost, m.total_cost, TOL, "  total_cost 往返");
        }
        std::remove(p2.c_str());
    }

    // ── ⑤ 损坏文件不能让好状态消失得无声无息 ────────────────────────────────
    // 半截 JSON（写盘途中崩溃）目前会让整个文件解析失败 → 返回空 → 全部仓位
    // 静默丢失。原子写盘已经在防这件事，这里确认解析器至少不会崩
    {
        const std::string p3 = "test_state_broken.json";
        { std::ofstream f(p3, std::ios::binary); f << "[{\"symbol\":\"BTCUSDT\",\"avg_pri"; }
        auto lb = load_headless_state(p3, cfgs);
        check(lb.empty(), "半截 JSON 不崩溃（返回空）");
        std::remove(p3.c_str());
    }

    // ── ⑥ 配置里已删除的品种，落盘状态应当被丢弃 ────────────────────────────
    {
        std::vector<CcgConfig> other = { make_cfg("ETHUSDT", CcgConfig::Direction::Long) };
        auto lo = load_headless_state(path, other);
        check(lo.empty(), "配置里没有的品种，落盘状态被丢弃");
    }

    std::remove(path.c_str());
    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
