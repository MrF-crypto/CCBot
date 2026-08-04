// ccbot_headless：无图形界面版本，配置文件驱动，Windows/Linux 都能编译运行。
// 用法：ccbot_headless [配置文件路径，默认 config.json]
#include "core/ccg_engine.h"
#include "core/sr_zones.h"
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

    log_line("ccbot headless 启动，配置文件: " + config_path + "，共 " +
             std::to_string(cfg.bots.size()) + " 个 bot");

    TradingClient::Config tc_cfg;
    tc_cfg.api_key    = cfg.api_key;
    tc_cfg.api_secret = cfg.api_secret;
    tc_cfg.testnet    = cfg.testnet;

    for (const auto& w : cfg.warnings) log_line("⚠ 配置警告: " + w, "WARN");

    auto client = std::make_shared<TradingClient>(tc_cfg);
    client->sync_server_time();
    auto info = client->fetch_account();
    if (!info.ok) {
        log_line("连接失败: " + info.error, "ERR");
        if (!cfg.alert_webhook.empty()) {
            send_webhook(cfg.alert_webhook, "[ccbot] 启动时连接失败: " + info.error);
        }
        { std::error_code ec; std::filesystem::remove(lock_path, ec); }
        return 1;
    }
    log_line("连接成功 | " + std::string(cfg.testnet ? "测试网" : "主网") +
             " | 权益 $" + std::to_string(info.total_equity) +
             " | 可用 $" + std::to_string(info.available), "OK");

    auto pool   = std::make_shared<ThreadPool>(4);
    auto engine = std::make_shared<CcgEngine>(client, pool);
    engine->set_max_total_margin(cfg.max_total_margin);

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
    }

    BookTickerStream ticker(cfg.testnet);
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
    std::atomic<bool> ind_busy{false}, sr_busy{false}, trend_busy{false}, hb_busy{false};
    std::mutex sr_mtx;   // sr_zones_map 由拉取线程写、主循环读
    std::map<std::string, std::vector<srzones::Zone>> sr_zones_map;
    std::map<std::string, std::pair<double, int64_t>>  sr_alert_dedup; // sym→{mid,ms}（仅主循环访问）
    auto fetch_pool = std::make_shared<ThreadPool>(2);

    int tick_n = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ++tick_n;

        auto bots = engine->get_bots();

        // ── 1) 价格喂入 + 策略判定：永远最先执行，不被任何数据拉取阻塞 ────────
        for (const auto& sym : symbols) {
            double price = ticker.mid_price(sym);   // 内置10秒陈旧保护，冻结价返回0
            if (price <= 0) price = client->fetch_mark_price(sym);
            if (price > 0) engine->tick(sym, price);
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
                        if (snap.ok) engine->update_indicator(b.bot_id, snap.boll_lb, snap.boll_ub, snap.rsi);
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
                fetch_pool->submit([client, radar, &sr_mtx, &sr_zones_map, &sr_busy]() {
                    for (const auto& [sym, interval] : radar) {
                        auto raw = client->fetch_bars(sym, interval, 400);
                        if (raw.size() < 50) continue;
                        std::vector<srzones::Bar> sbars;
                        sbars.reserve(raw.size());
                        for (const auto& r : raw) sbars.push_back({r.open, r.high, r.low, r.close, r.volume});
                        auto zones = srzones::detect_zones(sbars);
                        std::lock_guard<std::mutex> lk(sr_mtx);
                        sr_zones_map[sym] = std::move(zones);
                    }
                    sr_busy.store(false);
                });
            }
        }
        {
            std::lock_guard<std::mutex> lk(sr_mtx);
            for (const auto& [sym, zones] : sr_zones_map) {
                double price = ticker.mid_price(sym);
                if (price <= 0) continue;
                for (const auto& z : zones) {
                    if (!z.contains(price)) continue;
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    auto& [last_mid, last_ms] = sr_alert_dedup[sym];
                    bool same = std::fabs(z.mid() - last_mid) < 1e-9;
                    if (same && now_ms - last_ms < 2 * 3600 * 1000) break;
                    last_mid = z.mid(); last_ms = now_ms;
                    std::ostringstream ss;
                    int conf = z.confluence();
                    ss << "[SR雷达] " << sym << " 价格 " << price << " 进入区域["
                       << srzones::src_label(z);
                    if (conf >= 2) ss << " ×" << conf << "共振";
                    ss << "]（" << z.lo << " ~ " << z.hi << "，评分" << z.score
                       << "，触碰" << z.touches << "次）";
                    log_line(ss.str(), "WARN");
                    if (!webhook.empty()) {
                        std::thread([w = webhook, text = ss.str()]() { send_webhook(w, text); }).detach();
                    }
                    break;
                }
            }
        }

        // ── 4) 趋势状态机（每约5分钟，异步）──────────────────────────────────
        if ((tick_n - 1) % 100 == 0 && !trend_busy.load()) {
            std::vector<CcgBot> need;
            for (const auto& b : bots)
                if (b.state != CcgBot::State::Stopped && b.cfg.use_trend_filter)
                    need.push_back(b);
            if (!need.empty()) {
                trend_busy.store(true);
                fetch_pool->submit([client, engine, need, &trend_busy]() {
                    for (const auto& b : need) {
                        auto t = client->fetch_trend(b.cfg.symbol, b.cfg.trend_interval,
                                                      b.cfg.trend_ema_period);
                        if (t.ok) engine->update_trend(b.bot_id, t.bearish);
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
            fetch_pool->submit([client, &hb_busy]() {
                auto acc = client->fetch_account();
                if (acc.ok) {
                    log_line("心跳 | 权益 $" + std::to_string(acc.total_equity) +
                             " | 可用 $" + std::to_string(acc.available));
                } else {
                    log_line("心跳失败（网络异常?): " + acc.error, "ERR");
                }
                hb_busy.store(false);
            });
        }

        // ── 7) 服务器时间重对时（约1小时一次）：时钟漂移超 recvWindow 会让所有
        //     签名请求集体失败 ────────────────────────────────────────────────
        if (tick_n % 1200 == 0) {
            fetch_pool->submit([client]() { client->sync_server_time(); });
        }
    }

    log_line("收到退出信号，保存状态后退出");
    save_headless_state(cfg.state_path, engine->get_bots());
    ticker.stop();
    { std::error_code ec; std::filesystem::remove(lock_path, ec); }
    return 0;
}
