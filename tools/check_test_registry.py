#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyKV-CPP 测试登记表一致性检查（docs/testing.md §7）。

双向对账，任何遗漏产生明确失败（非零退出码）：
  E1 仓库 -> 表：所有 add_test(NAME ...) 注册名必须已登记
  E2 仓库 -> 表：tests/ 下所有 *_test.{cpp,cc,cxx,sh,py} 必须被某行 Scope 覆盖
  E3 表 -> 仓库：每行 Scope 路径必须存在
  E4 表 -> 仓库：Runner/Args 中引用的脚本路径必须存在
  E5 词表：Level、Metadata 标签必须在 docs/testing.md §3/§6 词表内
  E6 结构：ctest 测试名唯一；每行至少一个速度标签；表格可解析

用法：
  python tools/check_test_registry.py [--root <仓库路径>] [--show-rows]

仅用 Python 3 标准库，不依赖构建产物，可在编译前运行。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ---- 词表（与 docs/testing.md §3/§6 同步维护） -------------------------------
LEVELS = {"unit", "simulation", "integration", "bench"}
SPEED_TAGS = {"fast", "slow", "nightly"}
FIXED_META_TAGS = {"needs-linux", "serial", "skippable", "manual", "quarantine"}
TIMEOUT_TAG = re.compile(r"^timeout:\d+s$")

TEST_FILE_RE = re.compile(r".*_test\.(cpp|cc|cxx|sh|py)$")
TESTLIB_DIR = "testlib"  # 共享测试基建，不要求单独登记

REGISTRY_BEGIN = "<!-- registry:begin"
REGISTRY_END = "<!-- registry:end"
ADD_TEST_RE = re.compile(r"add_test\s*\(\s*NAME\s+([^\s),]+)", re.IGNORECASE)
BACKTICK_RE = re.compile(r"`([^`]+)`")
SCRIPT_PATH_RE = re.compile(r"[A-Za-z0-9_./\\-]+\.(?:sh|py)")
SEPARATOR_ROW_RE = re.compile(r"^[\s|:\-]+$")

COLUMNS = ("scope", "level", "runner", "args", "meta")


class Finding:
    def __init__(self, code: str, message: str, warn: bool = False):
        self.code, self.message, self.warn = code, message, warn


def norm(path_text: str) -> str:
    """统一路径分隔符，便于 Windows/Linux 双端比对。"""
    return path_text.strip().replace("\\", "/")


def parse_registry(md_path: Path):
    """解析 <!-- registry:begin v1 --> ... <!-- registry:end --> 之间的表格。"""
    text = md_path.read_text(encoding="utf-8-sig")
    begin = text.find(REGISTRY_BEGIN)
    end = text.find(REGISTRY_END)
    if begin == -1 or end == -1 or end < begin:
        return None, [Finding("E0", f"登记表标记区缺失或错序：{md_path} 中未找到 "
                                   f"{REGISTRY_BEGIN}...{REGISTRY_END}")]

    rows, errors, seen_names = [], [], set()
    for lineno, raw in enumerate(text[begin:end].splitlines(), start=1):
        line = raw.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) != 5 or SEPARATOR_ROW_RE.match(line) or cells[0] == "Scope":
            continue  # 表头 / 分隔行 / 空行
        row = dict(zip(COLUMNS, cells))
        row["lineno"] = lineno

        # Runner 首个反引号 token = ctest 测试名（唯一）
        bt = BACKTICK_RE.findall(row["runner"])
        if not bt:
            errors.append(Finding("E6", f"第 {lineno} 行：Runner 列缺少反引号包裹的 ctest 测试名"))
            continue
        row["name"] = bt[0]
        if row["name"] in seen_names:
            errors.append(Finding("E6", f"第 {lineno} 行：ctest 测试名重复：{row['name']}"))
        seen_names.add(row["name"])

        # E5 词表
        if row["level"] not in LEVELS:
            errors.append(Finding("E5", f"第 {lineno} 行：非法 Level：{row['level']}"
                                        f"（允许：{'/'.join(sorted(LEVELS))}）"))
        tags = [t.strip() for t in row["meta"].split(";") if t.strip()]
        if not tags:
            errors.append(Finding("E5", f"第 {lineno} 行：Metadata 列为空"))
        for t in tags:
            if t not in SPEED_TAGS and t not in FIXED_META_TAGS and not TIMEOUT_TAG.match(t):
                errors.append(Finding("E5", f"第 {lineno} 行：未知 Metadata 标签：{t}"
                                            f"（词表见 docs/testing.md §6）"))
        if not (SPEED_TAGS & set(tags)):
            errors.append(Finding("E6", f"第 {lineno} 行：Metadata 缺少速度标签"
                                        f"（{'/'.join(sorted(SPEED_TAGS))} 之一）"))

        # E4 Runner/Args 中引用的脚本必须存在（在 main 中对仓库根校验，这里先收集）
        row["scripts"] = SCRIPT_PATH_RE.findall(row["runner"] + " " + row["args"])
        rows.append(row)
    return rows, errors


def scan_add_tests(root: Path):
    """扫描全部 CMakeLists.txt 中的 add_test(NAME ...)。"""
    found, warns = [], []
    for cmake in sorted(root.rglob("CMakeLists.txt")):
        rel = cmake.relative_to(root).as_posix()
        if rel.startswith("extern/") or "/build/" in f"/{rel}" or rel.startswith("build/"):
            continue
        for lineno, line in enumerate(cmake.read_text(encoding="utf-8",
                                                      errors="replace").splitlines(), 1):
            m = ADD_TEST_RE.search(line)
            if not m:
                continue
            name = m.group(1)
            if "${" in name:  # foreach 等动态注册，静态无法确定展开名
                warns.append(Finding("W1", f"动态 add_test 注册无法静态校验：{rel}:{lineno}"
                                           f"（NAME={name}）；请确认展开后的每个测试都已登记",
                                    warn=True))
            else:
                found.append((name, f"{rel}:{lineno}"))
    return found, warns


def scan_test_files(root: Path):
    """tests/ 下所有 *_test.{cpp,cc,cxx,sh,py}，testlib/ 除外。"""
    tests_dir = root / "tests"
    if not tests_dir.is_dir():
        return []
    files = []
    for f in tests_dir.rglob("*"):
        if not f.is_file() or not TEST_FILE_RE.match(f.name):
            continue
        rel = f.relative_to(root).as_posix()
        if TESTLIB_DIR in rel.split("/"):
            continue
        files.append(rel)
    return sorted(files)


def scope_covers(scope: str, file_rel: str) -> bool:
    s = norm(scope).rstrip("/")
    return file_rel == s or file_rel.startswith(s + "/")


def main() -> int:
    ap = argparse.ArgumentParser(description="TinyKV 测试登记表一致性检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent),
                    help="仓库根目录（默认为脚本上两级）")
    ap.add_argument("--show-rows", action="store_true", help="打印解析到的登记行")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    md_path = root / "docs" / "testing.md"

    findings: list[Finding] = []
    if not md_path.exists():
        findings.append(Finding("E0", f"未找到 {md_path}"))
        rows = []
    else:
        rows, parse_errors = parse_registry(md_path)
        findings.extend(parse_errors)

    registered_names = {r["name"] for r in rows}
    registered_scopes = [r["scope"] for r in rows]

    add_tests, dyn_warns = scan_add_tests(root)
    test_files = scan_test_files(root)

    # E1：仓库 -> 表
    for name, loc in add_tests:
        if name not in registered_names:
            findings.append(Finding("E1", f"未登记测试：{name}（注册于 {loc}）；"
                                          f"请在 docs/testing.md §4 登记"))
    # E2：仓库 -> 表
    for f in test_files:
        if not any(scope_covers(s, f) for s in registered_scopes):
            findings.append(Finding("E2", f"测试文件未登记：{f}；"
                                          f"请在 docs/testing.md §4 以 Scope 覆盖它"))
    # E3/E4：表 -> 仓库
    for r in rows:
        scope = norm(r["scope"])
        if not (root / scope).exists():
            findings.append(Finding("E3", f"第 {r['lineno']} 行：幽灵登记（路径不存在）：{scope}"))
        for s in r.get("scripts", []):
            sp = norm(s)
            if "/" in sp and not (root / sp).exists():
                findings.append(Finding("E4", f"第 {r['lineno']} 行：Runner 脚本不存在：{sp}"))

    if args.show_rows:
        for r in rows:
            print(f"  行{r['lineno']:>3}  {r['name']:<40} [{r['level']}] {r['scope']}")

    errors = [f for f in findings if not f.warn]
    warns = [f for f in findings if f.warn] + dyn_warns

    print(f"== TinyKV 测试登记表一致性检查 ==")
    print(f"仓库根: {root}")
    print(f"登记行: {len(rows)}  add_test 注册: {len(add_tests)}  测试文件: {len(test_files)}")
    for w in warns:
        print(f"WARN [{w.code}] {w.message}")
    if errors:
        print(f"\nFAIL: 登记表与仓库不一致（docs/testing.md §4），共 {len(errors)} 处：")
        for e in errors:
            print(f"  [{e.code}] {e.message}")
        print("\n修复后重跑：python tools/check_test_registry.py")
        return 1
    print("\nPASS: 登记表与仓库一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
