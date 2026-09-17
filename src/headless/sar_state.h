#pragma once
#include "core/sar_engine.h"
#include <string>
#include <vector>

// SAR bot 的运行时状态落盘，重启续跑用。
//
// 这不是"锦上添花"的功能：不落盘的话，进程重启后引擎以为自己空仓，而交易所上
// 的仓位还在——下一个突破信号会再开一笔，净敞口翻倍且原来那笔没有任何止损线
// 守着。DCA 版重启丢状态只是少了摊薄记录，SAR 版重启丢状态是直接的风险敞口。
//
// 与 headless_state.h 分开：两套策略的字段完全不同（SAR 没有 entries/均价/层数），
// 塞进同一个文件会让两边的读写互相牵制。落盘也是两个独立文件。
namespace ccbot {

// 只存运行时状态，不存策略参数——参数每次都从配置文件重新读
void save_sar_state(const std::string& path, const std::vector<SarBot>& bots);

// 按 symbol 匹配配置里的 bot，把落盘的运行时状态拼回一个新的 SarBot
// （cfg 用传入的最新配置，其余用落盘值），交给 SarEngine::restore_bot() 用。
// 配置里已删除的品种，其落盘状态会被丢弃
std::vector<SarBot> load_sar_state(const std::string& path,
                                   const std::vector<SarConfig>& cfgs);

} // namespace ccbot
