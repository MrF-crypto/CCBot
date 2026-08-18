#!/usr/bin/env python3
"""发布包泄漏检查 —— 硬门禁。

用法:  python tools/leak_check.py <打包目录> [--expect-version vX.Y.Z] [--exe 可执行文件名]

存在的理由：这个仓库的 data/ 目录里有 API 凭证（DPAPI 密文）、持仓状态、交易记录。
打包时若把它们一起塞进 zip，凭证就会进一个**公开的 GitHub Release**。

在有这个脚本之前，拦住这件事的东西只有"我记得做"。现在它是 CI 里的一步，
不通过就发不出去——把最危险的一步从人工自觉变成机器强制。

设计取向：**白名单**而不是黑名单。
黑名单（"别带 config.json"）只能挡住想得到的东西；白名单（"只允许 exe 和 dll"）
连没想到的都挡住。发布包的内容本来就应该是可穷举的。
"""
import argparse
import re
import sys
from pathlib import Path

# Windows CI 的控制台默认不是 UTF-8，直接 print 中文会抛 UnicodeEncodeError
# 把整个门禁弄挂（实测过）。errors='replace' 保证再冷门的环境也不会因为编码问题
# 让检查本身失败——门禁只该因为"真的有问题"而失败
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# 允许出现在发布包里的文件后缀（Windows / macOS / Linux 三平台合集）。
# 无后缀文件单独处理：macOS/Linux 的可执行文件没有后缀
ALLOWED_SUFFIX = {".dll", ".exe", ".dylib", ".so"}

# 即便后缀合法也绝不允许的文件名片段（双保险：万一有人把凭证命名成 .dll）
FORBIDDEN_PAT = re.compile(
    r"(config|creds|credential|secret|api[_-]?key|token|ccg_bots|ccg_trades|"
    r"ccg_funding|ccg_settings|ccbot_state|\.alive$|\.lock$)",
    re.IGNORECASE,
)

# 允许的无后缀可执行文件名（macOS/Linux 包）
ALLOWED_NOEXT = {"CCGMonitor", "ccbot_headless"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg_dir")
    ap.add_argument("--expect-version", default="")
    ap.add_argument("--exe", default="")
    args = ap.parse_args()

    root = Path(args.pkg_dir)
    if not root.is_dir():
        print(f"FAIL  打包目录不存在: {root}")
        return 1

    files = sorted(p for p in root.rglob("*") if p.is_file())
    if not files:
        print("FAIL  打包目录是空的")
        return 1

    problems = []
    for f in files:
        rel = f.relative_to(root).as_posix()
        suffix = f.suffix.lower()

        if FORBIDDEN_PAT.search(f.name):
            problems.append(f"禁止的文件名: {rel}")
            continue
        if suffix in ALLOWED_SUFFIX:
            continue
        if suffix == "" and f.name in ALLOWED_NOEXT:
            continue
        problems.append(f"非白名单文件: {rel}（只允许 exe/dll/dylib/so 与已知可执行文件）")

    print(f"扫描 {len(files)} 个文件")
    for p in problems:
        print(f"  FAIL  {p}")

    # 版本核对：防止把上一版的 exe 打进新版的包（手工发布时真实存在的风险）
    if args.expect_version and args.exe:
        exe = root / args.exe
        if not exe.is_file():
            problems.append(f"找不到可执行文件: {args.exe}")
            print(f"  FAIL  找不到可执行文件: {args.exe}")
        else:
            blob = exe.read_bytes()
            want = args.expect_version.encode()
            if want not in blob:
                problems.append(f"{args.exe} 里找不到版本串 {args.expect_version}")
                print(f"  FAIL  {args.exe} 里找不到版本串 {args.expect_version}"
                      "（可能打包了旧的构建产物）")
            else:
                print(f"  OK    {args.exe} 内嵌版本号含 {args.expect_version}")

    if problems:
        print(f"\n泄漏检查未通过：{len(problems)} 项问题")
        return 1
    print("\n泄漏检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
