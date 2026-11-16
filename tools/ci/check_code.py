#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""code 阶段检查器：构建 + 快速测试 + 复杂度上限 + 复制粘贴检测。

对应 SPEC.md "堆细节造多余抽象" 与 "代码能跑但读不懂" 两行。

武装条件（rules.toml [code].arm_globs）：出现 C++ 源文件才启用；
此前整阶段 SKIP(NOT_ARMED)——从第 0 天 CI 可跑，代码落地后自动收紧（首次引入即基线）。

  * build    有 CMakeLists.txt 时：cmake 配置+构建（-Wall -Wextra -Werror，警告即失败）
             + ctest -L <fast>；已武装但无构建入口 → FAIL（判定只认实测，不虚绿）
  * static   函数体行数 / 圈复杂度近似值超上限 → FAIL
  * dup_code 规范化后 ≥ window 行的连续代码块出现 > max_copies 处 → FAIL

静态分析为 stdlib 实现的保守近似：宁可漏报不误报；精确 lint 交给编译警告与
后续接入的 clang-tidy（rules.toml 收紧即可，无需改检查器）。
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402

CPP_EXT = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh"}
CXX_SKIP_DIRS = {"build", ".git", ".idea", ".zcode", "out", "node_modules", "__pycache__"}

CONTROL_RE = re.compile(r"\b(if|for|while|switch|catch|else|do|try)\b")
CONTAINER_RE = re.compile(r"\b(namespace|class|struct|enum|union)\b")
BRANCH_RE = re.compile(r"\b(if|for|while|case|catch)\b")


# ---- 源码预处理：剔除注释与字符串字面量（保留结构换行） ----------------------
def strip_comments_strings(src: str) -> str:
    out_chars: list[str] = []
    i, n = 0, len(src)
    state = "code"  # code | line_comment | block_comment | string | char | raw_string
    raw_delim = ""
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"; i += 2; out_chars.append("\n"); continue
            if c == "/" and nxt == "*":
                state = "block_comment"; i += 2; continue
            if c == '"':
                state = "string"; i += 1; out_chars.append('"'); continue
            if c == "'":
                state = "char"; i += 1; out_chars.append("'"); continue
            if c == "R" and nxt == '"':
                m = re.match(r'R"([^()\\\s]{0,16})\(', src[i:])
                if m:
                    raw_delim = m.group(1)
                    state = "raw_string"
                    i += m.end()
                    continue
            out_chars.append(c)
            i += 1
        elif state == "line_comment":
            if c == "\n":
                state = "code"; out_chars.append("\n")
            i += 1
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"; i += 2; continue
            if c == "\n":
                out_chars.append("\n")
            i += 1
        elif state == "string":
            if c == "\\":
                i += 2; continue
            if c == '"':
                state = "code"; out_chars.append('""')
            i += 1
        elif state == "char":
            if c == "\\":
                i += 2; continue
            if c == "'":
                state = "code"; out_chars.append("''")
            i += 1
        elif state == "raw_string":
            end = f"){raw_delim}\""
            j = src.find(end, i)
            if j == -1:
                i = n
            else:
                state = "code"
                i = j + len(end)
                out_chars.append('""')
    return "".join(out_chars)


# ---- 复杂度：函数体扫描 ------------------------------------------------------
def scan_functions(clean: str):
    """产出 [(起始行, 结束行)]。保守近似：宁可漏报不误报。"""
    results: list[tuple[int, int]] = []
    header: list[str] = []          # 自上一个边界（; { }）以来累积的头部文本
    stack: list[str] = []           # 'container' | 'control' | 'func' | 'other'
    block_starts: list[int] = []    # 与 stack 平行：各块的起始行
    line = 1
    i, n = 0, len(clean)

    def header_kind(h: str) -> str:
        tail = h.strip().splitlines()[-1] if h.strip() else ""
        if CONTAINER_RE.search(h):
            return "container"
        if CONTROL_RE.search(tail) or CONTROL_RE.search(h.split("(")[0]):
            return "control"
        if "(" in h and "=" not in h.split("(")[-1].split(")")[0]:
            return "func"
        return "other"

    while i < n:
        c = clean[i]
        if c == "\n":
            line += 1
            header.append("\n")
        elif c == "{":
            kind = header_kind("".join(header))
            header = []
            stack.append(kind)
            block_starts.append(line)
        elif c == "}":
            if stack:
                kind = stack.pop()
                start = block_starts.pop()
                if kind == "func":
                    results.append((start, line))
            header = []
        elif c == ";":
            header = []
        else:
            header.append(c)
        i += 1
    return results


def complexity_of(body: str) -> int:
    return 1 + len(BRANCH_RE.findall(body)) + body.count("&&") + body.count("||") + body.count("?")


def check_static(root: Path, rules: dict, files: list[str]) -> L.CheckResult:
    res = L.CheckResult("static", L.PASS)
    cfg = rules["code"]
    max_lines = int(cfg["func_max_lines"])
    max_lines_test = int(cfg["func_max_lines_test"])
    max_cplx = int(cfg["func_max_complexity"])
    checked = 0
    for rel in files:
        p = root / rel
        is_test = rel.startswith("tests/")
        limit = max_lines_test if is_test else max_lines
        clean = strip_comments_strings(p.read_text(encoding="utf-8", errors="replace"))
        funcs = scan_functions(clean)
        for start, end in funcs:
            checked += 1
            body_lines = end - start + 1
            body_text = "\n".join(clean.splitlines()[start - 1:end])
            cplx = complexity_of(body_text)
            if body_lines > limit:
                res.status = L.FAIL
                res.add("CODE-FUNC-LINES",
                        f"{rel}:{start} 函数体 {body_lines} 行 > 上限 {limit}")
            if cplx > max_cplx:
                res.status = L.FAIL
                res.add("CODE-FUNC-CPLX",
                        f"{rel}:{start} 圈复杂度近似 {cplx} > 上限 {max_cplx}")
    if res.status == L.PASS:
        res.summary = f"{checked} 个函数全部在行数/复杂度上限内" if checked else "无 C++ 函数"
    return res


# ---- 复制粘贴检测 ------------------------------------------------------------
def check_dup_code(root: Path, rules: dict, files: list[str]) -> L.CheckResult:
    res = L.CheckResult("dup_code", L.PASS)
    cfg = rules["code"]
    window = int(cfg["dup_code_window"])
    max_copies = int(cfg["dup_code_max_copies"])

    kept: list[tuple[str, str, int]] = []   # (规范化行, 文件, 行号)
    for rel in files:
        p = root / rel
        clean = strip_comments_strings(p.read_text(encoding="utf-8", errors="replace"))
        for lineno, raw in enumerate(clean.splitlines(), 1):
            norm = re.sub(r"\s+", "", raw)
            if len(norm) < 8:
                continue
            kept.append((norm, rel, lineno))

    groups: dict[tuple[str, ...], list[int]] = defaultdict(list)
    for idx in range(len(kept) - window + 1):
        groups[tuple(k[0] for k in kept[idx:idx + window])].append(idx)

    covered: set[int] = set()
    reported = 0
    for h, idxs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        if len(idxs) <= max_copies:
            continue
        free = [i for i in idxs if i not in covered]
        while len(free) > max_copies:
            a, b = free[0], free[1]
            res.status = L.FAIL
            reported += 1
            loc_a = f"{kept[a][1]}:{kept[a][2]}"
            loc_b = f"{kept[b][1]}:{kept[b][2]}"
            res.add("CODE-DUP", f"重复代码块（{window} 行）：{loc_a} ↔ {loc_b}")
            covered.update(range(a, a + window))
            covered.update(range(b, b + window))
            free = [i for i in free if i not in covered]
            if reported >= 10:
                res.add("CODE-DUP", "…更多重复块已截断")
                break
        if reported >= 10:
            break
    if res.status == L.PASS:
        res.summary = f"无 ≥{window} 行重复代码块"
    return res


# ---- 构建与快速测试 ----------------------------------------------------------
def check_build(root: Path, rules: dict) -> L.CheckResult:
    res = L.CheckResult("build", L.PASS)
    cfg = rules["code"]
    if not shutil.which("cmake") or not (shutil.which("g++") or shutil.which("clang++")):
        res.status = L.FAIL
        res.add("CODE-TOOLCHAIN", "已武装但找不到 cmake/g++ 工具链——判定只认实测，不虚绿")
        return res
    if not (root / "CMakeLists.txt").is_file():
        res.status = L.FAIL
        res.add("CODE-NOBUILD", "已有 C++ 代码但没有 CMakeLists.txt 构建入口")
        return res
    build_dir = root / cfg["build_dir"]
    flags = " ".join(cfg["cxx_flags"])
    steps = [
        ["cmake", "-S", str(root), "-B", str(build_dir),
         "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_CXX_FLAGS={flags}"],
        ["cmake", "--build", str(build_dir), "--parallel"],
        ["ctest", "--test-dir", str(build_dir), "-L", cfg["ctest_label"],
         "--output-on-failure"],
    ]
    for cmd in steps:
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", cwd=str(root))
        except FileNotFoundError as e:
            res.status = L.FAIL
            res.add("CODE-TOOLCHAIN", f"工具调用失败：{e}")
            return res
        if proc.returncode != 0:
            res.status = L.FAIL
            res.add("CODE-BUILD", f"失败：{' '.join(cmd[:3])}…（退出码 {proc.returncode}）")
            res.detail = ((proc.stdout or "") + (proc.stderr or "")).strip()[-4000:]
            return res
    res.summary = f"构建通过，ctest -L {cfg['ctest_label']} 全绿"
    return res


# ---- 装配 --------------------------------------------------------------------
def armed_files(root: Path, rules: dict) -> list[str]:
    files: list[str] = []
    for p in root.rglob("*"):
        rel_parts = p.relative_to(root).parts
        if not p.is_file() or p.suffix.lower() not in CPP_EXT:
            continue
        if any(part.startswith(".") or part in CXX_SKIP_DIRS for part in rel_parts[:-1]):
            continue
        rel = L.posix(p.relative_to(root))
        if L.glob_match(rel, rules["code"]["arm_globs"]):
            files.append(rel)
    return sorted(files)


def run(root: Path, rules: dict) -> list[L.CheckResult]:
    files = armed_files(root, rules)
    if not files:
        skip = L.CheckResult("code", L.SKIP,
                             summary=f"SKIP(NOT_ARMED)：仓库尚无 C++ 代码"
                                     f"（arm_globs 见 rules.toml [code]），"
                                     f"构建/静态检查未武装")
        return [skip]
    return [check_build(root, rules), check_static(root, rules, files),
            check_dup_code(root, rules, files)]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI code 阶段检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules)
    L.out("== code 阶段 ==")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
        if r.detail:
            L.out(r.detail)
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
