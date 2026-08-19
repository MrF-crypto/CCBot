#pragma once

// 版本号的**唯一来源**是 CMakeLists 里的 project(... VERSION x.y.z)，
// 由构建系统作为编译宏注入。
//
// 为什么不直接写在源码里：发版时版本号出现在窗口标题、headless 启动横幅、
// CMake 工程版本三个地方，手改就一定会有漏掉的那次——而"漏掉"要等发布包发到
// 用户手上才看得见。发布流水线的泄漏检查会核对"tag 版本号是否内嵌在可执行文件里"
// （防止把上一版的构建产物打进新版的包），这个宏就是让那道核对有东西可查。
//
// 注：ccbot_headless 此前【完全没有】版本串，Linux 流水线一打 tag 就会卡在那道
// 核对上。是在准备 v3.9.1 时先查出来的，不是发出去才发现的。
#ifndef CCBOT_VERSION
#define CCBOT_VERSION "0.0.0-dev"   // 没走 CMake 的构建（如 IDE 直接开单文件）兜底
#endif

namespace ccbot {

// 带 v 前缀，与 git tag、Release 资产名的写法一致。
// 相邻字符串字面量在编译期就拼好了，二进制里是连续的 "vX.Y.Z"，
// 泄漏检查按字节搜得到
inline constexpr const char* kVersion = "v" CCBOT_VERSION;

}  // namespace ccbot
