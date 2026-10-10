#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace ccbot {

// 信号拉取的并发数（GUI 与 headless 共用）。4 路：100 个 bot 每次约 0.5 秒，
// 一轮十几秒；限流上每分钟约 100 权重，远在闸门之内
inline constexpr int kSignalFetchWorkers = 4;

// 把 [0, n) 分给 workers 个线程并行跑 fn(i)，全部做完才返回。调用线程自己也干活。
//
// 用在"逐品种拉一次 REST"这类 I/O 密集的循环上：100 个 bot 的信号串行拉要一两分钟，
// 超过刷新周期。HTTP 客户端自带连接池和限流闸门，并发是安全的。
// ⚠ fn 不得抛异常——跨线程的异常会直接 terminate。调用方在 fn 里自己兜住
inline void parallel_for(int n, int workers, const std::function<void(int)>& fn) {
    if (n <= 0) return;
    workers = std::clamp(workers, 1, n);
    std::atomic<int> next{0};
    auto run = [&] {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= n) break;
            fn(i);
        }
    };
    std::vector<std::thread> ts;
    ts.reserve(workers - 1);
    for (int w = 1; w < workers; ++w) ts.emplace_back(run);
    run();
    for (auto& t : ts) t.join();
}

} // namespace ccbot
