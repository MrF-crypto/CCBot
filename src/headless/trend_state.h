#pragma once
#include "core/trend_engine.h"
#include <string>
#include <vector>

// 趋势 bot 的运行时状态落盘，重启续跑用。
//
// 这不是"锦上添花"的功能：不落盘的话，进程重启后引擎以为自己空仓，而交易所上
// 的仓位还在——下一个信号会再开一笔，净敞口翻倍且原来那笔没有任何止损线守着。
// （对比一下：网格 DCA 重启丢状态只是少了摊薄记录，趋势策略重启丢状态
// 是直接的风险敞口——这也是它当年被独立出一套落盘的原因。）
//
// ⚠ 落盘路径仍是 <state_path>.sar，【故意不改】。
//   代码里的 SarEngine 已经改名成 TrendEngine（理由：三个策略里有一个叫 SAR，
//   顶层再叫 Sar 就永远说不清），但磁盘上那个文件名是【对外契约】——
//   老部署升级上来时它已经存在，改名等于把在跑的仓位状态丢掉。
//   同理 headless 配置里的 JSON 键仍是 "sar_bots"。
namespace ccbot {

// 只存运行时状态，不存策略参数——参数每次都从配置文件重新读
void save_trend_state(const std::string& path, const std::vector<TrendBot>& bots);

// 按 symbol 匹配配置里的 bot，把落盘的运行时状态拼回一个新的 TrendBot
// （cfg 用传入的最新配置，其余用落盘值），交给 TrendEngine::restore_bot() 用。
// 配置里已删除的品种，其落盘状态会被丢弃
std::vector<TrendBot> load_trend_state(const std::string& path,
                                   const std::vector<TrendConfig>& cfgs);

} // namespace ccbot
