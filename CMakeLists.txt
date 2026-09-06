cmake_minimum_required(VERSION 3.20)
project(CCGMonitor VERSION 4.0.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 版本号唯一来源：上面那行。窗口标题和 headless 启动横幅都从这里取（src/version.h），
# 发版时只改一处。发布流水线的泄漏检查会核对 tag 版本号确实内嵌在可执行文件里，
# 所以"改了 tag 忘了改源码"会被门禁拦住而不是发到用户手上
set(CCBOT_VERSION_DEF CCBOT_VERSION="${PROJECT_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ── 核心引擎/网络代码：GUI 版和无界面(headless)版共用，不含任何 Qt/Windows 专属代码 ──
set(CORE_SOURCES
    src/core/ccg_engine.cpp
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
    add_executable(CCGMonitor WIN32
        src/main.cpp
        src/core/key_store.cpp
        src/gui/main_window.cpp
        ${CORE_SOURCES}
        ${APP_ICON_SOURCES}
    )
    target_include_directories(CCGMonitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(CCGMonitor PRIVATE ${CORE_LIBS} Qt6::Widgets)
    target_compile_definitions(CCGMonitor PRIVATE ${CCBOT_VERSION_DEF})

    if(WIN32)
        target_link_libraries(CCGMonitor PRIVATE crypt32 ws2_32 wsock32)
        target_compile_definitions(CCGMonitor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

        # 构建后复制 Qt platform plugin（兼容共享 vcpkg_installed 与本地安装两种模式）
        if(DEFINED VCPKG_INSTALLED_DIR AND EXISTS "${VCPKG_INSTALLED_DIR}")
            set(_vcpkg_inst "${VCPKG_INSTALLED_DIR}")
        else()
            set(_vcpkg_inst "${CMAKE_BINARY_DIR}/vcpkg_installed")
        endif()

        add_custom_command(TARGET CCGMonitor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:CCGMonitor>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_vcpkg_inst}/x64-windows/Qt6/plugins/platforms/qwindows.dll"
                "$<TARGET_FILE_DIR:CCGMonitor>/platforms/qwindows.dll"
        )
    elseif(APPLE)
        # 密钥存储走系统钥匙串（key_store.cpp 的 __APPLE__ 分支）
        target_link_libraries(CCGMonitor PRIVATE
            "-framework Security" "-framework CoreFoundation")
        # 本机 vcpkg 构建的 Qt 插件路径编译期已内置，构建机上直接运行即可，
        # 无需像 Windows 那样拷贝 platform plugin
    endif()
endif()

# ── 无界面(headless)版：配置文件驱动，不依赖 Qt，Windows/Linux 都能编译 ──────────
add_executable(ccbot_headless
    src/headless/main.cpp
    src/headless/headless_config.cpp
    src/headless/headless_state.cpp
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

# ── 回测工具（P0 数据层）：不依赖 Qt/网络，Windows/Linux 都能跑 ────────────────
add_executable(ccbot_backtest
    src/backtest/bt_main.cpp
    src/backtest/bt_data.cpp
    src/backtest/bt_replay.cpp
    src/backtest/bt_portfolio.cpp
    src/core/ccg_engine.cpp     # 回测跑引擎本尊，不是仿制品
)
target_include_directories(ccbot_backtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccbot_backtest PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccbot_backtest PRIVATE /utf-8)
endif()

# ── 梯子深度分析：满层能扛多深 / 剩多少保证金没花。不跑行情，纯确定性推演 ────
add_executable(ladder_depth tools/ladder_depth.cpp src/core/ccg_engine.cpp)
target_include_directories(ladder_depth PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ladder_depth PRIVATE Threads::Threads)
target_compile_definitions(ladder_depth PRIVATE ${CCBOT_VERSION_DEF})
if(MSVC)
    target_compile_options(ladder_depth PRIVATE /utf-8)
endif()

# ── 指标数学逻辑单元测试——纯 C++ 头文件，不依赖 Qt/curl/网络，独立小可执行文件 ──
add_executable(ccg_indicator_tests tests/test_indicators.cpp)
target_include_directories(ccg_indicator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(ccg_indicator_tests PRIVATE /utf-8)
endif()

# ── 风控闸门测试（保证金上限 / 交易所侧灾难止损单）：用假客户端单跑引擎 ────────
add_executable(ccg_risk_tests tests/test_risk_gates.cpp src/core/ccg_engine.cpp)
target_include_directories(ccg_risk_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_risk_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_risk_tests PRIVATE /utf-8)
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
                                                            src/core/ccg_engine.cpp)
target_include_directories(ccg_config_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_config_tests PRIVATE simdjson::simdjson Threads::Threads)
if(MSVC)
    target_compile_options(ccg_config_tests PRIVATE /utf-8)
endif()

# ── 状态持久化往返测试：重启后仓位对不对，全看这一套 ──────────────────────────
add_executable(ccg_persist_tests tests/test_persist.cpp
                                 src/headless/headless_state.cpp
                                 src/core/ccg_engine.cpp)
target_include_directories(ccg_persist_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_persist_tests PRIVATE simdjson::simdjson Threads::Threads)
if(MSVC)
    target_compile_options(ccg_persist_tests PRIVATE /utf-8)
endif()

# ── 回测强平模型测试：该爆的要爆，不该爆的一个数字都不能动 ────────────────────
add_executable(ccg_liq_tests tests/test_liquidation.cpp
                             src/backtest/bt_portfolio.cpp
                             src/backtest/bt_data.cpp      # aggregate / interval_ms / BarStream
                             src/core/ccg_engine.cpp)
target_include_directories(ccg_liq_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_liq_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_liq_tests PRIVATE /utf-8)
endif()

# ── 压力测试（并发撕裂 / 极端行情）：实盘前的最后一道自检 ────────────────────
add_executable(ccg_stress_tests tests/test_stress.cpp src/core/ccg_engine.cpp)
target_include_directories(ccg_stress_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ccg_stress_tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(ccg_stress_tests PRIVATE /utf-8)
endif()

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
