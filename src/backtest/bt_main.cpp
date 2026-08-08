// ccbot_backtest：回测工具（P0 数据层阶段）
// 用法：
//   ccbot_backtest scan   <数据目录>            扫描全部CSV，输出数据集总览
//   ccbot_backtest check  <CSV文件>             单品种详细质检报告
//   ccbot_backtest cache  <CSV文件> <缓存路径>  解析并写二进制缓存（后续回放用）
#include "backtest/bt_data.h"
#include "backtest/bt_replay.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
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
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 1;
}
