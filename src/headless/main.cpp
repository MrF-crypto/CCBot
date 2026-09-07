// ccbot_headless：无图形界面版本，配置文件驱动，Windows/Linux 都能编译运行。
// 用法：ccbot_headless [配置文件路径，默认 config.json]
#include "version.h"
#include "core/ccg_engine.h"
#include "core/funding_ledger.h"
#include "core/sr_zones.h"
#include "core/decision.h"
#include "core/thread_pool.h"
#include "net/trading_client.h"
#include "net/book_ticker_stream.h"
#include "net/alert.h"
#include "headless/headless_config.h"
#include "headless/headless_state.h"

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <set>
#include <map>
#include <unordered_map>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <csignal>
#endif

using namespace ccbot;

namespace {

std::atomic<bool> g_running{true};
std::mutex        g_log_mtx;
std::string       g_log_path;

void handle_signal(int) { g_running.store(false); }

std::string now_str() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void log_line(const std::string& msg, const std::string& level = "INFO") {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::string line = "[" + now_str() + "] [" + level + "] " + msg;
    std::cout << line << std::endl;
    if (!g_log_path.empty()) {
        std::ofstream f(g_log_path, std::ios::app);
        if (f) f << line << "\n";
    }
}

// ── 单实例锁（PID 锁文件）────────────────────────────────────────────────────
// 同一个状态文件 = 同一个逻辑实例：双开会各自独立决策、对同一账户重复下单。
// 锁文件里写 PID；已存在时检查那个进程是否还活着，死进程残留的锁自动接管
bool pid_alive(long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

bool acquire_instance_lock(const std::string& lock_path) {
    if (std::filesystem::exists(lock_path)) {
        long old_pid = 0;
        { std::ifstream f(lock_path); f >> old_pid; }
        if (old_pid > 0 && pid_alive(old_pid)) return false;   // 真的有实例在跑
        std::error_code ec;
        std::filesystem::remove(lock_path, ec);                 // 死进程残留，接管
    }
    std::ofstream f(lock_path, std::ios::trunc);
    if (!f) return false;
#ifdef _WIN32
    f << _getpid();
#else
    f << ::getpid();
#endif
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = (argc > 1) ? argv[1] : "config.json";

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    HeadlessConfig cfg;
    std::string err;
    if (!load_headless_config(config_path, cfg, err)) {
        std::cerr << "配置加载失败: " << err << std::endl;
        return 1;
    }
    g_log_path = cfg.log_path;

    const std::string lock_path = cfg.state_path + ".lock";
    if (!acquire_instance_lock(lock_path)) {
        std::cerr << "已有另一个 ccbot_headless 实例在运行（锁文件 " << lock_path
                  << "）。同一账户双开会重复下单，拒绝启动。" << std::endl;
        return 1;
    }

    // 版本号写进日志：VPS 上排查问题时，第一件要确认的事就是"跑的到底是哪一版"。
    // 顺带让版本串真正进到二进制里——发布流水线的泄漏检查靠它核对
    // "包里的可执行文件是不是这个 tag 编出来的"
    log_line(std::string("ccbot headless ") + ccbot::kVersion + " 启动，配置文件: "
             + config_path + "，共 " + std::to_string(cfg.bots.size()) + " 个 bot");

    TradingClient::Config tc_cfg;
    tc_cfg.api_key    = cfg.api_key;
    tc_cfg.api_secret = cfg.api_secret;
    tc_cfg.testnet    = cfg.testnet;
    const bool is_pm  = (cfg.account_mode == "portfolio_margin");
    tc_cfg.account_mode = is_pm ? TradingClient::AccountMode::PortfolioMargin
                                : TradingClient::AccountMode::Futures;
    const std::string net_name = cfg.testnet ? "测试网" : (is_pm ? "统一账户(主网)" : "主网");

    for (const auto& w : cfg.warnings) log_line("⚠ 配置警告: " + w, "WARN");

    auto client = std::make_shared<TradingClient>(tc_cfg);
    const auto first_sync = client->sync_server_time();
    // 探测持仓模式（单向/双向）。此前这个查询【从未被调用】，dual_mode_ 一直是
    // 默认的 false，等于把"单向持仓"写死了：账户若是双向，每笔单都缺 positionSide
    // 参数被交易所拒单 -4061，而那个错误码光看字面很难联想到是这里
    const bool dual_mode = client->fetch_position_mode();
    auto info = client->fetch_account();
    if (!info.ok) {
        log_line("连接失败: " + info.error, "ERR");
        if (!cfg.alert_webhook.empty()) {
            send_webhook(cfg.alert_webhook, "[ccbot] 启动时连接失败: " + info.error);
        }
        { std::error_code ec; std::filesystem::remove(lock_path, ec); }
        return 1;
    }
    log_line("连接成功 | " + net_name +
             " | 权益 $" + std::to_string(info.total_equity) +
             " | 可用 $" + std::to_string(info.available) +
             (info.uni_mmr > 0 ? " | uniMMR " + std::to_string(info.uni_mmr) : "") +
             " | 持仓模式 " + (dual_mode ? "双向（带 positionSide）" : "单向"), "OK");
    // 启动这一条保留：一次性的，而且本机与交易所的时差是排查任何时间戳问题的起点
    log_line(first_sync.to_log(), first_sync.accepted ? "OK" : "WARN");

    auto pool   = std::make_shared<ThreadPool>(4);
    auto engine = std::make_shared<CcgEngine>(client, pool);
    engine->set_max_total_margin(cfg.max_total_margin);
    engine->set_max_open_positions(cfg.max_open_positions);

    std::atomic<bool> state_dirty{false};
    const std::string webhook = cfg.alert_webhook;

    engine->set_log_cb([&](const std::string& msg) {
        log_line(msg);
        state_dirty.store(true);
    });

    // 交易明细 CSV 落盘（周期统计数据源）：追加写，一行一笔平仓
    const std::string trades_csv = "ccbot_trades.csv";
    if (!std::filesystem::exists(trades_csv)) {
        std::ofstream f(trades_csv);
        if (f) f << "time,symbol,direction,reason,entry_price,exit_price,qty,pnl,layers\n";
    }

    engine->set_trade_cb([&](const TradeRecord& tr) {
        std::ostringstream ss;
        ss << tr.symbol << " " << tr.reason
           << " 均=$" << tr.entry_price << " 收=$" << tr.exit_price
           << " P&L=" << tr.pnl << "U 层数=" << tr.layers;
        log_line(ss.str(), tr.pnl >= 0 ? "OK" : "WARN");
        {
            std::ofstream f(trades_csv, std::ios::app);
            if (f) f << now_str() << "," << tr.symbol << ","
                     << (tr.direction == CcgConfig::Direction::Short ? "short" : "long") << ","
                     << tr.reason << "," << tr.entry_price << "," << tr.exit_price << ","
                     << tr.qty << "," << tr.pnl << "," << tr.layers << "\n";
        }
        if (tr.reason == "硬止损" && !webhook.empty()) {
            std::ostringstream alert_msg;
            alert_msg << "[ccbot] " << tr.symbol << " 触发硬止损平仓 | 均价 $" << tr.entry_price
                      << " -> 平仓 $" << tr.exit_price << " | 盈亏 " << tr.pnl << "U";
            std::thread([w = webhook, text = alert_msg.str()]() {
                send_webhook(w, text);
            }).detach();
        }
    });

    // 恢复上次落盘的仓位状态；配置文件里已经删掉的品种/方向落盘状态会被丢弃，
    // 配置里新增、落盘状态没有的品种按配置全新起步（立即开始监控，无界面没有"先停止"这一步）
    auto saved = load_headless_state(cfg.state_path, cfg.bots);
    std::set<std::string> restored_keys;
    for (auto& b : saved) {
        auto id = engine->restore_bot(b);
        if (!id.empty()) {
            restored_keys.insert(b.cfg.symbol + "|" + std::to_string((int)b.cfg.direction));
            log_line(b.cfg.symbol + " 从落盘状态恢复（均价=$" + std::to_string(b.avg_price) +
                     " 持仓=" + std::to_string(b.total_qty) + "）");
        }
    }
    for (const auto& c : cfg.bots) {
        std::string key = c.symbol + "|" + std::to_string((int)c.direction);
        if (restored_keys.count(key)) continue;
        auto id = engine->add_bot(c);
        if (!id.empty()) log_line(c.symbol + " 新建 bot，按配置文件立即开始监控");
    }

    // 启动对账：本地落盘的仓位 vs 交易所实际持仓（外部手动平过仓/强平过的话本地状态是错的）
    {
        auto ex_pos = client->fetch_positions();
        std::vector<CcgEngine::ExchangePos> ex;
        for (const auto& p : ex_pos) ex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
        auto issues = engine->reconcile_positions(ex);
        if (!issues.empty()) {
            save_headless_state(cfg.state_path, engine->get_bots());   // 把收敛后的状态立刻落盘
            if (!cfg.alert_webhook.empty()) {
                std::string msg = "[ccbot] 启动对账发现 " + std::to_string(issues.size()) + " 处不一致:";
                for (const auto& s : issues) msg += "\n" + s;
                send_webhook(cfg.alert_webhook, msg);
            }
        }
        // 对账之后重建交易所侧灾难止损单——必须等本地持仓收敛到真相，
        // 否则会照着一个错误的均价挂止损
        engine->resync_disaster_stops();
    }

    // 24h 涨幅的 REST 兜底缓存（全市场一次取回，90 秒有效期）
    std::unordered_map<std::string, double> chg24_rest;
    int64_t chg24_rest_ms = 0;

    BookTickerStream ticker(cfg.testnet);
    // 订阅被拒等服务端消息此前静默丢弃：VPS 上没有界面，这类问题只能靠日志发现
    ticker.on_server_msg([](const std::string& m) { log_line(m, "WARN"); });
    ticker.start();
    std::set<std::string> symbols;
    for (const auto& c : cfg.bots) symbols.insert(c.symbol);
    for (const auto& s : symbols) ticker.subscribe(s);

    log_line("主循环启动，Ctrl+C 退出");

    // 数据拉取专用线程池（与引擎下单池分离）：指标/SR/趋势/心跳都是几百毫秒~几秒的
    // 阻塞网络调用，原先在主循环里串行执行，行情剧烈+网络劣化时会把价格喂入和
    // 止损/止盈判定饿死几十秒——现在价格喂入永远最先、拉取全部异步。
    // 注意声明顺序：busy标记/互斥量/区域表必须在 fetch_pool 之前声明——析构是
    // 逆序的，池要最先销毁（join工人线程），否则在途任务会引用已析构的局部变量
    std::atomic<bool> ind_busy{false}, sr_busy{false}, trend_busy{false}, hb_busy{false}, rec_busy{false};
    std::mutex sr_mtx;   // sr_zones_map/sr_atr_map 由拉取线程写、主循环读
    std::map<std::string, std::vector<srzones::Zone>> sr_zones_map;
    std::map<std::string, double> sr_atr_map;   // 区域计算时的ATR（结构止损位推导）
    auto fetch_pool = std::make_shared<ThreadPool>(2);

    // ── 资金费账本 ───────────────────────────────────────────────────────────
    // 只记账不参与决策：永续每 8 小时结算一次，这是真实划走的现金，不是浮亏。
    // 对长期持有的仓位，它是唯一一笔价格涨回来也拿不回的成本
    FundingLedger funding;
    const std::string funding_path = cfg.state_path + ".funding";
    funding.load(funding_path);
    bool funding_backfilled = false;
    std::atomic<bool> fund_busy{false};
    std::set<std::string> fund_alerted;   // 年化超阈值已告警的品种

    // ── 看门狗状态 ───────────────────────────────────────────────────────────
    // 交易系统最阴的故障不是崩溃（崩溃至少进程没了，外部能看出来），而是
    // "进程还在、但已经不干活了"：喂价全部返回0、心跳一直失败、循环卡住。
    // 这种状态下仓位无人管理，而所有监控指标看起来都活着。
    const std::string alive_path = cfg.state_path + ".alive";
    std::map<std::string, int> stall_ticks;    // sym → 连续取不到价格的 tick 数
    std::set<std::string> stall_alerted;       // 已告警的品种，恢复后清除
    std::atomic<int> hb_fail_streak{0};
    std::atomic<bool> hb_alerted{false};

    int tick_n = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ++tick_n;

        auto bots = engine->get_bots();

        // ── 1) 价格喂入 + 策略判定：永远最先执行，不被任何数据拉取阻塞 ────────
        for (const auto& sym : symbols) {
            double price = ticker.mark_price(sym);   // 内置10秒陈旧保护，冻结价返回0
            if (price <= 0) {
                price = client->fetch_mark_price(sym);
                // 写回缓存：headless 没有界面，但状态落盘与日志同样读它
                if (price > 0) ticker.set_mark_price(sym, price);
            }
            if (price > 0) {
                engine->tick(sym, price);
                stall_ticks[sym] = 0;
                if (stall_alerted.erase(sym)) {
                    log_line(sym + " 行情已恢复", "OK");
                    if (!webhook.empty()) {
                        std::thread([w = webhook, s = sym]() {
                            send_webhook(w, "[ccbot] " + s + " 行情已恢复，策略判定重新运行");
                        }).detach();
                    }
                }
            } else {
                // WS 冻结 + REST 也拿不到价：该品种的止盈/止损/补仓全部停摆。
                // 有持仓的时候这等同于仓位无人看管，必须叫人
                int n = ++stall_ticks[sym];
                bool has_pos = false;
                for (const auto& b : bots)
                    if (b.cfg.symbol == sym && b.total_qty > 0) has_pos = true;
                if (n == 20 && has_pos && !stall_alerted.count(sym)) {   // 约1分钟
                    stall_alerted.insert(sym);
                    log_line(sym + " 连续1分钟取不到价格，该品种策略判定已停摆（有持仓!）", "ERR");
                    if (!webhook.empty()) {
                        std::thread([w = webhook, s = sym]() {
                            send_webhook(w, "[ccbot] ⚠ " + s + " 连续1分钟取不到价格，"
                                            "止盈/止损/补仓全部停摆，且该品种有持仓——请检查网络");
                        }).detach();
                    }
                }
            }
        }

        // ── 1b) 24h 滚动涨幅：优先读 @ticker 推送缓存（零请求），
        //     推送流不可用时回落到全市场 REST 快照。
        //     这条数据原本是全系统【唯一没有兜底】的：WS 一断，高位拦截在
        //     strict 下永久拦死，一单也开不出来
        {
            bool need_rest_chg = false;
            for (const auto& b : engine->get_bots()) {
                if (b.state == CcgBot::State::Stopped) continue;
                if (b.cfg.htf_24h_chg_max <= 0) continue;
                double pct = 0;
                bool ok = ticker.change_24h(b.cfg.symbol, pct);
                if (!ok) {
                    auto it = chg24_rest.find(b.cfg.symbol);
                    if (it != chg24_rest.end() &&
                        BookTickerStream::now_ms() - chg24_rest_ms < 90000) {
                        pct = it->second; ok = true;
                    } else {
                        need_rest_chg = true;
                    }
                }
                engine->update_24h_change(b.bot_id, ok, pct);
            }
            // 全市场一次取回（权重 40），不是逐品种——品种一多逐个查
            // 既费往返又费权重。同步调用即可：90 秒才会真正触发一次
            if (need_rest_chg) {
                auto m = client->fetch_all_24h_changes();
                if (!m.empty()) { chg24_rest = std::move(m); chg24_rest_ms = BookTickerStream::now_ms(); }
            }
        }

        // ── 1c) 心跳文件（dead-man's switch）：每 tick 写入当前时间戳。
        //     外部看门狗（systemd WatchdogSec / cron）检查这个文件的年龄就能发现
        //     "进程还在但循环卡住"——这是日志和进程存活检查都发现不了的故障
        {
            std::ofstream hf(alive_path, std::ios::trunc);
            if (hf) {
                hf << std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count()
                   << " tick=" << tick_n << " bots=" << bots.size() << "\n";
            }
        }

        // ── 1b) v3.0 结构摘要喂入（纯本地计算）────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(sr_mtx);
            if (!sr_zones_map.empty()) {
                for (const auto& b : bots) {
                    auto zit = sr_zones_map.find(b.cfg.symbol);
                    if (zit == sr_zones_map.end() || zit->second.empty()) continue;
                    double price = ticker.mark_price(b.cfg.symbol);
                    if (price <= 0) continue;
                    auto dg = decision::digest_zones(zit->second, price, b.cfg.sr_min_confluence);
                    double atr = sr_atr_map.count(b.cfg.symbol) ? sr_atr_map[b.cfg.symbol] : 0;
                    double stop_level = (dg.deep_sup_lo > 0 && atr > 0)
                                        ? dg.deep_sup_lo - 0.25 * atr : 0;
                    engine->update_sr_structure(b.bot_id, dg.at_support, dg.sup_hi,
                                                dg.res_lo, stop_level);
                }
            }
        }

        // ── 2) 指标拉取（异步，busy标记防任务堆积）───────────────────────────
        if (!ind_busy.load()) {
            std::vector<CcgBot> need;
            for (const auto& b : bots)
                if (b.state == CcgBot::State::Running &&
                    ((b.entries.empty() && b.cfg.entry_mode == CcgConfig::EntryMode::Indicator) ||
                     b.cfg.dynamic_band_mode))
                    need.push_back(b);
            if (!need.empty()) {
                ind_busy.store(true);
                fetch_pool->submit([client, engine, need, &ind_busy]() {
                    for (const auto& b : need) {
                        auto snap = client->fetch_indicators(b.cfg.symbol, b.cfg.kline_interval,
                                                              b.cfg.boll_period, b.cfg.boll_mult,
                                                              b.cfg.rsi_period);
                        if (!snap.ok) continue;
                        engine->update_indicator(b.bot_id, snap.boll_lb, snap.boll_ub, snap.rsi);
                        // 复用：指标拉的就是 1h 带，正好是多周期梯子的第0档
                        if (b.cfg.mtf_ladder && b.cfg.kline_interval == "1h")
                            engine->update_mtf_band(b.bot_id, 0, snap.boll_lb, snap.boll_ub);
                    }
                    ind_busy.store(false);
                });
            }
        }

        // ── 3) SR雷达重算（每约15分钟，异步）；触区检查每tick本地做（零开销）──
        if ((tick_n - 1) % 300 == 0 && !sr_busy.load()) {
            std::vector<std::pair<std::string, std::string>> radar;   // sym, interval
            for (const auto& b : bots)
                if (b.state != CcgBot::State::Stopped && b.cfg.sr_radar)
                    radar.push_back({b.cfg.symbol, b.cfg.sr_interval});
            if (!radar.empty()) {
                sr_busy.store(true);
                fetch_pool->submit([client, radar, &sr_mtx, &sr_zones_map, &sr_atr_map, &sr_busy]() {
                    for (const auto& [sym, interval] : radar) {
                        auto raw = client->fetch_bars(sym, interval, 400);
                        if (raw.size() < 50) continue;
                        std::vector<srzones::Bar> sbars;
                        sbars.reserve(raw.size());
                        for (const auto& r : raw) sbars.push_back({r.open, r.high, r.low, r.close, r.volume});
                        auto zones = srzones::detect_zones(sbars);
                        double atr  = srzones::atr(sbars, 14);
                        std::lock_guard<std::mutex> lk(sr_mtx);
                        sr_zones_map[sym] = std::move(zones);
                        sr_atr_map[sym]   = atr;
                    }
                    sr_busy.store(false);
                });
            }
        }
        // 触区告警已移除：SR 区域现在是三层拦截的内部数据源，不再是需要人盯的事件。
        // 它每 tick 都可能触发，是运行日志里最占地方的一类，而拦截生效后
        // "价格进了某个区域"本身并不需要人做任何事——该拦的闸门已经拦了。

        // ── 4) 趋势状态机 + v3.0日线%B（每约5分钟，异步同班车）────────────────
        if ((tick_n - 1) % 100 == 0 && !trend_busy.load()) {
            std::vector<CcgBot> need, htf_need, mtf_need;
            for (const auto& b : bots) {
                if (b.state == CcgBot::State::Stopped) continue;
                if (b.cfg.use_trend_filter) need.push_back(b);
                // %B 对所有非停止 bot 持续保鲜（立即开仓/冷却重进的首仓才赶得上数据）
                // 涨幅拦截与 %B 同源，任一开启都要拉这份高周期数据
                if (b.cfg.use_htf_filter ||
                    b.cfg.htf_24h_chg_max > 0 || b.cfg.htf_week_chg_max > 0)
                    htf_need.push_back(b);
                if (b.cfg.mtf_ladder)     mtf_need.push_back(b);
            }
            if (!need.empty() || !htf_need.empty() || !mtf_need.empty()) {
                trend_busy.store(true);
                fetch_pool->submit([client, engine, need, htf_need, mtf_need, &trend_busy]() {
                    for (const auto& b : need) {
                        auto t = client->fetch_trend(b.cfg.symbol, b.cfg.trend_interval,
                                                      b.cfg.trend_ema_period);
                        if (t.ok) engine->update_trend(b.bot_id, t.bearish);
                    }
                    for (const auto& b : htf_need) {
                        auto snap = client->fetch_indicators(b.cfg.symbol, b.cfg.htf_interval,
                                                              20, 2.0, 14);
                        if (!snap.ok) continue;
                        double pb = decision::pct_b(snap.price, snap.boll_lb, snap.boll_ub);
                        engine->update_htf(b.bot_id, pb, snap.chg_ok, snap.chg_7);
                        // 复用：宏观层拉的就是日线带，正好是第3档
                        if (b.cfg.mtf_ladder && b.cfg.htf_interval == "1d")
                            engine->update_mtf_band(b.bot_id, 3, snap.boll_lb, snap.boll_ub);
                    }
                    // 多周期梯子还差 4h / 12h 两档（1h 和 1d 上面顺带喂了）。
                    // 每个开了该模式的 bot 只多 2 个公开接口请求
                    for (const auto& b : mtf_need) {
                        const char* tf[2] = {"4h", "12h"};
                        for (int ti = 1; ti <= 2; ++ti) {
                            auto ms = client->fetch_indicators(b.cfg.symbol, tf[ti-1],
                                                               b.cfg.boll_period, b.cfg.boll_mult, 14);
                            if (ms.ok) engine->update_mtf_band(b.bot_id, ti, ms.boll_lb, ms.boll_ub);
                        }
                        if (b.cfg.kline_interval != "1h") {
                            auto ms = client->fetch_indicators(b.cfg.symbol, "1h",
                                                               b.cfg.boll_period, b.cfg.boll_mult, 14);
                            if (ms.ok) engine->update_mtf_band(b.bot_id, 0, ms.boll_lb, ms.boll_ub);
                        }
                        if (b.cfg.htf_interval != "1d") {
                            auto ms = client->fetch_indicators(b.cfg.symbol, "1d",
                                                               b.cfg.boll_period, b.cfg.boll_mult, 14);
                            if (ms.ok) engine->update_mtf_band(b.bot_id, 3, ms.boll_lb, ms.boll_ub);
                        }
                    }
                    trend_busy.store(false);
                });
            }
        }

        // ── 5) 状态落盘：脏标记触发之外每约1分钟强制存一次——tp_extreme/interval_hit
        //     这类追踪变量的变化不产生日志（不置脏），只靠脏标记会永远丢失 ─────
        if (tick_n % 20 == 0) state_dirty.store(true);
        if (state_dirty.exchange(false)) {
            save_headless_state(cfg.state_path, engine->get_bots());
        }

        // ── 6) 心跳（异步，约1分钟一次）─────────────────────────────────────
        if (tick_n % 20 == 0 && !hb_busy.load()) {
            hb_busy.store(true);
            fetch_pool->submit([client, &hb_busy, &hb_fail_streak, &hb_alerted, w = webhook]() {
                auto acc = client->fetch_account();
                if (acc.ok) {
                    log_line("心跳 | 权益 $" + std::to_string(acc.total_equity) +
                             " | 可用 $" + std::to_string(acc.available) +
                             (acc.uni_mmr > 0 ? " | uniMMR " + std::to_string(acc.uni_mmr) : ""));
                    hb_fail_streak.store(0);
                    // 限流状态：只在真的被限速/拒绝过时才打，平时不占日志
                    auto rl = client->rate_status();
                    if (rl.banned)
                        log_line("⚠ 交易所限流封禁中，剩余 " +
                                 std::to_string(rl.ban_left_ms / 1000) + " 秒", "ERR");
                    else if (rl.throttled > 0 || rl.rejected > 0)
                        log_line("限流 | 本分钟权重 " + std::to_string(rl.used_weight) +
                                 "/" + std::to_string(rl.limit) +
                                 " | 本地推迟 " + std::to_string(rl.throttled) +
                                 " 次 | 交易所拒绝 " + std::to_string(rl.rejected) + " 次");
                    if (hb_alerted.exchange(false) && !w.empty())
                        send_webhook(w, "[ccbot] 账户接口已恢复");
                    // 统一账户按【全账户】算强平，uniMMR 是唯一能看到真实距离的数。
                    // 1.05 是币安开始强制减仓的线，留出余量在 1.3 就叫人
                    if (acc.uni_mmr > 0 && acc.uni_mmr < 1.3) {
                        log_line("⚠ uniMMR " + std::to_string(acc.uni_mmr) +
                                 " 已接近强平线（1.05 起强制减仓）", "ERR");
                        if (!w.empty())
                            send_webhook(w, "[ccbot] ⚠ 统一账户 uniMMR " +
                                            std::to_string(acc.uni_mmr) +
                                            "，接近强平线（1.05 起强制减仓），请立即处理");
                    }
                } else {
                    log_line("心跳失败（网络异常?): " + acc.error, "ERR");
                    // -1021 = 时钟漂移超窗，立即重新对时自愈
                    if (acc.error.find("-1021") != std::string::npos)
                    {
                        // 只有异常才吭声——这条路径每次 -1021 都会走到
                        const auto ts = client->sync_server_time();
                        if (ts.noteworthy()) log_line(ts.to_log(), "WARN");
                    }
                    // 连续3次（约3分钟）失败才告警：偶发抖动不值得半夜叫醒人
                    if (hb_fail_streak.fetch_add(1) + 1 >= 3 && !hb_alerted.exchange(true) && !w.empty())
                        send_webhook(w, "[ccbot] ⚠ 账户接口连续3次拉取失败：" + acc.error);
                }
                hb_busy.store(false);
            });
        }

        // ── 6b) 资金费：费率约5分钟一刷，历史流水约1小时一同步 ──────────────
        if (tick_n % 100 == 1 && !fund_busy.load()) {
            fund_busy.store(true);
            std::vector<std::string> fsyms(symbols.begin(), symbols.end());
            // 首次补历史的起点 = 最早那笔持仓的建仓时间
            int64_t earliest = 0;
            for (const auto& b : bots) {
                if (b.entries.empty()) continue;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              b.entries.front().time.time_since_epoch()).count();
                if (ms > 0 && (earliest == 0 || ms < earliest)) earliest = ms;
            }
            const bool do_hist  = !funding_backfilled || (tick_n % 1200 == 1);
            const int64_t bfill = funding_backfilled ? 0 : earliest;
            fetch_pool->submit([client, &funding, &fund_busy, &funding_backfilled,
                                fsyms, do_hist, bfill, funding_path]() {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch()).count();
                for (const auto& s : fsyms) {
                    auto pi = client->fetch_premium(s);
                    if (pi.ok) funding.set_rate(s, pi.funding_rate, pi.next_ms, now_ms);
                }
                if (do_hist) {
                    int added = sync_funding_ledger(*client, funding, fsyms, bfill);
                    if (!funding_backfilled) {
                        funding_backfilled = true;
                        double sum = 0;
                        for (const auto& s : funding.symbols()) sum += funding.get(s).total;
                        log_line("资金费账本已同步 " + std::to_string(added) +
                                 " 条流水，累计 " + std::to_string(sum) + " USDT",
                                 sum < 0 ? "WARN" : "OK");
                    }
                    funding.save(funding_path);
                }
                fund_busy.store(false);
            });
        }

        // ── 6c) 资金费日志与告警（本地计算，零开销）──────────────────────────
        //  告警只报告【持有成本变了】，从不建议平仓——持有决策是使用者的事
        if (tick_n % 20 == 0) {
            for (const auto& b : bots) {
                if (b.total_qty <= 0) continue;
                auto fe = funding.get(b.cfg.symbol);
                if (fe.rate_ms == 0) continue;
                const double ann = FundingLedger::annualized_pct(fe.rate);
                if (fe.since_open != 0) {
                    const bool is_long = (b.cfg.direction == CcgConfig::Direction::Long);
                    double be = FundingLedger::effective_breakeven(
                        b.avg_price, b.total_qty, fe.since_open, is_long);
                    log_line("资金费 | " + b.cfg.symbol +
                             " 本轮已付 " + std::to_string(-fe.since_open) + " USDT" +
                             " | 年化 " + std::to_string(-ann) + "%" +
                             " | 回本价 " + std::to_string(b.avg_price) +
                             " → " + std::to_string(be));
                }
                // 年化持有成本跨过 30% 才叫人，回落到 20% 以下才解除（迟滞，防边界刷屏）
                if (ann > 30.0 && !fund_alerted.count(b.cfg.symbol)) {
                    fund_alerted.insert(b.cfg.symbol);
                    log_line("⚠ " + b.cfg.symbol + " 资金费年化已达 " +
                             std::to_string(ann) + "%，持有成本显著上升", "WARN");
                    if (!webhook.empty()) {
                        std::thread([w = webhook, s = b.cfg.symbol, ann]() {
                            send_webhook(w, "[ccbot] " + s + " 资金费年化 " +
                                            std::to_string(ann) + "%，持有成本显著上升"
                                            "（仅告知，策略未做任何改变）");
                        }).detach();
                    }
                } else if (ann < 20.0) {
                    fund_alerted.erase(b.cfg.symbol);
                }
            }
        }

        // ── 7) 服务器时间重对时（每15分钟，tick=3s）：时钟漂移超 recvWindow
        //     会让所有签名请求集体失败 ────────────────────────────────────────
        // 原为每小时。缩短的依据是实盘抓到本机时钟瞬间步进 ~1 秒、90 秒后跳回，
        // 而步进到下次对时之间会一直带着这个误差发单（超前侧预算仅 2000ms）。
        // 完整论证见 GUI 侧同一处注释。成本可忽略：权重 1、每小时 4 次
        if (tick_n % 300 == 0) {
            fetch_pool->submit([client]() {
                const auto ts = client->sync_server_time();
                // 常规对时静默；只有测量被丢弃或偏移大幅跳变才值得占一行
                if (ts.noteworthy()) log_line(ts.to_log(), "WARN");
            });
        }

        // ── 8) 周期对账（每 20 tick ≈ 1 分钟）───────────────────────────────
        // 此前只在启动时对一次账，所以运行中被外部（手机 App / 网页 / 强平）
        // 平掉的仓位要等到重启才会发现，期间 bot 拿着一个不存在的仓位继续算
        // 止盈止损、继续补仓。
        //
        // 用 Periodic 模式：它会跳过在途和刚成交的 bot，否则正在止盈平仓的
        // 那笔会被当成"外部平仓"清掉。
        // 拉取失败【绝不对账】——空的持仓列表既可能是"确实没仓"也可能是请求
        // 失败，把后者当成前者会凭空清掉真实持仓
        // 必须【异步】：同步调用会把主循环阻塞在这次 HTTP 上（超时最长 10 秒），
        // 期间所有 bot 的 tick 全停——行情不再推进，止盈止损判定跟着停摆。
        // 主循环里除了价格兜底之外的每一个 HTTP 都走 fetch_pool，这里同理
        if (tick_n % 20 == 0 && !rec_busy.load()) {
            rec_busy.store(true);
            fetch_pool->submit([client, engine, &rec_busy,
                                sp = cfg.state_path, w = cfg.alert_webhook]() {
                bool pos_ok = false;
                auto ex_pos = client->fetch_positions(&pos_ok);
                // 拉取失败绝不对账：空的持仓列表既可能是"确实没仓"也可能是请求
                // 失败，把后者当成前者会凭空清掉真实持仓
                if (pos_ok) {
                    std::vector<CcgEngine::ExchangePos> ex;
                    ex.reserve(ex_pos.size());
                    for (const auto& p : ex_pos)
                        ex.push_back({p.symbol, p.direction, p.qty, p.entry_price});
                    auto issues = engine->reconcile_positions(
                        ex, CcgEngine::ReconcileMode::Periodic);
                    // 明细不在这里打——引擎内部已经逐条 log 过（"⚠ 对账: ..."）
                    if (!issues.empty()) {
                        save_headless_state(sp, engine->get_bots());
                        if (!w.empty()) {
                            std::string msg = "[ccbot] 运行中对账发现 " +
                                              std::to_string(issues.size()) + " 处不一致:";
                            for (const auto& s : issues) msg += "\n" + s;
                            send_webhook(w, msg);
                        }
                    }
                }
                rec_busy.store(false);
            });
        }
    }

    log_line("收到退出信号，等待在途订单落地后保存状态…");
    // 与 GUI 同一处理：等在途任务跑完【再落盘】。
    //   · 消除 use-after-free —— 任务捕获 engine 裸指针，而 CcgEngine 里 pool_ 的
    //     声明位置在 mtx_/bots_ 之前，线程池 join 时那两个成员已经析构
    //   · 让在途订单的结果进得了状态文件 —— 否则 SIGTERM 瞬间正在成交的那笔
    //     本地无记录，重启只能靠对账认领，而认领会丢掉层数信息
    // 预算按【单个下单任务的最坏耗时】定，不是按单次 HTTP：一次 place_market
    // 会串起 POST 超时 10s → 空响应 → 查单恢复 3×(600ms+10s) ≈ 42s，
    // 坏网络下很平常。原先取 20 秒远远不够。完整推导见 GUI 侧同一处注释
    const bool drained = pool->wait_idle(60000);
    fetch_pool->wait_idle(5000);

    save_headless_state(cfg.state_path, engine->get_bots());
    funding.save(funding_path);

    // 没排空就直接结束进程，不跑析构。在途任务捏着 engine 的裸指针，而
    // CcgEngine 里 pool_ 的声明位置在 mtx_/bots_ 之前 —— 正常析构会先销毁那两个
    // 成员，任务一访问就是 use-after-free。状态此刻已经落盘，剩下唯一该做的
    // 就是别再碰内存
    if (!drained) {
        log_line("仍有下单任务未完成，跳过清理直接结束进程（避免访问已释放内存）；"
                 "在途成交由重启后的对账兜底", "WARN");
        { std::error_code ec; std::filesystem::remove(alive_path, ec); }
        { std::error_code ec; std::filesystem::remove(lock_path,  ec); }
        std::_Exit(0);
    }
    // 退出也要通知：进程停了就等于所有本地风控停了，只剩交易所侧的灾难止损单。
    // 这条消息本身就是"从现在起没人在管"的信号
    {
        int with_pos = 0;
        for (const auto& b : engine->get_bots()) if (b.total_qty > 0) ++with_pos;
        if (!webhook.empty())
            send_webhook(webhook, "[ccbot] 进程已退出（" + std::to_string(with_pos) +
                                  " 个品种仍有持仓）——本地止盈/止损从此刻停止，"
                                  "仅交易所侧灾难止损单仍然有效");
    }
    { std::error_code ec; std::filesystem::remove(alive_path, ec); }
    ticker.stop();
    { std::error_code ec; std::filesystem::remove(lock_path, ec); }
    return 0;
}
