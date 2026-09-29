#pragma once
#include <chrono>
#include <functional>

// 引擎的外部时间源与执行器。单独一个头文件而不是塞进 trend_engine.h：它描述的是
// "引擎怎么被驱动"这件事本身，与具体策略无关——当初 DCA 与 SAR 并列时两套共用它，
// 将来加第二套策略也照样共用。
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
