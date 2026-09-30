cmake_minimum_required(VERSION 3.20)
project(TradingBot VERSION 5.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 版本号唯一来源：上面那行。窗口标题和 headless 启动横幅都从这里取（src/version.h），
# 发版时只改一处。发布流水线的泄漏检查会核对 tag 版本号确实内嵌在可执行文件里，
# 所以"改了 tag 忘了改源码"会被门禁拦住而不是发到用户手上
set(CCBOT_VERSION_DEF CCBOT_VERSION="${PROJECT_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── 警告门禁 ──────────────────────────────────────────────────────────────────
# 在此之前本项目【一个警告开关都没开】——三个编译器只兑现了"能编过"这一层保证，
# 而默认警告等级恰好放过未初始化读取、符号比较、窄化转换这一类会亏钱的东西。
# 对一个跑真钱、几十处 mutex/atomic、跨三平台的 C++20 项目，这是最大的单点缺口。
#
# CCBOT_WERROR=ON 时警告升级为错误（CI 用）。本地开发默认只警告不拦，
# 否则改一行代码撞上一个陈旧警告就编不过，反而会让人把整个开关关掉。
#
# ⚠ 只作用于【我们自己的目标】，不用 add_compile_options 全局加——那会把
#   vcpkg 依赖和 Qt 生成的 moc 代码一起纳入，它们的警告我们既管不了也不该管。
option(CCBOT_WERROR "把编译警告当作错误（CI 门禁用）" OFF)

# ── Sanitizer ─────────────────────────────────────────────────────────────────
# CCBOT_SANITIZE=address / thread / undefined / address+undefined（分号或加号分隔）
#
# 为什么这是本项目最该补的一项验证：trend_engine.cpp 有 27 处 mutex/atomic、
# book_ticker_stream.cpp 还多一个看门狗线程，而并发缺陷是所有缺陷里唯一会
# "看起来一切正常，直到某天对不上账"的一类——单元测试全绿也证明不了它不存在。
# TSan 能证明。
#
# address 与 thread 互斥，必须分两次构建（编译器会直接拒绝同时给）。
# undefined 可以搭着 address 一起。
#
# ⚠ vcpkg 的依赖不带 instrumentation，所以 sanitizer 看不进 simdjson/curl 内部。
#   这不影响目的：要查的并发原语全在我们自己的引擎里，而那些测试用的是 FakeClient，
#   根本不碰网络库。
set(CCBOT_SANITIZE "" CACHE STRING "sanitizer：address / thread / undefined，可加号连接")

function(ccbot_sanitize tgt)
    if(CCBOT_SANITIZE STREQUAL "" OR MSVC)
        return()
    endif()
    string(REPLACE "+" "," _san "${CCBOT_SANITIZE}")
    target_compile_options(${tgt} PRIVATE
        -fsanitize=${_san}
        -fno-omit-frame-pointer   # 没有它栈回溯只剩地址，报告等于没有
        -g)
    target_link_options(${tgt} PRIVATE -fsanitize=${_san})
endfunction()

function(ccbot_harden tgt)
    ccbot_sanitize(${tgt})
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive- /utf-8
            # C4100 未引用的形参：接口实现里大量方法只用部分参数（ITradingClient
            # 的默认空实现就是），逐个加 (void) 是噪音换噪音
            /wd4100)
        if(CCBOT_WERROR)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra
            -Wshadow              # 局部变量遮蔽外层——本仓库改过好几次同名变量
            -Wnon-virtual-dtor    # ITradingClient 这类多态基类
            -Wcast-align -Wunused -Woverloaded-virtual
            -Wdouble-promotion    # float→double 隐式提升：价格计算里混用会丢精度
            -Wformat=2
            # ⚠ -Wformat=2 隐含 -Wformat-nonliteral，而它会在【simdjson 的头文件里】
            #   触发（simdjson.h 内部用非字面量格式串）。
            #   这暴露了"只给自有目标加 flag"并不足够：第三方【头文件】被包含进我们的
            #   翻译单元时，警告照样算在我们头上。理想解是把依赖的 include 目录标成
            #   SYSTEM，但 find_package 导入目标的 include 属性不总是好改。
            #   保留 -Wformat=2 的其余部分（-Wformat-security 才是真正有价值的那条：
            #   它能抓住 printf(用户输入) 这类），只关掉会在别人头文件里响的这一条。
            -Wno-format-nonliteral
            -Wno-unused-parameter)
        if(CCBOT_WERROR)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()
    endif()
endfunction()

find_package(ixwebsocket CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ── 核心引擎/网络代码：GUI 版和无界面(headless)版共用，不含任何 Qt/Windows 专属代码 ──
set(CORE_SOURCES
    src/core/trend_engine.cpp
    src/core/funding_ledger.cpp
    src/net/trading_client.cpp
    src/net/rate_gate.cpp
    src/net/book_ticker_stream.cpp
    src/net/alert.cpp
)
set(CORE_LIBS
    ixwebsocket::ixwebsocket
    simdjson::simdjson
    CURL::libcurl
    MbedTLS::mbedcrypto
    Threads::Threads
)

# ── GUI 版（Qt Widgets，Windows / macOS）──────────────────────────────────────
if(WIN32 OR APPLE)
    find_package(Qt6 COMPONENTS Widgets REQUIRED)

    # 图标资源。app.qrc（窗口/任务栏图标，AUTORCC 编译）两个平台都要；
    # app.rc（exe 文件本身的图标）是 Windows 资源脚本，只有 MSVC 认
    set(APP_ICON_SOURCES assets/app.qrc)
    if(WIN32)
        list(APPEND APP_ICON_SOURCES assets/app.rc)
    endif()

    # WIN32 关键字只在 Windows 生效（GUI 子系统，不带控制台窗口），macOS 上自动忽略
    add_executable(TradingBot WIN32
        src/main.cpp
        src/core/key_store.cpp
        src/gui/main_window.cpp
        src/gui/trend_panel.cpp
        ${CORE_SOURCES}
        ${APP_ICON_SOURCES}
    )
    target_include_directories(TradingBot PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(TradingBot PRIVATE ${CORE_LIBS} Qt6::Widgets)
    target_compile_definitions(TradingBot PRIVATE ${CCBOT_VERSION_DEF})

    if(WIN32)
        target_link_libraries(TradingBot PRIVATE crypt32 ws2_32 wsock32)
        target_compile_definitions(TradingBot PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

        # 构建后复制 Qt platform plugin（兼容共享 vcpkg_installed 与本地安装两种模式）
        if(DEFINED VCPKG_INSTALLED_DIR AND EXISTS "${VCPKG_INSTALLED_DIR}")
            set(_vcpkg_inst "${VCPKG_INSTALLED_DIR}")
        else()
            set(_vcpkg_inst "${CMAKE_BINARY_DIR}/vcpkg_installed")
        endif()

        add_custom_command(TARGET TradingBot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:TradingBot>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_vcpkg_inst}/x64-windows/Qt6/plugins/platforms/qwindows.dll"
                "$<TARGET_FILE_DIR:TradingBot>/platforms/qwindows.dll"
        )
    elseif(APPLE)
        # 密钥存储走系统钥匙串（key_store.cpp 的 __APPLE__ 分支）
        target_link_libraries(TradingBot PRIVATE
            "-framework Security" "-framework CoreFoundation")
        # 本机 vcpkg 构建的 Qt 插件路径编译期已内置，构建机上直接运行即可，
        # 无需像 Windows 那样拷贝 platform plugin
    endif()
endif()

# ── 无界面(headless)版：配置文件驱动，不依赖 Qt，Windows/Linux 都能编译 ──────────
add_executable(ccbot_headless
    src/headless/main.cpp
    src/headless/headless_config.cpp
    src/headless/trend_state.cpp
    ${CORE_SOURCES}
)
target_include_directories(ccbot_headless PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccbot_headless PRIVATE ${CORE_LIBS})
target_compile_definitions(ccbot_headless PRIVATE ${CCBOT_VERSION_DEF})
if(WIN32)
    target_link_libraries(ccbot_headless PRIVATE crypt32 ws2_32 wsock32)
    target_compile_definitions(ccbot_headless PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    # 不链接 Qt6，拿不到 Qt 自带的 /utf-8 编译选项，源文件里的中文注释要自己声明编码
    target_compile_options(ccbot_headless PRIVATE /utf-8)
endif()

# ── 回测已移出本仓库（v4.0.13）────────────────────────────────────────────────
# src/backtest/ 整个子系统（3418 行，占当时代码量 18%）连同 ccbot_backtest 目标
# 一并删除，回测由独立项目承担。
#
# 删它的理由不只是省代码量：那套东西是【第二份策略逻辑】。它要重新实现指标
# 推进、K线聚合、撮合、资金费，任何一处与实盘的偏差都会让回测结论悄悄失真，
# 而偏差在哪儿只有踩到才知道。v4.0.7 的涨幅拦截就已经出现"回测侧未实现，
# 强制归零"这种补丁——两套逻辑之间的缝开始显形。本仓库现在只留一条真相。
#
# 保留的是 EngineHost 的时钟/执行器注入（engine_host.h）：那不是为回测存在的，
# 单元测试的虚拟时钟与确定性复现全靠它。
#
# 历史结论仍记录在 docs/BACKTEST.md 与各处代码注释里，只是产生它们的工具搬走了。

# ── 网格DCA 已整体移除（本版）──────────────────────────────────────────────────
# CcgEngine（1809 行）、decision.h（DCA 入场拦截）、headless_state（DCA 落盘）、
# tools/ladder_depth（梯子深度推演，纯 DCA 工具）连同 ccg_risk_tests /
# ccg_persist_tests / ccg_stress_tests 一并删除。
#
# 不是"暂时停用"而是删干净：本仓库现在只做一件事——为趋势类策略提供稳定的
# 实时行情。两套并列策略时期最大的成本不是代码量，是每改一处共享代码都要同时
# 保两套语义不变，而 DCA 的补仓/止盈语义建立在"每 3 秒一次、价格用标记价"
# 这个节拍上，行情层要改成事件驱动就必须先把这份约束拿掉。
#
# DCA 的实测结论（哪些参数有效、哪些被证伪）保留在 docs/NEGATIVE_RESULTS.md，
# 代码删了但花钱买来的结论不删。
#
# ccg_stress_tests 删除后，并发压力覆盖由 ccg_trend_stress_tests 承担——
# 那是 TSan 矩阵里的目标之一，不能随 DCA 一起消失（见下）。

# ── 指标数学逻辑单元测试——纯 C++ 头文件，不依赖 Qt/curl/网络，独立小可执行文件 ──
add_executable(ccg_indicator_tests tests/test_indicators.cpp)
target_include_directories(ccg_indicator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(ccg_indicator_tests PRIVATE /utf-8)
endif()

# ── SAR 趋势跟随测试（决策纯逻辑 + 引擎订单路径）───────────────────────────────
add_executable(ccg_trend_tests tests/test_trend.cpp)
target_include_directories(ccg_trend_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_trend_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_trend_tests PRIVATE /utf-8)
endif()

add_executable(ccg_trend_engine_tests tests/test_trend_engine.cpp src/core/trend_engine.cpp)
target_include_directories(ccg_trend_engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_trend_engine_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_trend_engine_tests PRIVATE /utf-8)
endif()

# ── SAR 持久化与对账测试：守"引擎以为空仓而交易所有仓"这一类裸敞口 ───────────
add_executable(ccg_trend_state_tests tests/test_trend_state.cpp
                                   src/headless/trend_state.cpp
                                   src/core/trend_engine.cpp)
target_include_directories(ccg_trend_state_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_trend_state_tests PRIVATE simdjson::simdjson Threads::Threads)
if(MSVC)
    target_compile_options(ccg_trend_state_tests PRIVATE /utf-8)
endif()

# ── SAR 并发压力测试：实盘前的最后一道自检 ────────────────────────────────────
# 接替随 DCA 一起删掉的 ccg_stress_tests。它的价值不在"算得对不对"（那是
# ccg_trend_engine_tests 的事），而在【并发下的状态撕裂】——多线程同时 tick /
# 喂信号 / 读快照 / 对账，交易所返回随机化。这类缺陷在单线程测试里永远不出现，
# 而且它是 TSan 矩阵的目标之一，不能随 DCA 一起消失
add_executable(ccg_trend_stress_tests tests/test_trend_stress.cpp src/core/trend_engine.cpp)
target_include_directories(ccg_trend_stress_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_trend_stress_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_trend_stress_tests PRIVATE /utf-8)
endif()

# ── 资金费账本测试（去重/口径分离/落盘往返/回本价换算）──────────────────────
add_executable(ccg_funding_tests tests/test_funding.cpp src/core/funding_ledger.cpp
                                 src/net/trading_client.cpp
                                 src/net/rate_gate.cpp)
target_include_directories(ccg_funding_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_funding_tests PRIVATE ${CORE_LIBS})
if(WIN32)
    target_link_libraries(ccg_funding_tests PRIVATE crypt32 ws2_32 wsock32)
    target_compile_definitions(ccg_funding_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ccg_funding_tests PRIVATE /utf-8)
endif()

# ── 订单路径测试（超时恢复/部分成交/零成交/-1111重试）：用假HTTP响应复现故障 ──
add_executable(ccg_order_tests tests/test_order_path.cpp
                               src/net/trading_client.cpp src/net/rate_gate.cpp)
target_include_directories(ccg_order_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_order_tests PRIVATE ${CORE_LIBS})
if(WIN32)
    target_link_libraries(ccg_order_tests PRIVATE crypt32 ws2_32 wsock32)
    target_compile_definitions(ccg_order_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ccg_order_tests PRIVATE /utf-8)
endif()

# ── 线程池关停语义测试：wait_idle 必须等【在途任务】而不只是队列空 ───────────
add_executable(ccg_pool_tests tests/test_thread_pool.cpp)
target_include_directories(ccg_pool_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_pool_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_pool_tests PRIVATE /utf-8)
endif()

# ── 限流闸门测试（时间相关逻辑，且坐在每一个 HTTP 出口上）────────────────────
add_executable(ccg_rate_tests tests/test_rate_gate.cpp src/net/rate_gate.cpp)
target_include_directories(ccg_rate_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_rate_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_rate_tests PRIVATE /utf-8)
endif()

# ── 对时测试：往返过长的测量必须丢弃，诊断信息必须如实 ────────────────────────
add_executable(ccg_timesync_tests tests/test_time_sync.cpp
                                  src/net/trading_client.cpp src/net/rate_gate.cpp)
target_include_directories(ccg_timesync_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_timesync_tests PRIVATE ${CORE_LIBS})
if(WIN32)
    target_link_libraries(ccg_timesync_tests PRIVATE crypt32 ws2_32 wsock32)
    target_compile_definitions(ccg_timesync_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ccg_timesync_tests PRIVATE /utf-8)
endif()

# ── 价格源测试：报文解析 + 陈旧保护（WS 半开时的冻结价是最凶险的故障）──────────
add_executable(ccg_price_tests tests/test_price_feed.cpp src/net/book_ticker_stream.cpp)
target_include_directories(ccg_price_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_price_tests PRIVATE ixwebsocket::ixwebsocket simdjson::simdjson
                                              Threads::Threads)
if(WIN32)
    # bcrypt：ixwebsocket 的 TLS 后端（mbedtls）要 BCryptGenRandom。
    # 主程序通过别的目标间接带进来了，这个独立小目标必须自己声明
    target_link_libraries(ccg_price_tests PRIVATE crypt32 bcrypt ws2_32 wsock32)
    target_compile_definitions(ccg_price_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(ccg_price_tests PRIVATE /utf-8)
endif()

# ── headless 配置解析测试：漏填的键必须回落到【引擎】默认值，不能各写各的 ──────
add_executable(ccg_config_tests tests/test_config.cpp
                                src/headless/headless_config.cpp
                                src/core/trend_engine.cpp)
target_include_directories(ccg_config_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_config_tests PRIVATE simdjson::simdjson Threads::Threads)
if(MSVC)
    target_compile_options(ccg_config_tests PRIVATE /utf-8)
endif()

# ccg_persist_tests 随网格DCA 一并移除（本版）：它测的是 headless_state 的
# DCA 落盘往返。SAR 侧的同类覆盖在 ccg_trend_state_tests 里，一条不少。
#
# ccg_liq_tests 随回测子系统一并移除（v4.0.13）。它测的全部是【回测模拟器
# 内部】的强平会计（PortfolioResult / MultiSimClient / run_portfolio），
# 实盘引擎侧不损失任何覆盖——实盘的强平价来自交易所，不由本地逻辑推算。

# ── 凭证存储格式测试（含加账户类型字段后的向后兼容）──────────────────────────
add_executable(ccg_key_store_tests tests/test_key_store.cpp src/core/key_store.cpp)
target_include_directories(ccg_key_store_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(ccg_key_store_tests PRIVATE /utf-8)
    target_link_libraries(ccg_key_store_tests PRIVATE crypt32)
elseif(APPLE)
    target_link_libraries(ccg_key_store_tests PRIVATE
        "-framework Security" "-framework CoreFoundation")
endif()

# ── 给所有【我们自己的】目标加上警告门禁 ──────────────────────────────────────
# 放在文件末尾统一施加，而不是在每个 add_executable 后面各写一遍：
# 加目标时忘记跟一行是必然会发生的，而"忘了"的表现是那个目标静默失去警告保护。
# 列表里的目标必须存在，否则 ccbot_harden 会报错——这正是想要的：
# 删了目标忘了从这里摘掉，构建就会响
set(CCBOT_OWN_TARGETS
    ccbot_headless
    ccg_indicator_tests ccg_trend_tests ccg_trend_engine_tests ccg_trend_state_tests
    ccg_trend_stress_tests ccg_funding_tests ccg_order_tests ccg_pool_tests
    ccg_rate_tests ccg_timesync_tests ccg_price_tests ccg_config_tests
    ccg_key_store_tests
)
if(WIN32 OR APPLE)
    list(APPEND CCBOT_OWN_TARGETS TradingBot)
endif()
foreach(t ${CCBOT_OWN_TARGETS})
    ccbot_harden(${t})
endforeach()
