// 与交易所对时的测试。
//
// 起因是实盘的 -1021：那台 macOS 的本机时钟慢 2.17 秒、网络往返只有 270ms，
// 按 recvWindow 的预算算根本不该失败，实测却有 33% 的时间在报错。查了半天发现
// 对时函数是个彻底的黑盒——成功没成功、往返多久、算出多少、有没有被限流压住，
// 全都不对外说，只能靠猜。
//
// 这套测试钉住两件事：
//   ① 往返过长的测量必须被【丢弃】而不是采纳
//   ② 诊断信息必须如实反映发生了什么（那是排查的唯一依据）
//
// 为什么①是真缺陷而不是洁癖：中点估算 (t0+t1)/2 假设去回程等时。网络一慢
// 往往就不对称，一次 8 秒往返最坏能算出 4 秒的偏移误差——而整个预算才 4 秒。
// 更糟的是方向：回程慢会让偏移偏负、时间戳更旧，于是更容易 -1021，
// 自愈越修越坏，形成自我强化。
#include "net/trading_client.h"

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

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static TradingClient make_client() {
    TradingClient::Config c;
    c.api_key = "k"; c.api_secret = "s"; c.testnet = true;
    return TradingClient(c);
}

int main() {
    // ── ① 正常往返：采纳，并算出正确的偏移 ──────────────────────────────────
    // 造一个"本机比服务器慢 2170ms"的场景（正是实盘那台 iMac 的真实数值）
    {
        TradingClient cli = make_client();
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 2170) + "}";
            return true;
        });
        auto r = cli.sync_server_time();
        check(r.accepted, "正常往返：测量被采纳");
        // 允许几十毫秒的执行抖动
        check(r.offset_ms > 2100 && r.offset_ms < 2240,
              "  偏移约 +2170ms（本机慢 2.17 秒，与实盘那台一致）");
        check(r.rtt_ms >= 0 && r.rtt_ms < 500, "  往返记录合理");
        check(std::string(r.skip_reason).empty(), "  无丢弃原因");
    }

    // ── ② 往返过长：必须丢弃，且保留原偏移 ──────────────────────────────────
    // 这是本次改动的核心。测试钩子里 sleep 制造一个慢往返
    {
        TradingClient cli = make_client();
        // 先用一次正常同步把偏移建立起来
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 1000) + "}";
            return true;
        });
        auto good = cli.sync_server_time();
        check(good.accepted, "先建立一个好的偏移");
        const int64_t established = good.offset_ms;

        // 再来一次"慢往返 + 偏移差很多"的测量，必须被拒
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2300));  // > 2000ms 阈值
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 9999) + "}";
            return true;
        });
        auto slow = cli.sync_server_time();
        check(!slow.accepted, "往返 2.3 秒的测量被丢弃");
        check(slow.rtt_ms >= 2000, "  往返时长如实记录（" + std::to_string(slow.rtt_ms) + "ms）");
        check(slow.offset_ms == established,
              "  偏移保持原值，没有被那次坏测量污染");
        check(std::string(slow.skip_reason).find("往返") != std::string::npos,
              "  丢弃原因说明是往返过长");
        check(slow.prev_ms == established, "  prev_ms 记录了丢弃前的偏移");
    }

    // ── ③ 各类失败响应：一律不得改动偏移 ────────────────────────────────────
    {
        TradingClient cli = make_client();
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 3000) + "}";
            return true;
        });
        const int64_t base = cli.sync_server_time().offset_ms;

        struct Case { const char* body; const char* tag; };
        const Case cases[] = {
            { "",                       "空响应" },
            { "这不是json",              "非JSON" },
            { "{\"code\":-1003}",       "无 serverTime 字段" },
        };
        for (const auto& c : cases) {
            std::string b = c.body;
            cli.set_test_hook([b](const std::string&, const std::string& path,
                                  const std::string&, TradingClient::FakeReply& out) {
                if (path.find("/fapi/v1/time") == std::string::npos) return false;
                out.code = 200; out.body = b;
                return true;
            });
            auto r = cli.sync_server_time();
            check(!r.accepted && r.offset_ms == base,
                  std::string(c.tag) + " → 不采纳且偏移不变");
        }
    }

    // ── ④ 诊断输出必须包含排查需要的信息 ────────────────────────────────────
    // 这条不是形式主义：-1021 排查了整整一轮才发现"看不见对时发生了什么"是
    // 最大的障碍。日志缺一项就得再等一轮实盘复现
    {
        TradingClient cli = make_client();
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 5000) + "}";
            return true;
        });
        cli.sync_server_time();                       // 建立 +5000 的偏移
        cli.set_test_hook([](const std::string&, const std::string& path,
                             const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 100) + "}";
            return true;
        });
        auto r = cli.sync_server_time();              // 偏移骤降到 +100 = 跳变 -4900
        const std::string s = r.to_log();
        check(s.find("往返") != std::string::npos, "日志含往返时长");
        check(s.find("偏移") != std::string::npos, "日志含偏移量");
        check(s.find("跳变") != std::string::npos,
              "偏移大幅跳变时日志明确标出（时钟被系统步进的直接证据）");
    }

    // ── ⑤ 时间戳必须在限流闸门【放行之后】才算 ──────────────────────────────
    // 这是 v4.0 的核心修复，也是那台 macOS 实盘 33% 时间在报 -1021 的真正原因。
    //
    // 顺序反了（先签名 → 再等闸门 → 才发送）时，闸门一旦阻塞，请求就带着一个
    // 陈旧的时间戳抵达币安。实测闸门最长压过 44 秒，而 recvWindow 只有 5 秒。
    //
    // 怎么测：让测试钩子在被调用时读取 params 里的时间戳，与"钩子被调用那一刻"
    // 的真实时间比对。钩子是在闸门之后才触发的，所以两者的差值就等于
    // 「签名时刻 → 发送时刻」的间隔。修复前这个差值等于闸门的阻塞时长，
    // 修复后它恒等于 0（外加 ts_ms 刻意回拨的 1 秒）。
    {
        TradingClient cli = make_client();
        int64_t ts_in_params = 0, hook_called_at = 0;
        cli.set_test_hook([&](const std::string&, const std::string&,
                              const std::string& params, TradingClient::FakeReply& out) {
            auto pos = params.find("timestamp=");
            if (pos != std::string::npos)
                ts_in_params = std::atoll(params.c_str() + pos + 10);
            hook_called_at = now_ms();
            out.code = 200; out.body = "{}";
            return true;
        });

        // 走一个签名请求（fetch_account 内部是 http_get + 非空 params）
        cli.fetch_account();

        check(ts_in_params > 0, "签名请求确实带了 timestamp");
        // ts_ms() 会刻意回拨 1 秒，所以期望差值约 1000ms；给 500ms 余量容纳执行抖动
        const int64_t lag = hook_called_at - ts_in_params;
        check(lag >= 900 && lag < 1500,
              "时间戳与发送时刻的间隔约等于 1 秒的刻意回拨（实际 " +
              std::to_string(lag) + "ms）—— 说明它是在闸门放行【之后】才算的");
    }

    // ── ⑥ 择优采样：慢样本必须被后续的快样本取代 ────────────────────────────
    // 复现实盘抓到的形态：冷连接那一次往返 747ms、偏移 293ms，紧接着热连接
    // 往返 86ms、偏移 −14ms —— 差的 307ms 正好是握手时长的一半。
    // 单次采样时，那个 293ms 会被当成真实时差写进去，一直用到下次对时。
    {
        TradingClient cli = make_client();
        int calls = 0;
        cli.set_test_hook([&calls](const std::string&, const std::string& path,
                                   const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            ++calls;
            out.code = 200;
            if (calls == 1) {
                // 第一枪：慢，但仍在 2000ms 阈值内 —— 所以【不会】被原有的
                // 丢弃逻辑挡住，只能靠择优采样把它比下去
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                out.body = "{\"serverTime\":" + std::to_string(now_ms() + 5000) + "}";
            } else {
                out.body = "{\"serverTime\":" + std::to_string(now_ms() + 100) + "}";
            }
            return true;
        });
        auto r = cli.sync_server_time();
        check(r.accepted, "择优采样：仍然采纳");
        check(calls >= 2, "  第一枪慢(400ms)会补枪（实际请求 " + std::to_string(calls) + " 次）");
        check(r.rtt_ms < 150, "  记录的是最快那次的往返（" + std::to_string(r.rtt_ms) + "ms）");
        check(r.offset_ms > 0 && r.offset_ms < 500,
              "  【关键】采用快样本的 ~100ms，没被慢样本的 +5000 污染（实际 " +
              std::to_string(r.offset_ms) + "）");
    }

    // ── ⑦ 常态零开销：第一枪就够快时不得再发请求 ────────────────────────────
    // 择优采样若在正常情况下也多打两枪，等于把权重和延迟凭空翻三倍。
    // 实盘热连接往返中位 84ms、P95 101ms，都在 150ms 的收工阈值之内
    {
        TradingClient cli = make_client();
        int calls = 0;
        cli.set_test_hook([&calls](const std::string&, const std::string& path,
                                   const std::string&, TradingClient::FakeReply& out) {
            if (path.find("/fapi/v1/time") == std::string::npos) return false;
            ++calls;
            out.code = 200;
            out.body = "{\"serverTime\":" + std::to_string(now_ms() + 200) + "}";
            return true;
        });
        auto r = cli.sync_server_time();
        check(r.accepted, "热连接常态：采纳");
        check(calls == 1, "  【关键】只发一次请求，择优采样不给常态增加任何开销（实际 " +
              std::to_string(calls) + " 次）");
    }

    std::printf(g_fail ? "\n%d 项失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
