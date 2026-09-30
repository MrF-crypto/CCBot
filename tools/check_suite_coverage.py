#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""每个测试目标都必须被 sanitizer 门禁覆盖 —— 这条靠人记会漏。

存在的理由（2026-09-30 审计发现）：
sanitize.yml 里的套件列表是【硬编码】的。v5.7.0 新增 ccg_rest_feed_tests 时
没人改那个列表，于是它在 ASan 和 TSan 两个门禁里都不在 —— 而它偏偏是唯一
行情来源、还自带一个轮询后台线程。门禁没报错、CI 全绿，缺口静默存在了几个版本。

同一次审计还发现 build/Release 里躺着三个 DCA 时代的陈旧 exe
（ccg_atr_trail_tests / ccg_persist_tests / ccg_risk_tests）：CMakeLists 里早已
没有它们，但"遍历 build/Release/ccg_*tests.exe"的统计脚本照样把它们算进去，
于是对外报的断言数虚高了 135 条。陈旧产物比缺失产物更危险，因为它让人以为
覆盖率比实际更高。

这个脚本把两件事都变成硬门禁：
  ① CMakeLists 里的每个 ccg_*_tests 目标都必须出现在 ASan 列表里
  ② 带后台线程/线程池的套件必须出现在 TSan 列表里
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def cmake_targets() -> set[str]:
    txt = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"add_executable\((ccg_[a-z_]+)", txt))


def sanitize_lists() -> tuple[set[str], set[str]]:
    """返回 (ASan 覆盖集, TSan 覆盖集)。"""
    txt = (ROOT / ".github/workflows/sanitize.yml").read_text(encoding="utf-8")
    # ASan 走 "all" 分支里的 SUITES="..."
    m = re.search(r'SUITES="([^"]+)"', txt, re.S)
    asan = set(re.findall(r"ccg_[a-z_]+", m.group(1))) if m else set()
    # TSan 走 matrix 里 "san: thread" 那一项的 suites:
    m2 = re.search(r"san:\s*thread.*?suites:(.*?)(?:\n\s*\n|\n\S)", txt, re.S)
    tsan = set(re.findall(r"ccg_[a-z_]+", m2.group(1))) if m2 else set()
    return asan, tsan


def needs_tsan() -> set[str]:
    """源码里出现 std::thread / ThreadPool 的套件 —— 它们必须过 TSan。

    判据放在【测试源码】上而不是被测代码上：一个测试只有真的起了并发，
    TSan 才有东西可查。"""
    out = set()
    for f in (ROOT / "tests").glob("test_*.cpp"):
        body = f.read_text(encoding="utf-8", errors="replace")
        if "std::thread" in body or "ThreadPool" in body:
            out.add("ccg_" + f.stem.removeprefix("test_") + "_tests")
    return out


def main() -> int:
    targets = cmake_targets()
    asan, tsan = sanitize_lists()
    fail = []

    if not targets:
        fail.append("没能从 CMakeLists.txt 解析出任何 ccg_*_tests 目标 —— 这个脚本本身坏了")
    if not asan:
        fail.append("没能从 sanitize.yml 解析出 ASan 套件列表 —— 这个脚本本身坏了")

    missing_asan = sorted(targets - asan)
    if missing_asan:
        fail.append("这些测试目标不在 ASan 门禁里: " + ", ".join(missing_asan))

    # TSan 只要求"真的起并发"的那些；名字对不上 CMake 目标的忽略
    want_tsan = sorted((needs_tsan() & targets) - tsan)
    if want_tsan:
        fail.append("这些套件起了线程但不在 TSan 门禁里: " + ", ".join(want_tsan))

    stale = sorted(asan - targets)
    if stale:
        fail.append("ASan 列表里有 CMakeLists 里不存在的目标（改名/删除后没同步）: "
                    + ", ".join(stale))

    if fail:
        print("门禁覆盖检查失败：")
        for f in fail:
            print("  [X] " + f)
        print("\n修法：改 .github/workflows/sanitize.yml 里的套件列表。")
        print("硬编码列表加新套件时不会有人提醒，所以才需要这个脚本。")
        return 1

    print(f"门禁覆盖检查通过：{len(targets)} 个目标全在 ASan 列表里；"
          f"{len(tsan)} 个在 TSan 列表里")
    return 0


if __name__ == "__main__":
    sys.exit(main())
