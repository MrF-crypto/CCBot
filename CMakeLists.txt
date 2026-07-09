cmake_minimum_required(VERSION 3.20)
project(CCGMonitor VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(Qt6 COMPONENTS Widgets REQUIRED)

set(SOURCES
    src/main.cpp
    src/core/key_store.cpp
    src/core/ccg_engine.cpp
    src/net/trading_client.cpp
    src/net/book_ticker_stream.cpp
    src/net/alert.cpp
    src/gui/main_window.cpp
)

add_executable(CCGMonitor WIN32 ${SOURCES})

target_include_directories(CCGMonitor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(CCGMonitor PRIVATE
    ixwebsocket::ixwebsocket
    simdjson::simdjson
    CURL::libcurl
    Qt6::Widgets
)

if(WIN32)
    target_link_libraries(CCGMonitor PRIVATE bcrypt crypt32 ws2_32 wsock32)
    target_compile_definitions(CCGMonitor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

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

# 指标数学逻辑单元测试——纯 C++ 头文件，不依赖 Qt/curl/网络，独立小可执行文件
add_executable(ccg_indicator_tests tests/test_indicators.cpp)
target_include_directories(ccg_indicator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    # 不链接 Qt6，拿不到 Qt 自带的 /utf-8 编译选项，源文件里的中文注释要自己声明编码
    target_compile_options(ccg_indicator_tests PRIVATE /utf-8)
endif()
