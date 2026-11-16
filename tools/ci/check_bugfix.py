#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bugfix 阶段检查器：修 bug 必须留下触发场景与回归测试（"改完就完"在此拦下）。

对应 SPEC.md §2 表中"修 bug 不留回归场景"一行。判定全部可判定，不做语义判断：

    BUG-STRUCT        结构：标记区存在、5 列、Bug 编号 BUG-NNNN 且唯一
    BUG-SCENE         场景：触发场景字数 ≥ [bugfix].min_scene_chars（挡"修了个 bug"式空描述）
    BUG-REPRO         复现：复现列必须含反引号包裹的可执行命令（种子/参数/脚本均可）
    BUG-NOREG         登记：回归测试名必须已在 docs/testing.md §4 登记（沿用唯一事实来源）
    BUG-UNLINKED      绑定：回归测试源码里必须出现该 Bug 编号（防止"登记了但没真跑"）
    BUG-DATE          日期：修复列必须含 YYYY-MM-DD
    BUG-ROWDEL        只增：基线中已有的 Bug 编号不得消失（历史不可删，更正另起一行）
    BUG-NOROW         声明：task_scope.json 声明 bugfix=true 时必须新增登记行
    BUG-FIXES-MISSING 声明：task_scope.json fixes 列出的编号必须真实存在
    BUG-REGISTRY-DEL  登记表被删除（基线中有、现在没有）

登记表格式（docs/bugs.md 标记区内）：`Bug | 触发场景 | 复现 | 回归测试 | 修复`。

单独运行：
    python tools/ci/check_bugfix.py [--root <仓库路径>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402

REGISTRY_BEGIN = "<!-- bugs:begin"
REGISTRY_END = "<!-- bugs:end"
TEST_REG_BEGIN = "<!-- registry:begin"
TEST_REG_END = "<!-- registry:end"
COLUMNS = ("bug", "scene", "repro", "test", "fix")
HEADER_FIRST = "Bug"
ID_RE = re.compile(r"^BUG-\d{4}$")
DATE_RE = re.compile(r"\b\d{4}-\d{2}-\d{2}\b")
BACKTICK_RE = re.compile(r"`([^`]+)`")
SEPARATOR_ROW_RE = re.compile(r"^[\s|:\-]+$")
TEST_FILE_RE = re.compile(r".*_test\.(cpp|cc|cxx|h|hpp|sh|py)$")
CMD_HINT = ("/", "=", "--", " ")


def region(text: str, begin: str, end: str) -> str | None:
    """取 begin…end 之间的文本；标记缺失或错序返回 None。"""
    b, e = text.find(begin), text.find(end)
    if b == -1 or e == -1 or e < b:
        return None
    return text[b:e]


def parse_bugs(text: str) -> tuple[list[dict], list[tuple[str, str]]]:
    """解析标记区表格，返回 (登记行, [(发现码, 说明)])。"""
    rows: list[dict] = []
    errs: list[tuple[str, str]] = []
    seen: set[str] = set()
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if SEPARATOR_ROW_RE.match(line) or cells[0] == HEADER_FIRST:
            continue
        if len(cells) != len(COLUMNS):
            errs.append(("BUG-STRUCT", f"第 {lineno} 行：列数 {len(cells)} ≠ {len(COLUMNS)}"
                                       f"（表头：Bug | 触发场景 | 复现 | 回归测试 | 修复）；"
                                       f"单元格内不要写竖线"))
            continue
        row = dict(zip(COLUMNS, cells))
        row["lineno"] = lineno
        if not ID_RE.match(row["bug"]):
            errs.append(("BUG-STRUCT", f"第 {lineno} 行：Bug 编号非法：{row['bug']}"
                                       f"（应为 BUG-NNNN，四位递增不复用）"))
            continue
        if row["bug"] in seen:
            errs.append(("BUG-STRUCT", f"第 {lineno} 行：Bug 编号重复：{row['bug']}"))
        seen.add(row["bug"])
        rows.append(row)
    return rows, errs


def parse_test_registry(root: Path, rel: str) -> dict[str, str]:
    """docs/testing.md §4 登记表 → {ctest 测试名: Scope 路径}；不可解析返回 {}。"""
    path = root / rel
    if not path.is_file():
        return {}
    body = region(L.read_text(path), TEST_REG_BEGIN, TEST_REG_END)
    if body is None:
        return {}
    mapping: dict[str, str] = {}
    for raw in body.splitlines():
        line = raw.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 3 or SEPARATOR_ROW_RE.match(line) or cells[0] == "Scope":
            continue
        bt = BACKTICK_RE.findall(cells[2])
        if bt:
            mapping[bt[0]] = L.posix(cells[0])
    return mapping


def scope_has_marker(root: Path, scope: str, marker: str) -> bool:
    """Scope 指向的文件/目录内是否出现该 Bug 编号标记。"""
    p = root / scope
    if p.is_file():
        return marker in L.read_text(p)
    if p.is_dir():
        for f in sorted(p.rglob("*")):
            if f.is_file() and TEST_FILE_RE.match(f.name) and marker in L.read_text(f):
                return True
    return False


def check_bugfix(root: Path, rules: dict) -> L.CheckResult:
    cfg = rules["bugfix"]
    res = L.CheckResult("bugfix", L.PASS)
    reg_rel = L.posix(cfg["registry_file"])
    reg_path = root / reg_rel

    # 0) 登记表本身：不存在 → 未武装；基线中存在却被删 → 红
    if not reg_path.is_file():
        rc, _ = L.git(root, "cat-file", "-e", f"HEAD:{reg_rel}")
        if rc == 0:
            res.status = L.FAIL
            res.add("BUG-REGISTRY-DEL", f"{reg_rel} 已被删除（基线中存在），缺陷登记不可删除")
        else:
            res.status = L.SKIP
            res.summary = f"SKIP(NOT_ARMED)：{reg_rel} 未建立，缺陷回归门禁未武装"
        return res

    body = region(L.read_text(reg_path), REGISTRY_BEGIN, REGISTRY_END)
    if body is None:
        res.status = L.FAIL
        res.add("BUG-STRUCT", f"{reg_rel} 缺少标记区 {REGISTRY_BEGIN} … {REGISTRY_END}，"
                              f"登记表无法解析")
        return res

    rows, errs = parse_bugs(body)
    for code, msg in errs:
        res.status = L.FAIL
        res.add(code, msg)

    test_map = parse_test_registry(root, cfg["test_registry_file"])
    min_chars = int(cfg["min_scene_chars"])

    for r in rows:
        bid = r["bug"]
        # 1) 触发场景：短到看不出现象与条件 = 没留下场景
        n = L.doc_word_count(r["scene"])
        if n < min_chars:
            res.status = L.FAIL
            res.add("BUG-SCENE", f"{bid}：触发场景仅 {n} 字（< {min_chars}）——写清现象 + 触发条件，"
                                 f"不写「修了个 bug」")
        # 2) 复现：必须给得出可重跑的命令（种子/参数/脚本）
        cmds = [c for c in BACKTICK_RE.findall(r["repro"])
                if len(c) >= 6 and any(h in c for h in CMD_HINT)]
        if not cmds:
            res.status = L.FAIL
            res.add("BUG-REPRO", f"{bid}：复现列没有可执行命令——用反引号写种子/参数/脚本，"
                                 f"如 `--seed=42`、`ctest -R sim_split --gtest_filter=*Crash*`")
        # 3) 回归测试必须先在 docs/testing.md §4 登记；且源码里有编号标记
        name = (BACKTICK_RE.findall(r["test"]) or [r["test"]])[0].strip()
        scope = test_map.get(name)
        if scope is None:
            res.status = L.FAIL
            res.add("BUG-NOREG", f"{bid}：回归测试 `{name}` 未在 {cfg['test_registry_file']} §4 登记"
                                 f"（先按 §5 SOP 登记测试，再回填本表）")
        elif not (root / scope).exists():
            res.status = L.FAIL
            res.add("BUG-NOREG", f"{bid}：回归测试 `{name}` 的 Scope 路径不存在：{scope}")
        elif not scope_has_marker(root, scope, bid):
            res.status = L.FAIL
            res.add("BUG-UNLINKED", f"{bid}：{scope} 内找不到标记 {bid}——回归测试用例需加注释 "
                                    f"`// {bid}: <一句话>`，否则无从追到具体用例")
        # 4) 修复列必须带日期（可追到哪次改动）
        if not DATE_RE.search(r["fix"]):
            res.status = L.FAIL
            res.add("BUG-DATE", f"{bid}：修复列缺少日期（YYYY-MM-DD，有 commit 一并写上）")

    if not rows and res.status == L.PASS:
        res.summary = f"缺陷登记 0 条（{reg_rel} 尚未有条目）"

    # 5) 历史不可删 + 任务声明联动（首次引入即基线：基线无此文件时只为空集合）
    rc, head_text = L.git(root, "show", f"HEAD:{reg_rel}")
    head_rows = (parse_bugs(region(head_text, REGISTRY_BEGIN, REGISTRY_END) or "")[0]
                 if rc == 0 else [])
    current_ids = {r["bug"] for r in rows}
    head_ids = {r["bug"] for r in head_rows}
    for missing in sorted(head_ids - current_ids):
        res.status = L.FAIL
        res.add("BUG-ROWDEL", f"{missing} 被本次改动删除——缺陷登记只增不删，"
                              f"更正请另起一行并写明取代关系")
    try:
        ts = L.load_task_scope(root, rules)
    except (OSError, ValueError):
        ts = {}
    fixes = [str(x).strip() for x in ts.get("fixes", []) if str(x).strip()]
    if bool(ts.get("bugfix")) and not (current_ids - head_ids):
        res.status = L.FAIL
        res.add("BUG-NOROW", "task_scope.json 声明 bugfix=true，但本任务没有新增缺陷登记行"
                             "——触发场景必须先落下，再谈修复")
    for fid in fixes:
        if fid not in current_ids:
            res.status = L.FAIL
            res.add("BUG-FIXES-MISSING", f"task_scope.json fixes 声明了 {fid}，"
                                         f"但 {reg_rel} 没有这条登记")

    if res.status == L.PASS and rows:
        res.summary = f"缺陷登记 {len(rows)} 条：场景/复现/登记回归测试/编号标记全部就位"
    return res


def run(root: Path, rules: dict) -> list[L.CheckResult]:
    return [check_bugfix(root, rules)]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI bugfix 阶段检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules)
    L.out("== bugfix 阶段 ==")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
