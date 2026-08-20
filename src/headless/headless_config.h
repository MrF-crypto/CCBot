#pragma once
#include "core/ccg_engine.h"
#include "core/scanner.h"
#include <string>
#include <vector>

namespace ccbot {

// 无图形界面版本的启动配置：全部来自一个 JSON 配置文件，不走 DPAPI/GUI 那套。
// 字段名跟 GUI 策略弹窗一一对应，语义完全一致（见 README-headless.md）。
struct HeadlessConfig {
    std::string api_key;
    std::string api_secret;
    bool        testnet          = false;
    // "futures"(默认) = 普通合约账户 fapi；"portfolio_margin" = 统一账户 papi。
    // 统一账户没有测试网，选它时 testnet 会被强制置 false
    std::string account_mode     = "futures";
    double      max_total_margin = 0;    // 0=不限，同 GUI 的账户总保证金上限
    // 账户级并发持仓上限（0=不限）。与保证金上限是两道不同的闸：
    // 前者管"总共投多少钱"，这个管"同时压在几个品种上"
    int         max_open_positions = 0;
    std::string alert_webhook;           // 同 GUI 的关键事件提醒 webhook
    std::string state_path = "ccbot_state.json";   // 仓位运行时状态落盘路径，重启续跑用
    std::string log_path;                // 空=只输出到 stdout，不落盘
    std::vector<CcgConfig> bots;
    std::vector<std::string> warnings;   // 非致命配置问题（未知键等），启动时打给用户看

    // ── 全市场扫描模式（默认关）────────────────────────────────────────────
    // 与 bots 列表【可以共存】：bots 里的是常驻品种（比如你长期持有的 BTC），
    // 扫描器额外动态开仓。两者共享同一套账户级闸门（保证金上限、并发上限），
    // 所以常驻仓位会自动占用扫描器的额度——这是对的，账户只有一个。
    struct ScannerSection {
        bool   enabled       = false;
        int    interval_secs = 60;     // 扫描周期。1h K线一小时才变一次，60秒足够
        double budget_per_position = 0; // 每个扫描仓位的预算（0=用 template 里的）
        ScannerConfig cfg;
        // 扫描命中后用这套参数建 bot。symbol 会被逐个覆盖
        CcgConfig     templ;
    } scanner;
};

// 从 JSON 文件加载配置；失败返回 false 并把原因写进 err
bool load_headless_config(const std::string& path, HeadlessConfig& out, std::string& err);

} // namespace ccbot
