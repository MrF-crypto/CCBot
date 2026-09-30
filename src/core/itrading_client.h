#pragma once
#include <string>

// 引擎依赖的交易接口（引擎只用这4个方法）。
// 实盘由 TradingClient 实现；回测由 SimClient 实现模拟撮合——
// 这样回测跑的是【引擎本尊】而不是仿制品，策略逻辑零复制。
namespace ccbot {

struct OrderOutcome {
    bool        ok = false;
    std::string order_id;
    std::string error;
    double      avg_price    = 0;
    double      executed_qty = 0;
    bool        uncertain    = false;   // 状态不明（超时且查单失败）
};

class ITradingClient {
public:
    virtual ~ITradingClient() = default;

    virtual OrderOutcome place_market_order(const std::string& symbol, const std::string& side,
                                            double qty, bool reduce_only) = 0;
    virtual double round_qty(const std::string& symbol, double qty) = 0;
    virtual bool   set_leverage(const std::string& symbol, int lev)  = 0;
    virtual bool   is_dual_mode() const = 0;

    // ── 交易所侧硬止损（进程外兜底，"保命单"）────────────────────────────────
    // 本地的移动止损活在进程里，程序崩溃、断电、误关窗口之后仓位就完全裸奔。
    // 这两个方法在交易所挂一张 STOP_MARKET + closePosition 的单子，
    // 开仓时挂一次、此后不动，只防瀑布，进程死了它还在。
    struct StopPlacement {
        std::string order_id;          // 非空 = 挂上了
        std::string error;             // 失败原因，给人看
        // 重试有没有意义。区分它的理由：参数类错误（精度不对、触发价在错误的
        // 一侧）重试十次也是同样的结果，而引擎的重试阶梯要跑二十多秒——
        // 那二十多秒里仓位没有进程外保护。分流之后这类错误直接走到兜底处置。
        //
        // ⚠ 默认 true。判错的两个方向代价不对称：
        //   误判成可重试  → 白等二十多秒，然后照样走兜底（只是慢）
        //   误判成不可重试 → 一个本来能恢复的抖动直接把仓位平掉（丢单）
        //   所以只对【确定无望】的错误码标 false
        bool retryable = true;
        // 挂上了，但用的是【条件单端点】而不是当前账户模式对应的那个端点。
        //
        // 这是一条要让人看见的诊断：能走到这儿说明普通端点回了 -4120
        // （"这个端点不收 STOP_MARKET，请用 Algo Order API"），而换端点之后成功了
        // ⇒ 账户模式设错了。account_mode 是手工选的不是探测的，设错时行情、
        // 余额、市价单全部照常工作，只有条件单会露馅——不报出来就永远查不到
        bool via_cond_fallback = false;
        // 一次性的说明，非空时上层【原样打一条日志】。
        // 用来报告"换了写法/换了端点"这类少见但必须让人知道的事——每次挂单都打
        // 就是噪音，所以由下层只在真的变化时填，读走即清
        std::string note;
        bool ok() const { return !order_id.empty(); }
    };

    // 默认实现返回"不支持且不必重试"：回测的 SimClient 用不到它（回测里进程
    // 不会崩）。标 retryable=false 是有意的——否则模拟环境会白跑一遍重试阶梯
    virtual StopPlacement place_disaster_stop(const std::string& /*symbol*/,
                                              double /*stop_price*/,
                                              const std::string& /*entry_side*/) {
        // ⚠ 按字段赋值，不用花括号聚合初始化。
        //   聚合初始化只写前 N 个字段时，GCC/Clang 的
        //   -Wmissing-field-initializers + -Werror 会直接拒编（MSVC 放过），
        //   于是每次给 StopPlacement 加字段就要去修所有构造点。
        //   这个坑在本会话已经踩过一次（加 note 字段时四处同时编不过）
        StopPlacement p;
        p.error     = "该客户端不支持交易所侧硬止损";
        p.retryable = false;
        return p;
    }
    virtual bool cancel_disaster_stop(const std::string& /*symbol*/,
                                      const std::string& /*order_id*/) { return true; }
};

} // namespace ccbot
