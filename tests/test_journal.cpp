// 数据记录层测试：CSV 格式、表头只写一次、按日文件、过期清理、字段转义。
#include "core/journal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ccbot;
namespace fs = std::filesystem;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static std::vector<std::string> lines_of(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream f(p, std::ios::binary);
    std::string l;
    while (std::getline(f, l)) out.push_back(l);
    return out;
}

static std::chrono::system_clock::time_point at(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "ccg_journal_test";
    fs::remove_all(dir);
    // 2026-10-10 08:00:00 UTC
    const int64_t t0 = 1791619200000LL;

    check(Journal::utc_time(t0) == "2026-10-10 08:00:00", "UTC 时间格式");
    check(Journal::utc_date(t0) == "20261010", "UTC 日期格式");
    check(Journal::csv_field("abc") == "abc", "普通字段原样");
    check(Journal::csv_field("a,b") == "\"a,b\"", "含逗号加引号");
    check(Journal::csv_field("说\"明\"") == "\"说\"\"明\"\"\"", "引号转义成两个");

    Journal j(dir.string(), 60);

    // ── 成交 ──
    TrendTrade t;
    t.symbol = "AAAUSDT"; t.side = trend::Pos::Long;
    t.entry_price = 100; t.exit_price = 110; t.qty = 2; t.pnl = 20;
    t.reason = "追踪止盈出场，等待新信号";
    t.open_time = at(t0); t.close_time = at(t0 + 5 * 3600000LL);
    t.is_auto = true; t.seg = smart::kSegStrong; t.rank = 3; t.score = 72.5;
    t.entry_dist_atr = 0.8; t.interval = "1h";
    t.mfe_usd = 30; t.mae_usd = -8; t.mfe_pct = 15; t.mae_pct = -4;
    t.mfe_atr = 7.5; t.mae_atr = -2; t.mfe_r = 2.5; t.mae_r = -0.6667;
    j.trade(t);
    TrendTrade t2 = t;
    t2.symbol = "BBBUSDT"; t2.entry_dist_atr = std::numeric_limits<double>::quiet_NaN();
    t2.mfe_atr = t2.mae_atr = t2.mfe_r = t2.mae_r = std::numeric_limits<double>::quiet_NaN();
    t2.reason = "原因里有,英文逗号";
    j.trade(t2);
    auto tl = lines_of(dir / "trades.csv");
    check(tl.size() == 3, "trades.csv：表头 1 行 + 2 笔（表头只写一次）");
    if (tl.size() == 3) {
        check(tl[0].rfind("\xEF\xBB\xBF", 0) == 0 && has(tl[0], "入场离通道边沿ATR"), "  带 BOM 的中文表头");
        check(has(tl[1], "AAAUSDT") && has(tl[1], "强势") && has(tl[1], ",3,") &&
              has(tl[1], "72.5") && has(tl[1], ",0.8,") && has(tl[1], "自动"),
              "  开仓时所在段、名次、分数、入场离边沿 ATR 都写进去了");
        check(has(tl[1], "2026-10-10 13:00:00,2026-10-10 08:00:00,5,"), "  平仓/开仓时间与持仓小时");
        // 估算手续费 (100+110)*2*0.0005 = 0.21，净盈亏 19.79
        check(has(tl[1], ",0.21,19.79,"), "  估算手续费与净盈亏");
        check(has(tl[2], "\"原因里有,英文逗号\""), "  原因含逗号：加引号，不串列");
        check(has(tl[2], "19.79,,30,"), "  入场离边沿拿不到（NaN）：留空，不写 nan");
        check(has(tl[0], "浮盈亏最高U,浮盈亏最低U,浮盈亏最高%,浮盈亏最低%,浮盈亏最高ATR,"
                         "浮盈亏最低ATR,浮盈亏最高R,浮盈亏最低R,原因"), "  浮盈亏最高/最低四种单位的表头");
        check(has(tl[1], ",0.8,30,-8,15,-4,7.5,-2,2.5,-0.6667,"), "  浮盈亏最高/最低：美元、%、ATR、R");
        check(has(tl[2], ",30,-8,15,-4,,,,,\""), "  ATR / R 算不了（NaN）：留空");
    }

    // ── 过滤 / 过期清理 ──
    std::ofstream(dir / "scan-20200101.csv") << "old";
    std::ofstream(dir / "filter-20200101.csv") << "old";
    std::ofstream(dir / "scan-20261001.csv") << "recent";
    FilterEvent fe;
    fe.time = at(t0); fe.symbol = "CCCUSDT"; fe.is_long = false; fe.category = "量比";
    fe.price = 1.23; fe.detail = "CCCUSDT 向下突破待放量：量比 0.8x（需≥1.5）";
    j.filter(fe);
    j.filter(fe);
    auto fl = lines_of(dir / "filter-20261010.csv");
    check(fl.size() == 3 && has(fl[1], "向下") && has(fl[1], "量比"), "filter 按 UTC 日分文件，表头一次");
    check(!fs::exists(dir / "scan-20200101.csv") && !fs::exists(dir / "filter-20200101.csv"),
          "超过保留天数的按日文件被清理");
    check(fs::exists(dir / "scan-20261001.csv"), "  保留期内的不动");

    // ── 候选池 ──
    j.pool(t0, "DDDUSDT", "加入", smart::kSegHot, 1, 40, "仅空");
    j.pool(t0, "DDDUSDT", "移出", 0, 0, 0, "连续 2 轮掉出名单");
    auto pl = lines_of(dir / "pool.csv");
    check(pl.size() == 3 && has(pl[1], "过热") && has(pl[2], "掉出名单"), "pool.csv：加入 / 移出事件");

    // ── 扫描快照 ──
    smart::Ranking rk;
    smart::Scored s;
    s.symbol = "EEEUSDT"; s.long_score = 80; s.rs_pct = 90;
    s.f.ok = true; s.f.rs = 1.2; s.f.close = 0.01234; s.f.max_up24 = 12;
    rk.rows["EEEUSDT"] = s;
    smart::Scored s2 = s; s2.symbol = "FFFUSDT"; s2.f.rs = 0;
    rk.rows["FFFUSDT"] = s2;
    smart::Segmented sg;
    sg.seg["EEEUSDT"] = smart::kSegStrong;
    sg.seg["FFFUSDT"] = smart::kSegMid;
    sg.strong = {"EEEUSDT"};
    j.scan(t0, rk, sg, {"EEEUSDT"});
    auto sl = lines_of(dir / "scan-20261010.csv");
    check(sl.size() == 3, "scan 快照：全部合格币各一行（不只是进名单的）");
    if (sl.size() == 3) {
        check(has(sl[1], "EEEUSDT,强势,1,") && has(sl[1], "0.01234") && has(sl[1], ",是"),
              "  所在段、段内名次、收盘价、已有 bot");
        check(has(sl[2], "FFFUSDT,中间,,"), "  分了段但没进名单：名次留空");
    }

    fs::remove_all(dir);
    if (g_fail == 0) {
        std::printf("OK: 数据记录层测试全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "共 %d 条断言失败\n", g_fail);
    return 1;
}
