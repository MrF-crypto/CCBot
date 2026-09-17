#pragma once
#include <chrono>
#include <functional>

// 引擎的外部时间源与执行器。CcgEngine（DCA 网格）与 SarEngine（趋势反转）
// 共用同一套注入方式——所以它住在这里，而不是任何一个引擎的头文件里。
//
// 时钟必须可注入：回放一年数据只需几秒，用真实时钟的话冷却永远走不完、
// 指标新鲜度检查永远通过，回放结果完全失真。
namespace ccbot {

struct EngineHost {
    // 墙钟（冷却计时、成交时间戳）
    std::function<std::chrono::system_clock::time_point()> now_wall =
        []{ return std::chrono::system_clock::now(); };
    // 单调时钟（指标/趋势/结构数据的新鲜度检查）
    std::function<std::chrono::steady_clock::time_point()> now_steady =
        []{ return std::chrono::steady_clock::now(); };
    // 异步执行器（实盘=线程池；测试/回放=内联同步，保证确定性可复现）
    std::function<void(std::function<void()>)> submit;
};

} // namespace ccbot
