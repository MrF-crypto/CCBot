cmake_minimum_required(VERSION 3.20)
project(CCGMonitor VERSION 2.7.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/net/trading_client.cpp
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

    # WIN32 关键字只在 Windows 生效（GUI 子系统，不带控制台窗口），macOS 上自动忽略
    add_executable(CCGMonitor WIN32
        src/main.cpp
        src/core/key_store.cpp
        src/gui/main_window.cpp
        ${CORE_SOURCES}
    )
    target_include_directories(CCGMonitor PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(CCGMonitor PRIVATE ${CORE_LIBS} Qt6::Widgets)

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
if(WIN32)
    target_link_libraries(ccbot_headless PRIVATE crypt32 ws2_32 wsock32)
    target_compile_definitions(ccbot_headless PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    # 不链接 Qt6，拿不到 Qt 自带的 /utf-8 编译选项，源文件里的中文注释要自己声明编码
    target_compile_options(ccbot_headless PRIVATE /utf-8)
endif()

# ── 指标数学逻辑单元测试——纯 C++ 头文件，不依赖 Qt/curl/网络，独立小可执行文件 ──
add_executable(ccg_indicator_tests tests/test_indicators.cpp)
target_include_directories(ccg_indicator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(ccg_indicator_tests PRIVATE /utf-8)
endif()
