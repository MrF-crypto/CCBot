#pragma once
#include "core/sar_engine.h"
#include <string>
#include <vector>

// SAR bot 的运行时状态落盘，重启续跑用。
//
// 这不是"锦上添花"的功能：不落盘的话，进程重启后引擎以为自己空仓，而交易所上
// 的仓位还在——下一个突破信号会再开一笔，净敞口翻倍且原来那笔没有任何止损线
// 守着。（对比一下：网格 DCA 重启丢状态只是少了摊薄记录，趋势策略重启丢状态
// 是直接的风险敞口——这也是它当年被独立出一套落盘的原因。）
//
// 文件名仍带 sar_ 前缀、落盘路径仍是 <state_path>.sar：老部署升级上来时那个文件
// 已经存在，改名等于把在跑的仓位状态丢掉。
namespace ccbot {

// 只存运行时状态，不存策略参数——参数每次都从配置文件重新读
void save_sar_state(const std::string& path, const std::vector<SarBot>& bots);

// 按 symbol 匹配配置里的 bot，把落盘的运行时状态拼回一个新的 SarBot
// （cfg 用传入的最新配置，其余用落盘值），交给 SarEngine::restore_bot() 用。
// 配置里已删除的品种，其落盘状态会被丢弃
std::vector<SarBot> load_sar_state(const std::string& path,
                                   const std::vector<SarConfig>& cfgs);

} // namespace ccbot
