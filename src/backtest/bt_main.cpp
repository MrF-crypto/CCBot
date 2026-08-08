// ccbot_backtest：回测工具（P0 数据层阶段）
// 用法：
//   ccbot_backtest scan   <数据目录>            扫描全部CSV，输出数据集总览
//   ccbot_backtest check  <CSV文件>             单品种详细质检报告
//   ccbot_backtest cache  <CSV文件> <缓存路径>  解析并写二进制缓存（后续回放用）
#include "backtest/bt_data.h"
#include "backtest/bt_replay.h"
#include "backtest/bt_portfolio.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

using namespace ccbot::bt;
using ccbot::CcgConfig;
namespace fs = std::filesystem;

static void usage() {
    std::cout <<
        "ccbot_backtest — 回测工具\n"
        "  scan  <数据目录>             扫描全部CSV，输出数据集总览\n"
        "  check <CSV文件>              单品种详细质检报告\n"
        "  cache <CSV文件> <缓存路径>   解析并写二进制缓存\n"
        "  run   <CSV或缓存> [选项]     单组参数回测\n"
        "        --budget N --lev N --layers N --curve linear|flat|martingale\n"
        "        --dynamic --trend --sr --gates --indicator\n"
        "        --floor N --htf-max N --headroom N --verbose\n";
}

// 命令行参数小工具
static std::string arg_str(int argc, char** argv, const std::string& key, const std::string& def) {
    for (int i = 0; i < argc - 1; ++i) if (key == argv[i]) return argv[i + 1];
    return def;
}
static double arg_num(int argc, char** argv, const std::string& key, double def) {
    std::string s = arg_str(argc, argv, key, "");
    if (s.empty()) return def;
    try { return std::stod(s); } catch (...) { return def; }
}
static bool arg_flag(int argc, char** argv, const std::string& key) {
    for (int i = 0; i < argc; ++i) if (key == argv[i]) return true;
    return false;
}

static int cmd_run(int argc, char** argv) {
    std::string path = argv[2];
    Series s; QualityReport rep; std::string err;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".bin") {
        if (!load_cache(path, s)) { std::cerr << "读缓存失败\n"; return 1; }
    } else if (!load_csv(path, s, rep, err)) {
        std::cerr << "失败: " << err << "\n"; return 1;
    }

    ReplayOptions opt;
    opt.initial_equity = arg_num(argc, argv, "--equity", 10000);
    opt.verbose        = arg_flag(argc, argv, "--verbose");

    auto& c = opt.cfg;
    c.symbol       = s.symbol;
    c.direction    = CcgConfig::Direction::Long;
    c.budget_usdt  = arg_num(argc, argv, "--budget", 1000);
    c.leverage     = (int)arg_num(argc, argv, "--lev", 3);
    c.max_entries  = (int)arg_num(argc, argv, "--layers", 6);
    c.interval_pct = arg_num(argc, argv, "--interval", 1.0);
    c.trail_entry  = arg_num(argc, argv, "--trail-entry", 0.2);
    c.tp_pct       = arg_num(argc, argv, "--tp", 1.5);
    c.trail_tp     = arg_num(argc, argv, "--trail-tp", 0.3);
    c.stop_loss_pct= arg_num(argc, argv, "--sl", 0);
    c.cooldown_secs= (int)arg_num(argc, argv, "--cooldown", 300);
    c.auto_restart = true;

    std::string curve = arg_str(argc, argv, "--curve", "linear");
    c.strat_type = curve == "flat"       ? CcgConfig::StratType::Flat
                 : curve == "martingale" ? CcgConfig::StratType::Martingale
                 : curve == "square"     ? CcgConfig::StratType::Square
                 : curve == "fibonacci"  ? CcgConfig::StratType::Fibonacci
                                         : CcgConfig::StratType::Linear;

    if (arg_flag(argc, argv, "--indicator")) {
        c.entry_mode = CcgConfig::EntryMode::Indicator;
        c.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;
    }
    c.kline_interval  = arg_str(argc, argv, "--tf", "1h");
    c.rsi_threshold   = arg_num(argc, argv, "--rsi-th", 35);
    c.rsi_oversold_th = arg_num(argc, argv, "--rsi-os", 25);

    c.dynamic_band_mode = arg_flag(argc, argv, "--dynamic");
    c.min_profit_floor  = arg_num(argc, argv, "--floor", 0.3);
    c.use_trend_filter  = arg_flag(argc, argv, "--trend");
    c.sr_radar          = arg_flag(argc, argv, "--sr") || arg_flag(argc, argv, "--gates");
    c.smart_gates       = arg_flag(argc, argv, "--gates");
    c.htf_pos_max       = arg_num(argc, argv, "--htf-max", 0.80);
    c.sr_headroom_ratio = arg_num(argc, argv, "--headroom", 1.5);
    c.use_sr_exit          = arg_flag(argc, argv, "--sr-exit");
    c.use_structural_stop  = arg_flag(argc, argv, "--struct-stop");

    std::cout << "回测 " << s.symbol << "  " << s.bars.size() << " 根1m  "
              << ts_to_str(s.bars.front().ts_ms) << " ~ " << ts_to_str(s.bars.back().ts_ms) << "\n"
              << "参数: 预算" << c.budget_usdt << " 杠杆" << c.leverage << " 层数" << c.max_entries
              << " 曲线" << curve
              << (c.dynamic_band_mode ? " 动态W" : "")
              << (c.use_trend_filter ? " 趋势" : "")
              << (c.smart_gates ? " 三层拦截" : (c.sr_radar ? " SR" : ""))
              << (c.entry_mode == CcgConfig::EntryMode::Indicator ? " 指标首单" : " 立即首单")
              << "\n\n";

    auto t0 = std::chrono::steady_clock::now();
    auto r = run_replay(s, opt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    std::cout << r.to_text() << "  回放耗时  : " << ms << " ms\n";
    return 0;
}

static int cmd_check(const std::string& path) {
    Series s; QualityReport rep; std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!load_csv(path, s, rep, err)) { std::cerr << "失败: " << err << "\n"; return 1; }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    std::cout << rep.to_text() << "  解析耗时  : " << ms << " ms\n";

    // 聚合抽查：1m→1h/4h/1d 的根数应与时间跨度吻合
    for (const char* iv : {"1h", "4h", "1d"}) {
        auto agg = aggregate(s.bars, interval_ms(iv));
        std::cout << "  聚合 " << iv << "    : " << agg.size() << " 根";
        if (!agg.empty())
            std::cout << "  首根 " << ts_to_str(agg.front().ts_ms)
                      << "  末根 " << ts_to_str(agg.back().ts_ms);
        std::cout << "\n";
    }
    // 价差统计（滑点模型的输入）。数据里的 Spread 已是比例值，直接×10000转基点
    double sp_sum = 0; size_t sp_n = 0;
    for (const auto& b : s.bars) if (b.spread > 0) {
        sp_sum += b.spread * 10000.0; ++sp_n;
    }
    if (sp_n) std::cout << "  平均价差  : " << std::fixed << std::setprecision(2)
                        << (sp_sum / sp_n) << " bp（滑点模型用真实值）\n";
    return 0;
}

static int cmd_scan(const std::string& dir) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".csv") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::cerr << "目录下没有CSV: " << dir << "\n"; return 1; }

    std::cout << "扫描 " << files.size() << " 个文件...\n\n";
    size_t ok = 0, fail = 0, total_bars = 0, total_gaps = 0, total_bad = 0;
    int64_t gmin = 0, gmax = 0;
    std::vector<std::pair<std::string,size_t>> worst;   // 缺口最多的品种

    for (const auto& p : files) {
        Series s; QualityReport rep; std::string err;
        if (!load_csv(p.string(), s, rep, err)) {
            ++fail;
            std::cout << "[FAIL] " << p.filename().string() << " : " << err << "\n";
            continue;
        }
        ++ok;
        total_bars += rep.total_bars;
        total_gaps += rep.gap_count;
        total_bad  += rep.bad_ohlc + rep.dup_ts;
        if (gmin == 0 || rep.first_ts < gmin) gmin = rep.first_ts;
        if (rep.last_ts > gmax) gmax = rep.last_ts;
        if (rep.gap_minutes > 0) worst.emplace_back(rep.symbol, rep.gap_minutes);
        if (ok % 50 == 0) std::cout << "  已处理 " << ok << " / " << files.size() << "\n";
    }

    std::sort(worst.begin(), worst.end(),
              [](auto& a, auto& b){ return a.second > b.second; });

    std::cout << "\n================ 数据集总览 ================\n"
              << "  品种数    : " << ok << " 成功 / " << fail << " 失败\n"
              << "  K线总数   : " << total_bars << "\n"
              << "  时间范围  : " << ts_to_str(gmin) << " ~ " << ts_to_str(gmax) << " (UTC)\n"
              << "  缺口段总数: " << total_gaps << "\n"
              << "  坏点剔除  : " << total_bad << "\n";
    if (!worst.empty()) {
        std::cout << "  缺口最多的品种（前10）:\n";
        for (size_t i = 0; i < worst.size() && i < 10; ++i)
            std::cout << "    " << worst[i].first << "  缺 " << worst[i].second << " 分钟\n";
    }
    std::cout << "  完整品种  : " << (ok - worst.size()) << " 个无任何缺口\n";
    return 0;
}

static int cmd_cache(const std::string& csv, const std::string& out) {
    Series s; QualityReport rep; std::string err;
    if (!load_csv(csv, s, rep, err)) { std::cerr << "失败: " << err << "\n"; return 1; }
    if (!save_cache(out, s)) { std::cerr << "写缓存失败: " << out << "\n"; return 1; }
    Series verify;
    if (!load_cache(out, verify) || verify.bars.size() != s.bars.size()) {
        std::cerr << "缓存回读校验失败\n"; return 1;
    }
    std::cout << "缓存已写入: " << out << "  (" << s.bars.size() << " 根, "
              << (s.bars.size() * sizeof(Bar) / 1024 / 1024) << " MB)  回读校验通过\n";
    return 0;
}

// ── sweep：多品种 × 参数网格并行寻优 ─────────────────────────────────────────
// 用法：ccbot_backtest sweep <数据根目录> --symbols BTC-USDT,ETH-USDT,...
//        [--htf 0.6,0.7,0.8,0.9,1.1] [--head 1.0,1.5,2.0,3.0] [--baseline]
static std::vector<double> parse_list(const std::string& s) {
    std::vector<double> v; std::string cur;
    for (char c : s + ",") {
        if (c == ',') { if (!cur.empty()) { try { v.push_back(std::stod(cur)); } catch(...){} cur.clear(); } }
        else cur += c;
    }
    return v;
}
static std::vector<std::string> parse_slist(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s + ",") {
        if (c == ',') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    return v;
}

static int cmd_sweep(int argc, char** argv) {
    std::string root = argv[2];
    auto syms = parse_slist(arg_str(argc, argv, "--symbols", "BTC-USDT,ETH-USDT,SOL-USDT,BNB-USDT"));
    auto htf_list  = parse_list(arg_str(argc, argv, "--htf",  "0.60,0.70,0.80,0.90,1.10"));
    auto head_list = parse_list(arg_str(argc, argv, "--head", "1.0,1.5,2.0,3.0"));
    const bool with_baseline = true;
    const double budget = arg_num(argc, argv, "--budget", 1000);
    const int    lev    = (int)arg_num(argc, argv, "--lev", 3);
    const int    layers = (int)arg_num(argc, argv, "--layers", 6);

    // 加载数据（跨年目录自动合并）
    struct Loaded { std::string sym; Series s; };
    std::vector<Loaded> data;
    for (const auto& sym : syms) {
        std::vector<std::string> files;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().filename().string() == sym + ".csv")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
        if (files.empty()) { std::cout << "[跳过] 找不到 " << sym << "\n"; continue; }
        Series s; QualityReport rep; std::string err;
        if (!load_csv_multi(files, s, rep, err)) { std::cout << "[失败] " << sym << ": " << err << "\n"; continue; }
        std::cout << "加载 " << s.symbol << "  " << s.bars.size() << " 根  "
                  << ts_to_str(rep.first_ts) << " ~ " << ts_to_str(rep.last_ts)
                  << "  缺口 " << rep.gap_minutes << " 分钟  (" << files.size() << " 个文件)\n";
        data.push_back({sym, std::move(s)});
    }
    if (data.empty()) { std::cerr << "没有可用数据\n"; return 1; }

    // 参数组合（基线 = 不启用三层拦截，用于对照）
    struct Combo { bool gates; double htf; double head; };
    std::vector<Combo> combos;
    if (with_baseline) combos.push_back({false, 0, 0});
    for (double h : htf_list) for (double hr : head_list) combos.push_back({true, h, hr});

    std::cout << "\n组合数 " << combos.size() << " × 品种 " << data.size()
              << " = " << combos.size() * data.size() << " 次回放，并行计算中...\n\n";

    struct Row { Combo c; std::string sym; BacktestResult r; };
    std::vector<Row> rows(combos.size() * data.size());
    std::atomic<size_t> next{0};
    const unsigned nth = std::max(2u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nth; ++t) {
        pool.emplace_back([&]{
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= rows.size()) return;
                const auto& c = combos[i / data.size()];
                const auto& d = data[i % data.size()];
                ReplayOptions o;
                o.initial_equity = 10000;
                auto& cf = o.cfg;
                cf.symbol = d.s.symbol;
                cf.direction = CcgConfig::Direction::Long;
                cf.budget_usdt = budget; cf.leverage = lev; cf.max_entries = layers;
                cf.strat_type = CcgConfig::StratType::Linear;
                cf.cooldown_secs = 300; cf.auto_restart = true;
                cf.entry_mode = CcgConfig::EntryMode::Indicator;      // 指标首单
                cf.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;  // RSI反转
                cf.rsi_threshold = 35; cf.rsi_oversold_th = 25;
                cf.kline_interval = "1h";
                cf.dynamic_band_mode = true;                          // 动态W
                cf.use_trend_filter  = true;
                cf.sr_radar = true;
                cf.smart_gates = c.gates;
                if (c.gates) { cf.htf_pos_max = c.htf; cf.sr_headroom_ratio = c.head; }
                rows[i] = { c, d.sym, run_replay(d.s, o) };
            }
        });
    }
    for (auto& th : pool) th.join();

    // 按组合聚合（跨品种汇总）
    struct Agg { double pnl=0, dd=0; int cycles=0, wins=0, pass=0, blk_h=0, blk_s=0; };
    std::map<std::string, Agg> agg;
    std::map<std::string, Combo> agg_c;
    for (const auto& row : rows) {
        char key[64];
        if (row.c.gates) std::snprintf(key, sizeof(key), "%.2f/%.1f", row.c.htf, row.c.head);
        else             std::snprintf(key, sizeof(key), "基线(不拦截)");
        auto& a = agg[key];
        agg_c[key] = row.c;
        a.pnl += row.r.total_pnl; a.dd += row.r.max_drawdown;
        a.cycles += row.r.cycles; a.wins += row.r.wins;
        a.pass += row.r.gate_pass; a.blk_h += row.r.gate_block_htf; a.blk_s += row.r.gate_block_sr;
    }

    std::vector<std::pair<std::string, Agg>> sorted(agg.begin(), agg.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
        double ra = a.second.dd > 1e-9 ? a.second.pnl / a.second.dd : a.second.pnl;
        double rb = b.second.dd > 1e-9 ? b.second.pnl / b.second.dd : b.second.pnl;
        return ra > rb;
    });

    std::cout << "════════ 汇总（" << data.size() << " 品种合计，按 收益/回撤 排序）════════\n";
    std::cout << std::left << std::setw(16) << "%B阈值/净空比"
              << std::right << std::setw(11) << "净盈亏U"
              << std::setw(11) << "总回撤U" << std::setw(9) << "收益/撤"
              << std::setw(8) << "周期" << std::setw(8) << "胜率%"
              << std::setw(9) << "放行" << std::setw(9) << "宏观拦" << std::setw(9) << "结构拦" << "\n";
    for (const auto& [k, a] : sorted) {
        double ratio = a.dd > 1e-9 ? a.pnl / a.dd : 0;
        std::cout << std::left << std::setw(16) << k << std::right << std::fixed
                  << std::setprecision(1) << std::setw(11) << a.pnl
                  << std::setw(11) << a.dd
                  << std::setprecision(3) << std::setw(9) << ratio
                  << std::setprecision(0) << std::setw(8) << a.cycles
                  << std::setprecision(1) << std::setw(8) << (a.cycles ? 100.0*a.wins/a.cycles : 0)
                  << std::setprecision(0) << std::setw(9) << a.pass
                  << std::setw(9) << a.blk_h << std::setw(9) << a.blk_s << "\n";
    }

    // 分品种明细（最优组合 vs 基线）
    std::cout << "\n════════ 分品种明细 ════════\n";
    for (const auto& d : data) {
        std::cout << "\n" << d.s.symbol << ":\n";
        for (const auto& row : rows) {
            if (row.sym != d.sym) continue;
            char key[64];
            if (row.c.gates) std::snprintf(key, sizeof(key), "%.2f/%.1f", row.c.htf, row.c.head);
            else             std::snprintf(key, sizeof(key), "基线");
            std::cout << "  " << std::left << std::setw(14) << key << std::right << std::fixed
                      << std::setprecision(1) << std::setw(10) << row.r.total_pnl << "U"
                      << "  回撤" << std::setw(9) << row.r.max_drawdown
                      << "  周期" << std::setw(4) << row.r.cycles
                      << "  收益/撤 " << std::setprecision(3)
                      << (row.r.max_drawdown > 1e-9 ? row.r.total_pnl/row.r.max_drawdown : 0) << "\n";
        }
    }
    return 0;
}

// ── wf：Walk-Forward 分段验证 ────────────────────────────────────────────────
// 把数据切成 N 段，每个参数组合在每段独立跑——判定"最优参数"是真规律还是拟合噪音。
// 用法：ccbot_backtest wf <数据根目录> --symbols ... --head 1.5,2.0,3.0 --segments 4
static int cmd_wf(int argc, char** argv) {
    std::string root = argv[2];
    auto syms = parse_slist(arg_str(argc, argv, "--symbols", "BTC-USDT,ETH-USDT,SOL-USDT,BNB-USDT"));
    auto head_list = parse_list(arg_str(argc, argv, "--head", "1.5,2.0,2.5,3.0,4.0"));
    auto htf_list  = parse_list(arg_str(argc, argv, "--htf",  "0.60,0.80"));
    const int nseg  = (int)arg_num(argc, argv, "--segments", 4);
    const double budget = arg_num(argc, argv, "--budget", 1000);
    const int lev = (int)arg_num(argc, argv, "--lev", 3);
    const int layers = (int)arg_num(argc, argv, "--layers", 6);

    struct Loaded { std::string sym; Series s; };
    std::vector<Loaded> data;
    for (const auto& sym : syms) {
        std::vector<std::string> files;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().filename().string() == sym + ".csv")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
        if (files.empty()) continue;
        Series s; QualityReport rep; std::string err;
        if (!load_csv_multi(files, s, rep, err)) continue;
        std::cout << "加载 " << s.symbol << "  " << s.bars.size() << " 根\n";
        data.push_back({sym, std::move(s)});
    }
    if (data.empty()) { std::cerr << "没有可用数据\n"; return 1; }

    // 时间切段（按全局时间范围等分）
    int64_t gmin = data[0].s.bars.front().ts_ms, gmax = data[0].s.bars.back().ts_ms;
    for (const auto& d : data) {
        gmin = std::min(gmin, d.s.bars.front().ts_ms);
        gmax = std::max(gmax, d.s.bars.back().ts_ms);
    }
    const int64_t seg_len = (gmax - gmin) / nseg;
    std::vector<std::pair<int64_t,int64_t>> segs;
    for (int i = 0; i < nseg; ++i)
        segs.emplace_back(gmin + seg_len * i, gmin + seg_len * (i + 1));

    std::cout << "\n分 " << nseg << " 段:\n";
    for (int i = 0; i < nseg; ++i)
        std::cout << "  S" << (i+1) << ": " << ts_to_str(segs[i].first)
                  << " ~ " << ts_to_str(segs[i].second) << "\n";

    // 组合：基线 + (htf × head)
    struct Combo { bool gates; double htf; double head; std::string label; };
    std::vector<Combo> combos{{false, 0, 0, "基线"}};
    for (double h : htf_list) for (double hr : head_list) {
        char lb[32]; std::snprintf(lb, sizeof(lb), "%.2f/%.1f", h, hr);
        combos.push_back({true, h, hr, lb});
    }

    const size_t total = combos.size() * data.size() * segs.size();
    std::cout << "\n" << combos.size() << " 组合 × " << data.size() << " 品种 × "
              << segs.size() << " 段 = " << total << " 次回放...\n\n";

    std::vector<BacktestResult> results(total);
    std::atomic<size_t> next{0};
    const unsigned nth = std::max(2u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nth; ++t) {
        pool.emplace_back([&]{
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= total) return;
                size_t si = i % segs.size();
                size_t di = (i / segs.size()) % data.size();
                size_t ci = i / (segs.size() * data.size());
                ReplayOptions o;
                o.initial_equity = 10000;
                o.start_ms = segs[si].first;
                o.end_ms   = segs[si].second;
                auto& cf = o.cfg;
                cf.symbol = data[di].s.symbol;
                cf.direction = CcgConfig::Direction::Long;
                cf.budget_usdt = budget; cf.leverage = lev; cf.max_entries = layers;
                cf.strat_type = CcgConfig::StratType::Linear;
                cf.cooldown_secs = 300; cf.auto_restart = true;
                cf.entry_mode = CcgConfig::EntryMode::Indicator;
                cf.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;
                cf.rsi_threshold = 35; cf.rsi_oversold_th = 25;
                cf.kline_interval = "1h";
                cf.dynamic_band_mode = true;
                cf.use_trend_filter = true;
                cf.sr_radar = true;
                cf.smart_gates = combos[ci].gates;
                if (combos[ci].gates) {
                    cf.htf_pos_max = combos[ci].htf;
                    cf.sr_headroom_ratio = combos[ci].head;
                }
                results[i] = run_replay(data[di].s, o);
            }
        });
    }
    for (auto& th : pool) th.join();

    // 汇总：每个组合 × 每段（跨品种合计）
    auto at = [&](size_t ci, size_t di, size_t si) -> const BacktestResult& {
        return results[ci * segs.size() * data.size() + di * segs.size() + si];
    };
    std::cout << "════════ Walk-Forward：各组合在每段的 收益/回撤（跨品种合计）════════\n";
    std::cout << std::left << std::setw(12) << "组合";
    for (size_t s = 0; s < segs.size(); ++s) std::cout << std::right << std::setw(11) << ("S" + std::to_string(s+1));
    std::cout << std::setw(11) << "为正段数" << std::setw(12) << "总盈亏U" << "\n";

    struct Score { std::string label; int pos_segs; double total_pnl; std::vector<double> ratios; };
    std::vector<Score> scores;
    for (size_t ci = 0; ci < combos.size(); ++ci) {
        Score sc; sc.label = combos[ci].label; sc.pos_segs = 0; sc.total_pnl = 0;
        std::cout << std::left << std::setw(12) << combos[ci].label << std::fixed;
        for (size_t si = 0; si < segs.size(); ++si) {
            double pnl = 0, dd = 0;
            for (size_t di = 0; di < data.size(); ++di) {
                pnl += at(ci, di, si).total_pnl;
                dd  += at(ci, di, si).max_drawdown;
            }
            double ratio = dd > 1e-9 ? pnl / dd : 0;
            sc.ratios.push_back(ratio);
            sc.total_pnl += pnl;
            if (pnl > 0) ++sc.pos_segs;
            std::cout << std::right << std::setw(11) << std::setprecision(3) << ratio;
        }
        std::cout << std::setw(11) << sc.pos_segs << "/" << segs.size()
                  << std::setw(11) << std::setprecision(1) << sc.total_pnl << "\n";
        scores.push_back(sc);
    }

    // 稳健性排序：先看"为正的段数"（跨段一致性），再看总盈亏
    std::sort(scores.begin(), scores.end(), [](const Score& a, const Score& b) {
        if (a.pos_segs != b.pos_segs) return a.pos_segs > b.pos_segs;
        return a.total_pnl > b.total_pnl;
    });
    std::cout << "\n════════ 稳健性排名（跨段一致性优先）════════\n";
    for (size_t i = 0; i < scores.size() && i < 8; ++i)
        std::cout << "  " << (i+1) << ". " << std::left << std::setw(12) << scores[i].label
                  << "  正收益段 " << scores[i].pos_segs << "/" << segs.size()
                  << "   总盈亏 " << std::fixed << std::setprecision(1) << scores[i].total_pnl << "U\n";
    return 0;
}

// ── rsi：RSI 参数寻优（三组正交实验 + walk-forward + 样本量门槛）─────────────
// 实验A: 确认带位置与宽度（探底/回穿阈值组合）
// 实验B: 反转确认(cross) vs 瞬时快照(snapshot)
// 实验C: RSI 周期敏感度
// 用法：ccbot_backtest rsi <数据根目录> --symbols ... --segments 4 [--min-cycles 15]
static int cmd_rsi(int argc, char** argv) {
    std::string root = argv[2];
    auto syms = parse_slist(arg_str(argc, argv, "--symbols", "BTC-USDT,ETH-USDT,SOL-USDT,BNB-USDT"));
    const int nseg = (int)arg_num(argc, argv, "--segments", 4);
    const int min_cycles = (int)arg_num(argc, argv, "--min-cycles", 15);
    const double budget = arg_num(argc, argv, "--budget", 1000);

    struct Loaded { std::string sym; Series s; };
    std::vector<Loaded> data;
    for (const auto& sym : syms) {
        std::vector<std::string> files;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().filename().string() == sym + ".csv")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
        if (files.empty()) continue;
        Series s; QualityReport rep; std::string err;
        if (!load_csv_multi(files, s, rep, err)) continue;
        std::cout << "加载 " << s.symbol << "  " << s.bars.size() << " 根\n";
        data.push_back({sym, std::move(s)});
    }
    if (data.empty()) { std::cerr << "没有可用数据\n"; return 1; }

    int64_t gmin = data[0].s.bars.front().ts_ms, gmax = data[0].s.bars.back().ts_ms;
    for (const auto& d : data) {
        gmin = std::min(gmin, d.s.bars.front().ts_ms);
        gmax = std::max(gmax, d.s.bars.back().ts_ms);
    }
    const int64_t seg_len = (gmax - gmin) / nseg;
    std::vector<std::pair<int64_t,int64_t>> segs;
    for (int i = 0; i < nseg; ++i) segs.emplace_back(gmin + seg_len*i, gmin + seg_len*(i+1));

    // 组合定义
    struct RC { std::string label; double os; double th; bool cross; int period; };
    std::vector<RC> combos;
    const std::string mode = arg_str(argc, argv, "--mode", "abc");

    if (mode == "grid") {
        // 二维全网格：探底阈值 × 回穿阈值（要求回穿>探底），供指定或用默认集
        auto os_list = parse_list(arg_str(argc, argv, "--os", "15,18,20,22,25,28,30,32"));
        auto th_list = parse_list(arg_str(argc, argv, "--th", "25,28,30,32,35,38,40,45"));
        for (double o : os_list) for (double t : th_list) {
            if (t <= o) continue;                       // 回穿必须高于探底
            if (t - o > 25) continue;                   // 带宽过大无意义
            char lb[40]; std::snprintf(lb, sizeof(lb), "%.0f/%.0f(+%.0f)", o, t, t - o);
            combos.push_back({lb, o, t, true, 14});
        }
    } else {
        // 实验A：确认带（全部 cross 模式，周期14）
        const double bands[][2] = {{20,30},{20,35},{20,40},{25,30},{25,35},{25,40},{30,35},{30,40}};
        for (auto& b : bands) {
            char lb[40]; std::snprintf(lb, sizeof(lb), "A %.0f/%.0f", b[0], b[1]);
            combos.push_back({lb, b[0], b[1], true, 14});
        }
        // 实验B：瞬时快照对照（同样阈值，无记忆）
        for (auto& b : {std::pair<double,double>{25,35}, {20,35}, {25,40}}) {
            char lb[40]; std::snprintf(lb, sizeof(lb), "B 快照%.0f", b.second);
            combos.push_back({lb, b.first, b.second, false, 14});
        }
        // 实验C：RSI周期（固定25/35 cross）
        for (int p : {7, 14, 21}) {
            char lb[40]; std::snprintf(lb, sizeof(lb), "C 周期%d", p);
            combos.push_back({lb, 25, 35, true, p});
        }
    }

    const size_t total = combos.size() * data.size() * segs.size();
    std::cout << "\n" << combos.size() << " 组合 × " << data.size() << " 品种 × "
              << segs.size() << " 段 = " << total << " 次回放"
              << "（样本量门槛：总周期数 < " << min_cycles << " 视为不足）...\n\n";

    std::vector<BacktestResult> results(total);
    std::atomic<size_t> next{0};
    const unsigned nth = std::max(2u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nth; ++t) {
        pool.emplace_back([&]{
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= total) return;
                size_t si = i % segs.size();
                size_t di = (i / segs.size()) % data.size();
                size_t ci = i / (segs.size() * data.size());
                const auto& rc = combos[ci];
                ReplayOptions o;
                o.initial_equity = 10000;
                o.start_ms = segs[si].first; o.end_ms = segs[si].second;
                auto& cf = o.cfg;
                cf.symbol = data[di].s.symbol;
                cf.direction = CcgConfig::Direction::Long;
                cf.budget_usdt = budget; cf.leverage = 3; cf.max_entries = 6;
                cf.strat_type = CcgConfig::StratType::Linear;
                cf.cooldown_secs = 300; cf.auto_restart = true;
                cf.entry_mode = CcgConfig::EntryMode::Indicator;
                cf.kline_interval = "1h";
                // 被扫描的 RSI 参数
                cf.rsi_confirm_mode = rc.cross ? CcgConfig::RsiConfirmMode::CrossFromOversold
                                               : CcgConfig::RsiConfirmMode::Snapshot;
                cf.rsi_oversold_th = rc.os;
                cf.rsi_threshold   = rc.th;
                cf.rsi_period      = rc.period;
                // 其余按 v3.1 实证默认
                cf.dynamic_band_mode = true;
                cf.use_trend_filter = true;
                cf.sr_radar = true;
                cf.smart_gates = true;
                cf.htf_pos_max = 0.60;
                cf.sr_headroom_ratio = 3.0;
                results[i] = run_replay(data[di].s, o);
            }
        });
    }
    for (auto& th : pool) th.join();

    auto at = [&](size_t ci, size_t di, size_t si) -> const BacktestResult& {
        return results[ci * segs.size() * data.size() + di * segs.size() + si];
    };

    std::cout << "════════ RSI 参数寻优（收益/回撤，跨品种合计）════════\n";
    std::cout << std::left << std::setw(12) << "组合";
    for (size_t s = 0; s < segs.size(); ++s) std::cout << std::right << std::setw(10) << ("S"+std::to_string(s+1));
    std::cout << std::setw(8) << "正段" << std::setw(10) << "总盈亏" << std::setw(8) << "周期"
              << std::setw(8) << "胜率%" << "  备注\n";

    struct Score { std::string label; int pos; double pnl; int cycles; double wins; bool enough; };
    std::vector<Score> scores;
    for (size_t ci = 0; ci < combos.size(); ++ci) {
        Score sc{combos[ci].label, 0, 0, 0, 0, false};
        std::cout << std::left << std::setw(12) << combos[ci].label << std::fixed;
        for (size_t si = 0; si < segs.size(); ++si) {
            double pnl = 0, dd = 0;
            for (size_t di = 0; di < data.size(); ++di) {
                const auto& r = at(ci, di, si);
                pnl += r.total_pnl; dd += r.max_drawdown;
                sc.cycles += r.cycles; sc.wins += r.wins;
            }
            sc.pnl += pnl;
            if (pnl > 0) ++sc.pos;
            std::cout << std::right << std::setw(10) << std::setprecision(3)
                      << (dd > 1e-9 ? pnl/dd : 0);
        }
        sc.enough = sc.cycles >= min_cycles;
        std::cout << std::setw(8) << (std::to_string(sc.pos) + "/" + std::to_string(segs.size()))
                  << std::setw(10) << std::setprecision(1) << sc.pnl
                  << std::setw(8) << sc.cycles
                  << std::setw(8) << (sc.cycles ? 100.0*sc.wins/sc.cycles : 0)
                  << (sc.enough ? "" : "  ⚠样本不足") << "\n";
        scores.push_back(sc);
    }

    std::vector<Score> valid;
    for (auto& s : scores) if (s.enough) valid.push_back(s);
    std::sort(valid.begin(), valid.end(), [](const Score& a, const Score& b) {
        if (a.pos != b.pos) return a.pos > b.pos;
        return a.pnl > b.pnl;
    });
    std::cout << "\n════════ 稳健性排名（样本充足者，跨段一致性优先）════════\n";
    for (size_t i = 0; i < valid.size() && i < 15; ++i)
        std::cout << "  " << std::right << std::setw(2) << (i+1) << ". " << std::left << std::setw(14) << valid[i].label
                  << "  正收益段 " << valid[i].pos << "/" << segs.size()
                  << "   总盈亏 " << std::fixed << std::setprecision(1) << std::setw(8) << valid[i].pnl << "U"
                  << "   周期 " << valid[i].cycles << "\n";

    if (mode == "grid") {
        // ── 规律分析：按【确认带宽度】和【探底深度】两个维度分别聚合 ──────────
        struct Bucket { double pnl = 0; int n = 0, all_pos = 0, cycles = 0; };
        std::map<int, Bucket> by_width, by_depth;
        for (size_t ci = 0; ci < combos.size(); ++ci) {
            if (!scores[ci].enough) continue;
            int w = (int)std::lround(combos[ci].th - combos[ci].os);
            int d = (int)std::lround(combos[ci].os);
            for (auto* m : { &by_width, &by_depth }) {
                int key = (m == &by_width) ? w : d;
                auto& b = (*m)[key];
                b.pnl += scores[ci].pnl; ++b.n; b.cycles += scores[ci].cycles;
                if (scores[ci].pos == (int)segs.size()) ++b.all_pos;
            }
        }
        std::cout << "\n════════ 规律①：按确认带宽度（回穿−探底）聚合 ════════\n";
        std::cout << std::left << std::setw(10) << "带宽" << std::right << std::setw(10) << "组合数"
                  << std::setw(12) << "平均盈亏" << std::setw(12) << "4/4段占比" << std::setw(12) << "平均周期\n";
        for (const auto& [w, b] : by_width)
            std::cout << std::left << std::setw(10) << ("+" + std::to_string(w)) << std::right
                      << std::setw(10) << b.n << std::fixed << std::setprecision(1)
                      << std::setw(12) << (b.pnl / b.n)
                      << std::setw(11) << (100.0 * b.all_pos / b.n) << "%"
                      << std::setw(12) << (b.cycles / b.n) << "\n";

        std::cout << "\n════════ 规律②：按探底深度聚合 ════════\n";
        std::cout << std::left << std::setw(10) << "探底" << std::right << std::setw(10) << "组合数"
                  << std::setw(12) << "平均盈亏" << std::setw(12) << "4/4段占比" << std::setw(12) << "平均周期\n";
        for (const auto& [d, b] : by_depth)
            std::cout << std::left << std::setw(10) << ("RSI≤" + std::to_string(d)) << std::right
                      << std::setw(10) << b.n << std::fixed << std::setprecision(1)
                      << std::setw(12) << (b.pnl / b.n)
                      << std::setw(11) << (100.0 * b.all_pos / b.n) << "%"
                      << std::setw(12) << (b.cycles / b.n) << "\n";

        // 高原检验：最优组合的相邻格子是否也在前列
        if (!valid.empty()) {
            std::cout << "\n════════ 高原检验：最优组合 " << valid[0].label << " 的邻居 ════════\n";
            size_t best_ci = 0;
            for (size_t ci = 0; ci < combos.size(); ++ci)
                if (combos[ci].label == valid[0].label) { best_ci = ci; break; }
            double bo = combos[best_ci].os, bt = combos[best_ci].th;
            for (size_t ci = 0; ci < combos.size(); ++ci) {
                double dd = std::fabs(combos[ci].os - bo) + std::fabs(combos[ci].th - bt);
                if (dd > 0 && dd <= 6 && scores[ci].enough) {
                    int rank = 0;
                    for (size_t k = 0; k < valid.size(); ++k)
                        if (valid[k].label == combos[ci].label) { rank = (int)k + 1; break; }
                    std::cout << "  邻居 " << std::left << std::setw(14) << combos[ci].label
                              << " 排名 " << std::setw(4) << rank
                              << " 盈亏 " << std::fixed << std::setprecision(1) << scores[ci].pnl
                              << "U  正段 " << scores[ci].pos << "/" << segs.size() << "\n";
                }
            }
        }
    }
    return 0;
}

// ── portfolio：组合回测（多品种共享一个账户，与实盘同构）────────────────────
static int cmd_portfolio(int argc, char** argv) {
    std::string root = argv[2];
    const double equity = arg_num(argc, argv, "--equity", 10000);
    const double per_sym = arg_num(argc, argv, "--per-symbol", 2000);
    const int max_pos = (int)arg_num(argc, argv, "--max-positions", 50);
    const int max_syms = (int)arg_num(argc, argv, "--max-symbols", 60);
    const double cap = arg_num(argc, argv, "--margin-cap", 0);
    std::string sym_arg = arg_str(argc, argv, "--symbols", "");

    // 品种选择：显式指定，或按数据完整性+成交额自动挑选流动性最好的
    std::vector<std::string> files;
    if (!sym_arg.empty()) {
        for (const auto& sy : parse_slist(sym_arg))
            for (const auto& e : fs::recursive_directory_iterator(root))
                if (e.is_regular_file() && e.path().filename().string() == sy + ".csv")
                    files.push_back(e.path().string());
    } else {
        std::map<std::string, std::vector<std::string>> by_sym;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || e.path().extension() != ".csv") continue;
            by_sym[e.path().filename().string()].push_back(e.path().string());
        }
        // 只要 USDT 永续、且各年份文件齐全的（跨年连续）
        std::vector<std::pair<std::string, std::vector<std::string>>> cand;
        size_t max_files = 0;
        for (auto& [n, v] : by_sym) max_files = std::max(max_files, v.size());
        for (auto& [n, v] : by_sym) {
            if (n.find("-USDT.csv") == std::string::npos) continue;
            if (v.size() < max_files) continue;              // 缺年份的跳过
            cand.emplace_back(n, v);
        }
        std::sort(cand.begin(), cand.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });
        std::cout << "自动筛选：" << cand.size() << " 个跨年完整的 USDT 永续，取前 "
                  << max_syms << " 个（按文件大小=活跃度排序）\n";
        // 按总文件大小排序（成交活跃的品种文件更大）
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b){
            uintmax_t sa = 0, sb = 0;
            for (auto& p : a.second) sa += fs::file_size(p);
            for (auto& p : b.second) sb += fs::file_size(p);
            return sa > sb;
        });
        for (size_t i = 0; i < cand.size() && (int)i < max_syms; ++i)
            for (auto& p : cand[i].second) files.push_back(p);
    }
    if (files.empty()) { std::cerr << "没有找到数据\n"; return 1; }

    // 按品种分组
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& f : files) grouped[normalize_symbol(f)].push_back(f);
    // 品种多时用流式（全量载入 596 品种需 36GB 内存）
    const bool use_stream = grouped.size() > 80 || arg_flag(argc, argv, "--stream");

    PortfolioOptions o;
    o.initial_equity = equity;
    o.per_symbol_budget = per_sym;
    o.max_positions = max_pos;
    o.max_total_margin = cap;
    auto& c = o.base_cfg;
    c.direction = CcgConfig::Direction::Long;
    c.leverage = (int)arg_num(argc, argv, "--lev", 3);
    c.max_entries = (int)arg_num(argc, argv, "--layers", 6);
    c.strat_type = CcgConfig::StratType::Linear;
    c.cooldown_secs = 300; c.auto_restart = true;
    c.entry_mode = CcgConfig::EntryMode::Indicator;
    c.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;
    c.rsi_oversold_th = arg_num(argc, argv, "--rsi-os", 25);
    c.rsi_threshold   = arg_num(argc, argv, "--rsi-th", 30);
    c.rsi_period      = (int)arg_num(argc, argv, "--rsi-period", 14);
    c.kline_interval  = arg_str(argc, argv, "--tf", "1h");
    c.boll_period = 20; c.boll_mult = 2.0;
    c.dynamic_band_mode = true;
    c.min_profit_floor  = arg_num(argc, argv, "--floor", 2.0);
    c.use_trend_filter  = true;
    c.sr_radar = true;
    c.smart_gates = true;
    c.htf_pos_max = arg_num(argc, argv, "--htf-max", 0.60);
    c.sr_headroom_ratio = arg_num(argc, argv, "--headroom", 3.0);
    c.use_sr_exit = true;
    c.use_structural_stop = arg_flag(argc, argv, "--struct-stop");

    std::cout << "配置: 本金" << equity << "U  每品种" << per_sym << "U  杠杆" << c.leverage
              << "  层数" << c.max_entries << "  RSI " << c.rsi_oversold_th << "/" << c.rsi_threshold
              << "  保底利润" << c.min_profit_floor << "%  %B" << c.htf_pos_max
              << "  净空" << c.sr_headroom_ratio << "\n"
              << "开关: 动态W + 趋势过滤 + SR雷达 + 三层拦截 + 止盈锚定阻力"
              << (c.use_structural_stop ? " + 结构止损" : "") << "\n\n";

    auto t0 = std::chrono::steady_clock::now();
    PortfolioResult r;
    if (use_stream) {
        std::vector<std::pair<std::string, std::vector<std::string>>> fl(grouped.begin(), grouped.end());
        std::cout << "流式模式：" << fl.size() << " 个品种（内存与品种数无关）\n\n";
        r = run_portfolio_stream(fl, o);
    } else {
        std::vector<Series> all;
        std::cout << "加载 " << grouped.size() << " 个品种...\n";
        for (auto& [sym, f2] : grouped) {
            std::sort(f2.begin(), f2.end());
            Series s; QualityReport rep; std::string err;
            if (!load_csv_multi(f2, s, rep, err)) { std::cout << "  [跳过] " << sym << "\n"; continue; }
            all.push_back(std::move(s));
        }
        if (all.empty()) { std::cerr << "加载失败\n"; return 1; }
        std::cout << "已加载 " << all.size() << " 个品种，"
                  << ts_to_str(all[0].bars.front().ts_ms) << " ~ "
                  << ts_to_str(all[0].bars.back().ts_ms) << "\n\n";
        r = run_portfolio(all, o);
    }
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();

    std::cout << "════════ 组合回测结果 ════════\n" << r.to_text()
              << "  回放耗时  : " << secs << " 秒\n";

    std::cout << "\n════════ 分品种盈亏（前15 / 后10）════════\n";
    for (size_t i = 0; i < r.per_symbol.size() && i < 15; ++i) {
        const auto& s = r.per_symbol[i];
        std::cout << "  " << std::left << std::setw(14) << s.symbol << std::right
                  << std::fixed << std::setprecision(1) << std::setw(9) << s.pnl << "U"
                  << "  周期" << std::setw(3) << s.cycles
                  << "  胜率" << std::setw(5) << std::setprecision(0)
                  << (s.cycles ? 100.0*s.wins/s.cycles : 0) << "%"
                  << "  最大层" << s.max_layers << "\n";
    }
    if (r.per_symbol.size() > 15) {
        std::cout << "  ...\n";
        for (size_t i = std::max((size_t)15, r.per_symbol.size() - 10); i < r.per_symbol.size(); ++i) {
            const auto& s = r.per_symbol[i];
            std::cout << "  " << std::left << std::setw(14) << s.symbol << std::right
                      << std::fixed << std::setprecision(1) << std::setw(9) << s.pnl << "U"
                      << "  周期" << std::setw(3) << s.cycles
                      << "  胜率" << std::setw(5) << std::setprecision(0)
                      << (s.cycles ? 100.0*s.wins/s.cycles : 0) << "%"
                      << "  最大层" << s.max_layers << "\n";
        }
    }
    // 权益曲线（按月采样输出）
    if (!r.equity_curve.empty()) {
        std::cout << "\n════════ 权益曲线（月末）════════\n";
        int last_m = -1;
        for (const auto& [ts, eq] : r.equity_curve) {
            std::string d = ts_to_str(ts);
            int m = std::stoi(d.substr(5,2));
            if (m != last_m) {
                last_m = m;
                std::cout << "  " << d.substr(0,7) << "  " << std::fixed
                          << std::setprecision(0) << eq << " U\n";
            }
        }
    }
    return 0;
}

// ── pgrid：组合回测的多配置并行网格（把多核用满）─────────────────────────────
// 数据只载入一次（共享只读），N 个配置各占一个线程独立跑账户模拟。
// 用法：ccbot_backtest pgrid <数据根目录> --max-symbols 100
//        --positions 5,8,12,15,20,30,50 --budgets 500,1000,2000 [--layers 4,6,8]
static int cmd_pgrid(int argc, char** argv) {
    std::string root = argv[2];
    const int max_syms = (int)arg_num(argc, argv, "--max-symbols", 100);
    const double equity = arg_num(argc, argv, "--equity", 10000);
    auto pos_list = parse_list(arg_str(argc, argv, "--positions", "5,8,12,15,20,30,50"));
    auto bud_list = parse_list(arg_str(argc, argv, "--budgets", "500,1000,2000"));
    auto lay_list = parse_list(arg_str(argc, argv, "--layers", "6"));
    const unsigned max_threads = (unsigned)arg_num(argc, argv, "--threads", 60);
    std::string sym_arg = arg_str(argc, argv, "--symbols", "");

    // ── 品种选择 ────────────────────────────────────────────────────────────
    std::map<std::string, std::vector<std::string>> by_file;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".csv") continue;
        by_file[e.path().filename().string()].push_back(e.path().string());
    }
    size_t max_files = 0;
    for (auto& [n, v] : by_file) max_files = std::max(max_files, v.size());

    std::vector<std::pair<std::string, std::vector<std::string>>> chosen;
    if (!sym_arg.empty()) {
        for (const auto& sy : parse_slist(sym_arg)) {
            auto it = by_file.find(sy + ".csv");
            if (it != by_file.end()) chosen.push_back(*it);
        }
    } else {
        for (auto& [n, v] : by_file) {
            if (n.find("-USDT.csv") == std::string::npos) continue;
            if (v.size() < max_files) continue;      // 跨年不完整的跳过
            chosen.push_back({n, v});
        }
        std::sort(chosen.begin(), chosen.end(), [](auto& a, auto& b) {
            uintmax_t sa = 0, sb = 0;
            for (auto& p : a.second) sa += fs::file_size(p);
            for (auto& p : b.second) sb += fs::file_size(p);
            return sa > sb;                          // 文件大=活跃度高
        });
        if ((int)chosen.size() > max_syms) chosen.resize(max_syms);
    }
    if (chosen.empty()) { std::cerr << "没有找到数据\n"; return 1; }

    // ── 数据一次性载入到内存，全部配置共享只读 ──────────────────────────────
    std::cout << "载入 " << chosen.size() << " 个品种到内存（多配置共享）...\n";
    auto tl0 = std::chrono::steady_clock::now();
    std::vector<Series> all(chosen.size());
    {
        std::atomic<size_t> idx{0};
        std::atomic<size_t> done{0};
        unsigned lth = std::max(4u, std::min(std::thread::hardware_concurrency(), 24u));
        std::vector<std::thread> lp;
        for (unsigned t = 0; t < lth; ++t) lp.emplace_back([&]{
            for (;;) {
                size_t i = idx.fetch_add(1);
                if (i >= chosen.size()) return;
                auto paths = chosen[i].second;
                std::sort(paths.begin(), paths.end());
                Series s; QualityReport rep; std::string err;
                if (load_csv_multi(paths, s, rep, err)) all[i] = std::move(s);
                size_t d = ++done;
                if (d % 25 == 0) std::cout << "  已载入 " << d << "/" << chosen.size() << "\n";
            }
        });
        for (auto& t : lp) t.join();
    }
    all.erase(std::remove_if(all.begin(), all.end(),
              [](const Series& s){ return s.bars.empty(); }), all.end());
    auto lsec = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - tl0).count();
    size_t total_bars = 0;
    for (const auto& s : all) total_bars += s.bars.size();
    std::cout << "载入完成: " << all.size() << " 品种 / " << (total_bars/1000000) << "M 根 / "
              << (total_bars * sizeof(Bar) / 1024 / 1024) << " MB / " << lsec << " 秒\n";

    // ── 配置网格 ────────────────────────────────────────────────────────────
    struct Cfg { int positions; double budget; int layers; };
    std::vector<Cfg> grid;
    for (double p : pos_list) for (double b : bud_list) for (double l : lay_list)
        grid.push_back({(int)p, b, (int)l});

    const unsigned nth = std::max(1u, std::min({max_threads,
                          std::thread::hardware_concurrency(), (unsigned)grid.size()}));
    std::cout << "\n配置组合 " << grid.size() << " 组，并行度 " << nth
              << " 线程（CPU " << std::thread::hardware_concurrency() << " 逻辑核）\n\n";

    std::vector<PortfolioResult> results(grid.size());
    std::atomic<size_t> next{0};
    std::atomic<size_t> finished{0};
    std::mutex log_mtx;
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nth; ++t) {
        pool.emplace_back([&]{
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= grid.size()) return;
                PortfolioOptions o;
                o.initial_equity = equity;
                o.per_symbol_budget = grid[i].budget;
                o.max_positions = grid[i].positions;
                // 关键：账户总保证金上限 = 并发数 × 每品种保证金（这才是"限制并发"的
                // 真实机制——引擎用总保证金上限挡新首仓，没有单独的"最多N个仓"开关）
                o.max_total_margin = std::min(equity * 0.9,
                    grid[i].positions * grid[i].budget / 3.0);
                auto& c = o.base_cfg;
                c.direction = CcgConfig::Direction::Long;
                c.leverage = 3; c.max_entries = grid[i].layers;
                c.strat_type = CcgConfig::StratType::Linear;
                c.cooldown_secs = 300; c.auto_restart = true;
                c.entry_mode = CcgConfig::EntryMode::Indicator;
                c.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;
                c.rsi_oversold_th = 25; c.rsi_threshold = 30; c.rsi_period = 14;
                c.kline_interval = "1h"; c.boll_period = 20; c.boll_mult = 2.0;
                c.dynamic_band_mode = true;
                c.min_profit_floor = arg_num(argc, argv, "--floor", 2.0);
                c.use_trend_filter = true;
                c.sr_radar = true; c.smart_gates = true;
                c.htf_pos_max = 0.60; c.sr_headroom_ratio = 3.0;
                c.use_sr_exit = true;
                results[i] = run_portfolio(all, o);
                size_t f = ++finished;
                std::lock_guard<std::mutex> lk(log_mtx);
                std::cout << "  [" << f << "/" << grid.size() << "] 并发"
                          << grid[i].positions << " 预算" << (int)grid[i].budget
                          << " 层" << grid[i].layers << " → "
                          << std::fixed << std::setprecision(1) << results[i].return_pct << "%"
                          << "  回撤" << results[i].max_dd_pct << "%"
                          << "  实际峰值持仓" << results[i].peak_positions << "\n";
            }
        });
    }
    for (auto& t : pool) t.join();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();

    // ── 结果表（按 收益/回撤 排序）───────────────────────────────────────────
    std::vector<size_t> order(grid.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double ra = results[a].max_drawdown > 1e-9 ? results[a].total_pnl/results[a].max_drawdown : -99;
        double rb = results[b].max_drawdown > 1e-9 ? results[b].total_pnl/results[b].max_drawdown : -99;
        return ra > rb;
    });

    std::cout << "\n════════ 组合参数网格结果（" << all.size() << " 品种池，按 收益/回撤 排序）════════\n";
    std::cout << std::left << std::setw(8) << "并发" << std::setw(9) << "预算"
              << std::setw(7) << "层数" << std::right
              << std::setw(11) << "收益%" << std::setw(10) << "回撤%"
              << std::setw(10) << "收益/撤" << std::setw(9) << "周期"
              << std::setw(8) << "胜率%" << std::setw(10) << "峰值仓"
              << std::setw(10) << "峰值保证" << std::setw(9) << "拦截" << "\n";
    for (size_t k : order) {
        const auto& r = results[k];
        std::cout << std::left << std::setw(8) << grid[k].positions
                  << std::setw(9) << (int)grid[k].budget << std::setw(7) << grid[k].layers
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(11) << r.return_pct << std::setw(10) << r.max_dd_pct
                  << std::setprecision(2) << std::setw(10)
                  << (r.max_drawdown > 1e-9 ? r.total_pnl/r.max_drawdown : 0)
                  << std::setprecision(0) << std::setw(9) << r.total_cycles
                  << std::setprecision(1) << std::setw(8)
                  << (r.total_cycles ? 100.0*r.total_wins/r.total_cycles : 0)
                  << std::setw(10) << r.peak_positions
                  << std::setw(10) << std::setprecision(0) << r.peak_margin
                  << std::setw(9) << r.margin_blocks << "\n";
    }
    std::cout << "\n总耗时 " << secs << " 秒（" << grid.size() << " 组并行）\n";

    // 最优组合的分品种明细
    if (!order.empty()) {
        const auto& best = results[order[0]];
        std::cout << "\n════════ 最优组合（并发" << grid[order[0]].positions
                  << " 预算" << (int)grid[order[0]].budget << "）分品种前20 ════════\n";
        for (size_t i = 0; i < best.per_symbol.size() && i < 20; ++i) {
            const auto& s = best.per_symbol[i];
            std::cout << "  " << std::left << std::setw(16) << s.symbol << std::right
                      << std::fixed << std::setprecision(1) << std::setw(9) << s.pnl << "U"
                      << "  周期" << std::setw(3) << s.cycles
                      << "  最大层" << s.max_layers << "\n";
        }
    }
    return 0;
}

// ── floor：保底利润 × 层数 × 止盈锚 三维扫描（组合账户 + walk-forward）───────
// 已被验证的参数做基底（RSI 25/30 cross、周期14、净空3.0、%B 0.60），
// 只扫描三个尚无证据的维度。
static int cmd_floor(int argc, char** argv) {
    std::string root = argv[2];
    auto syms = parse_slist(arg_str(argc, argv, "--symbols", "BTC-USDT,ETH-USDT,SOL-USDT,BNB-USDT"));
    auto floor_list = parse_list(arg_str(argc, argv, "--floors", "0.3,0.5,1.0,1.5,2.0,2.5,3.0,4.0"));
    auto layer_list = parse_list(arg_str(argc, argv, "--layers", "4,6,8"));
    const int nseg = (int)arg_num(argc, argv, "--segments", 4);
    const double equity = arg_num(argc, argv, "--equity", 10000);
    const double per_sym = arg_num(argc, argv, "--per-symbol", 2000);
    const unsigned max_threads = (unsigned)arg_num(argc, argv, "--threads", 72);

    // 载入数据（共享只读）
    std::vector<Series> all;
    for (const auto& sym : syms) {
        std::vector<std::string> files;
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().filename().string() == sym + ".csv")
                files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
        if (files.empty()) continue;
        Series s; QualityReport rep; std::string err;
        if (!load_csv_multi(files, s, rep, err)) continue;
        std::cout << "加载 " << s.symbol << "  " << s.bars.size() << " 根\n";
        all.push_back(std::move(s));
    }
    if (all.empty()) { std::cerr << "没有可用数据\n"; return 1; }

    int64_t gmin = all[0].bars.front().ts_ms, gmax = all[0].bars.back().ts_ms;
    for (const auto& s : all) {
        gmin = std::min(gmin, s.bars.front().ts_ms);
        gmax = std::max(gmax, s.bars.back().ts_ms);
    }
    const int64_t seg_len = (gmax - gmin) / nseg;
    std::vector<std::pair<int64_t,int64_t>> segs;
    for (int i = 0; i < nseg; ++i) segs.emplace_back(gmin + seg_len*i, gmin + seg_len*(i+1));

    struct Cfg { double floor; int layers; bool sr_exit; };
    std::vector<Cfg> grid;
    for (double f : floor_list) for (double l : layer_list) for (bool e : {true, false})
        grid.push_back({f, (int)l, e});

    const size_t total = grid.size() * segs.size();
    const unsigned nth = std::max(1u, std::min({max_threads,
                          std::thread::hardware_concurrency(), (unsigned)total}));
    std::cout << "\n" << grid.size() << " 组配置 × " << segs.size() << " 段 = "
              << total << " 次组合回放，并行度 " << nth << "\n"
              << "基底: RSI25/30反转 周期14 · 净空3.0 · %B0.60 · 动态W · 趋势 · SR雷达 · 三层拦截\n"
              << "扫描: 保底利润 × 层数 × 止盈锚定阻力\n\n";

    std::vector<PortfolioResult> results(total);
    std::atomic<size_t> next{0}, done{0};
    std::mutex lg;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nth; ++t) {
        pool.emplace_back([&]{
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= total) return;
                size_t ci = i / segs.size(), si = i % segs.size();
                PortfolioOptions o;
                o.initial_equity = equity;
                o.per_symbol_budget = per_sym;
                o.max_positions = (int)all.size();
                o.start_ms = segs[si].first; o.end_ms = segs[si].second;
                auto& c = o.base_cfg;
                c.direction = CcgConfig::Direction::Long;
                c.leverage = 3; c.max_entries = grid[ci].layers;
                c.strat_type = CcgConfig::StratType::Linear;
                c.cooldown_secs = 300; c.auto_restart = true;
                c.entry_mode = CcgConfig::EntryMode::Indicator;
                c.rsi_confirm_mode = CcgConfig::RsiConfirmMode::CrossFromOversold;
                c.rsi_oversold_th = 25; c.rsi_threshold = 30; c.rsi_period = 14;
                c.kline_interval = "1h"; c.boll_period = 20; c.boll_mult = 2.0;
                c.dynamic_band_mode = true;
                c.min_profit_floor = grid[ci].floor;
                c.use_trend_filter = true;
                c.sr_radar = true; c.smart_gates = true;
                c.htf_pos_max = 0.60; c.sr_headroom_ratio = 3.0;
                c.use_sr_exit = grid[ci].sr_exit;
                results[i] = run_portfolio(all, o);
                size_t d = ++done;
                if (d % 20 == 0) {
                    std::lock_guard<std::mutex> lk(lg);
                    std::cout << "  进度 " << d << "/" << total << "\n";
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();

    // 聚合每个配置的跨段表现
    struct Sc { std::string label; double floor; int layers; bool ex;
                int pos = 0; double pnl = 0, dd = 0; int cycles = 0, wins = 0; };
    std::vector<Sc> scores;
    for (size_t ci = 0; ci < grid.size(); ++ci) {
        Sc s; s.floor = grid[ci].floor; s.layers = grid[ci].layers; s.ex = grid[ci].sr_exit;
        char lb[48];
        std::snprintf(lb, sizeof(lb), "%.1f%%/%d层/%s", grid[ci].floor, grid[ci].layers,
                      grid[ci].sr_exit ? "锚定" : "上轨");
        s.label = lb;
        for (size_t si = 0; si < segs.size(); ++si) {
            const auto& r = results[ci * segs.size() + si];
            s.pnl += r.total_pnl; s.dd += r.max_drawdown;
            s.cycles += r.total_cycles; s.wins += r.total_wins;
            if (r.total_pnl > 0) ++s.pos;
        }
        scores.push_back(s);
    }
    std::sort(scores.begin(), scores.end(), [](const Sc& a, const Sc& b){
        if (a.pos != b.pos) return a.pos > b.pos;
        double ra = a.dd > 1e-9 ? a.pnl/a.dd : -99, rb = b.dd > 1e-9 ? b.pnl/b.dd : -99;
        return ra > rb;
    });

    std::cout << "\n════════ 全部组合（跨段一致性优先）════════\n";
    std::cout << std::left << std::setw(20) << "保底/层数/止盈锚" << std::right
              << std::setw(8) << "正段" << std::setw(11) << "总盈亏"
              << std::setw(11) << "总回撤" << std::setw(10) << "收益/撤"
              << std::setw(9) << "周期" << std::setw(8) << "胜率%" << "\n";
    for (const auto& s : scores)
        std::cout << std::left << std::setw(20) << s.label << std::right
                  << std::setw(6) << s.pos << "/" << segs.size()
                  << std::fixed << std::setprecision(1)
                  << std::setw(11) << s.pnl << std::setw(11) << s.dd
                  << std::setprecision(3) << std::setw(10) << (s.dd > 1e-9 ? s.pnl/s.dd : 0)
                  << std::setprecision(0) << std::setw(9) << s.cycles
                  << std::setprecision(1) << std::setw(8)
                  << (s.cycles ? 100.0*s.wins/s.cycles : 0) << "\n";

    // 单维度聚合：保底利润的规律（核心问题）
    std::map<double, std::pair<double,int>> by_floor;   // floor → {pnl合计, 组数}
    std::map<int, std::pair<double,int>>    by_layer;
    std::map<bool, std::pair<double,int>>   by_exit;
    std::map<double, int> floor_cycles;
    for (const auto& s : scores) {
        by_floor[s.floor].first += s.pnl; by_floor[s.floor].second++;
        by_layer[s.layers].first += s.pnl; by_layer[s.layers].second++;
        by_exit[s.ex].first += s.pnl;      by_exit[s.ex].second++;
        floor_cycles[s.floor] += s.cycles;
    }
    std::cout << "\n════════ 规律①：保底利润（核心未知数）════════\n";
    std::cout << std::left << std::setw(12) << "保底%" << std::right
              << std::setw(14) << "平均盈亏" << std::setw(12) << "总周期数\n";
    for (const auto& [f, v] : by_floor)
        std::cout << std::left << std::setw(12) << f << std::right << std::fixed
                  << std::setprecision(1) << std::setw(14) << (v.first/v.second)
                  << std::setw(12) << (floor_cycles[f]/v.second) << "\n";

    std::cout << "\n════════ 规律②：层数 ════════\n";
    for (const auto& [l, v] : by_layer)
        std::cout << "  " << l << " 层 : 平均盈亏 " << std::fixed << std::setprecision(1)
                  << (v.first/v.second) << " U\n";

    std::cout << "\n════════ 规律③：止盈锚定阻力 ════════\n";
    for (const auto& [e, v] : by_exit)
        std::cout << "  " << (e ? "锚定阻力" : "仅上轨  ") << " : 平均盈亏 "
                  << std::fixed << std::setprecision(1) << (v.first/v.second) << " U\n";

    std::cout << "\n总耗时 " << secs << " 秒\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    try {
        if (cmd == "scan")  return cmd_scan(argv[2]);
        if (cmd == "check") return cmd_check(argv[2]);
        if (cmd == "cache" && argc >= 4) return cmd_cache(argv[2], argv[3]);
        if (cmd == "run")   return cmd_run(argc, argv);
        if (cmd == "sweep") return cmd_sweep(argc, argv);
        if (cmd == "wf")    return cmd_wf(argc, argv);
        if (cmd == "rsi")   return cmd_rsi(argc, argv);
        if (cmd == "portfolio") return cmd_portfolio(argc, argv);
        if (cmd == "pgrid")     return cmd_pgrid(argc, argv);
        if (cmd == "floor")     return cmd_floor(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 1;
}
