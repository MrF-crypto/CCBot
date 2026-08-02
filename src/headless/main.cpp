// ccbot_headless：无图形界面版本，配置文件驱动，Windows/Linux 都能编译运行。
// 用法：ccbot_headless [配置文件路径，默认 config.json]
#include "core/ccg_engine.h"
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

    auto client = std::make_shared<TradingClient>(tc_cfg);
    client->sync_server_time();
    auto info = client->fetch_account();
    if (!info.ok) {
        log_line("连接失败: " + info.error, "ERR");
        if (!cfg.alert_webhook.empty()) {
            send_webhook(cfg.alert_webhook, "[ccbot] 启动时连接失败: " + info.error);
        }
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

    engine->set_trade_cb([&](const TradeRecord& tr) {
        std::ostringstream ss;
        ss << tr.symbol << " " << tr.reason
           << " 均=$" << tr.entry_price << " 收=$" << tr.exit_price
           << " P&L=" << tr.pnl << "U";
        log_line(ss.str(), tr.pnl >= 0 ? "OK" : "WARN");
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
        for (const auto& p : ex_pos) ex.push_back({p.symbol, p.direction, p.qty});
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

    int tick_n = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ++tick_n;

        auto bots = engine->get_bots();

        // 指标拉取（公开接口，不占签名限流）：等首单信号的 bot + 动态W模式的 bot
        // （动态模式持仓中也要持续拉取——补仓锚定下轨/止盈锚定上轨都依赖实时轨道）
        for (const auto& b : bots) {
            if (b.state == CcgBot::State::Running &&
                ((b.entries.empty() && b.cfg.entry_mode == CcgConfig::EntryMode::Indicator) ||
                 b.cfg.dynamic_band_mode)) {
                auto snap = client->fetch_indicators(b.cfg.symbol, b.cfg.kline_interval,
                                                      b.cfg.boll_period, b.cfg.boll_mult,
                                                      b.cfg.rsi_period);
                if (snap.ok) engine->update_indicator(b.bot_id, snap.boll_lb, snap.boll_ub, snap.rsi);
            }
        }

        for (const auto& sym : symbols) {
            double price = ticker.mid_price(sym);
            if (price <= 0) price = client->fetch_mark_price(sym);
            if (price > 0) engine->tick(sym, price);
        }

        if (state_dirty.exchange(false)) {
            save_headless_state(cfg.state_path, engine->get_bots());
        }

        // 每 20 个 tick（约1分钟）打一次心跳，确认程序还活着、网络还通
        if (tick_n % 20 == 0) {
            auto acc = client->fetch_account();
            if (acc.ok) {
                log_line("心跳 | 权益 $" + std::to_string(acc.total_equity) +
                         " | 可用 $" + std::to_string(acc.available));
            } else {
                log_line("心跳失败（网络异常?): " + acc.error, "ERR");
            }
        }
    }

    log_line("收到退出信号，保存状态后退出");
    save_headless_state(cfg.state_path, engine->get_bots());
    ticker.stop();
    { std::error_code ec; std::filesystem::remove(lock_path, ec); }
    return 0;
}
