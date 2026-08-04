#pragma once
#include "core/ccg_engine.h"
#include <string>
#include <vector>

namespace ccbot {

// 无图形界面版本的启动配置：全部来自一个 JSON 配置文件，不走 DPAPI/GUI 那套。
// 字段名跟 GUI 策略弹窗一一对应，语义完全一致（见 README-headless.md）。
struct HeadlessConfig {
    std::string api_key;
    std::string api_secret;
    bool        testnet          = false;
    double      max_total_margin = 0;    // 0=不限，同 GUI 的账户总保证金上限
    std::string alert_webhook;           // 同 GUI 的关键事件提醒 webhook
    std::string state_path = "ccbot_state.json";   // 仓位运行时状态落盘路径，重启续跑用
    std::string log_path;                // 空=只输出到 stdout，不落盘
    std::vector<CcgConfig> bots;
    std::vector<std::string> warnings;   // 非致命配置问题（未知键等），启动时打给用户看
};

// 从 JSON 文件加载配置；失败返回 false 并把原因写进 err
bool load_headless_config(const std::string& path, HeadlessConfig& out, std::string& err);

} // namespace ccbot
