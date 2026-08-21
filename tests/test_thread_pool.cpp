// 线程池的关停语义测试。
//
// 起因是第三轮审计发现的一条 use-after-free：任务的 lambda 捕获调用方的 this，
// 而 CcgEngine 里 pool_ 的声明位置在 mtx_/bots_ 之前——线程池 join 的时候那两个
// 成员早就析构了，在途任务一访问就是 UAF。
//
// 修法是让调用方在成员开始销毁之前显式等排空。那么 wait_idle 就必须真的可靠：
// 它要等的是"没有在途任务"，而不是"队列空"——一笔下单的 HTTP 可能已经出队、
// 正在网络上跑，而队列是空的。这两者的区别正是这套测试的核心。
#include "core/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace ccbot;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++g_fail;
}

int main() {
    // ── ① 核心：队列已空但任务还在跑时，wait_idle 必须继续等 ────────────────
    // 如果实现只看 tasks_.empty()，这条会失败——而那正是 UAF 的入口
    {
        ThreadPool p(2);
        std::atomic<bool> started{false}, finished{false};
        p.submit([&] {
            started.store(true);            // 已出队、开始执行 —— 此刻队列是空的
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            finished.store(true);
        });
        // 自旋等到任务【确实开始执行】，而不是 sleep 一个拍脑袋的时长。
        // 原先用 sleep(60ms) 制造这个时机，在 macOS arm64 上实测睡了约 165ms，
        // 于是后面"至少阻塞 250ms"的断言就崩了——CI 抓到的正是这个。
        // 基于绝对时长的断言在不同硬件的调度粒度下天然不可靠
        while (!started.load()) std::this_thread::yield();

        auto t0 = std::chrono::steady_clock::now();
        bool ok = p.wait_idle(5000);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();

        check(ok, "wait_idle 返回成功");
        // 这一条才是核心判据：队列早就空了，它必须等到任务真正执行完
        check(finished.load(), "  返回时任务确实已经执行完（不是只等到队列空）");
        // 时长只做辅助确认"没有立即返回"，阈值放宽——精确时长不该被断言
        check(ms >= 100, "  确实阻塞等待了（实际 " + std::to_string(ms) + "ms）");
    }

    // ── ② 多任务：全部完成才返回 ────────────────────────────────────────────
    {
        ThreadPool p(4);
        std::atomic<int> done{0};
        for (int i = 0; i < 20; ++i)
            p.submit([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                ++done;
            });
        check(p.wait_idle(10000), "20 个任务：wait_idle 成功");
        check(done.load() == 20, "  全部 20 个都执行完了（实际 " +
                                 std::to_string(done.load()) + "）");
    }

    // ── ③ 空闲池上调用应立即返回 ────────────────────────────────────────────
    {
        ThreadPool p(2);
        auto t0 = std::chrono::steady_clock::now();
        check(p.wait_idle(5000), "空闲池 wait_idle 成功");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        check(ms < 100, "  立即返回，不空等（实际 " + std::to_string(ms) + "ms）");
    }

    // ── ④ 超时兜底：卡死的任务不能让关窗口变成假死 ──────────────────────────
    {
        ThreadPool p(1);
        std::atomic<bool> release{false}, running{false};
        p.submit([&] {
            running.store(true);
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (!running.load()) std::this_thread::yield();   // 同上，不用固定 sleep

        auto t0 = std::chrono::steady_clock::now();
        bool ok = p.wait_idle(300);          // 明显短于任务时长
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        check(!ok, "任务卡住时 wait_idle 返回 false，而不是无限等下去");
        // 只验"没有立即返回、也没有无限等"，上界给足余量容纳慢机器的调度抖动
        check(ms >= 200 && ms < 3000, "  在超时附近返回（实际 " + std::to_string(ms) + "ms）");

        release.store(true);                 // 放行，让池能干净析构
        p.wait_idle(5000);
    }

    // ── ⑤ 析构仍然会把已入队的任务执行完（原有语义不能被破坏）──────────────
    {
        std::atomic<int> ran{0};
        {
            ThreadPool p(2);
            for (int i = 0; i < 10; ++i)
                p.submit([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    ++ran;
                });
        }   // 析构：join 会把剩余任务跑完
        check(ran.load() == 10, "析构时已入队的任务全部执行完（实际 " +
                                std::to_string(ran.load()) + "）");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
