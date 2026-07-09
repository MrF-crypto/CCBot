#pragma once
#include "core/ccg_engine.h"
#include <string>
#include <vector>

namespace ccbot {

// Bot 运行时状态（仓位/均价/加仓记录等）落盘，重启续跑用——不含 cfg 里的策略参数
// （策略参数每次都从配置文件重新读，运行时状态才需要跨重启保留）。
void save_headless_state(const std::string& path, const std::vector<CcgBot>& bots);

// 按 symbol+direction 匹配配置里的 bot，把落盘的运行时状态字段拼回一个新的 CcgBot
// （cfg 用传入的最新配置，其余运行时字段用落盘值），交给 CcgEngine::restore_bot() 用
std::vector<CcgBot> load_headless_state(const std::string& path, const std::vector<CcgConfig>& cfgs);

} // namespace ccbot
