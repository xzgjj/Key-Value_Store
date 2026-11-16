#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scope 阶段检查器：改动范围白名单 / diff 预算 / 测试删除告警。

对应 SPEC.md "小任务变成大重构" 与 "改测试让失败消失" 两行。
依赖 git 基线（HEAD）；无基线时整阶段 SKIP(NO_BASELINE)——首次引入即基线，
集成是项目侧的后续事项，本检查器只在基线存在后武装。

任务声明文件（rules.toml [scope].task_scope_file）格式：
    {
      "task": "一句话任务描述",
      "paths": ["ci/", "tools/"],            // 允许触碰的路径（目录或文件）
      "reason": "为什么需要这些改动",
      "overflow_approved": false,            // diff 超预算时的人工批准
      "behavior_change_approved": false,     // 删除测试/登记行时的人工批准
      "approver": ""                         // 批准人（必填才生效）
    }
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402


def check_scope(root: Path, rules: dict, base: str | None = None) -> L.CheckResult:
    res = L.CheckResult("scope", L.PASS)
    # 基线解析：显式参数 > 环境变量（run_ci --base 会设 TINYKV_DIFF_BASE）> 工作树模式
    base = base or L.resolve_base()
    if not L.has_head(root):
        res.status = L.SKIP
        res.summary = "SKIP(NO_BASELINE)：仓库尚无基线提交，范围/预算检查未武装"
        return res

    scfg = rules["scope"]
    changed = L.filter_ignored(L.changed_files(root, base), rules)
    changed = [(c, p) for c, p in changed
               if not L.glob_match(p, scfg.get("always_allowed", []))]

    if not changed:
        res.summary = ("工作区干净（或仅 always_allowed 内容）" if not base
                       else f"与基线 {base} 相比无改动")
        return res

    # 1) 任务声明
    ts_path = root / scfg["task_scope_file"]
    declared: list[str] = []
    overflow_ok = behavior_ok = False
    approver = ""
    if ts_path.is_file():
        try:
            ts = L.load_task_scope(root, rules)
            declared = [L.posix(x).rstrip("/") for x in ts.get("paths", [])]
            overflow_ok = bool(ts.get("overflow_approved"))
            behavior_ok = bool(ts.get("behavior_change_approved"))
            approver = str(ts.get("approver", "")).strip()
        except (json.JSONDecodeError, KeyError, OSError) as e:
            res.status = L.FAIL
            res.add("SCOPE-DECL", f"{scfg['task_scope_file']} 解析失败：{e}")
            return res
        if not declared:
            res.status = L.FAIL
            res.add("SCOPE-DECL", "任务声明存在但 paths 为空")
    else:
        res.status = L.FAIL
        res.add("SCOPE-DECL",
                f"有未提交改动但未写任务声明 {scfg['task_scope_file']}（格式见 CI.md）")

    # 2) 范围对账：声明外的改动
    outsiders = [p for _, p in changed
                 if not any(p == d or p.startswith(d + "/") for d in declared)]
    if outsiders:
        res.status = L.FAIL
        for p in outsiders[:10]:
            res.add("SCOPE-OUT", f"声明范围外改动：{p}")
        if len(outsiders) > 10:
            res.add("SCOPE-OUT", f"…另有 {len(outsiders) - 10} 个范围外改动")
        res.add("SCOPE-FIX", "处理：更新 task_scope.json 声明，或撤销改动；范围外新增须人工批准")

    # 3) diff 预算
    added, deleted, _ = L.diff_numstat(root, base)
    # base 模式（CI）下 git diff 已覆盖全部改动；未跟踪文件只存在于工作树模式
    untracked_lines = 0 if base else L.untracked_added_lines(root, changed)
    total_lines = added + deleted + untracked_lines
    total_files = len({p for _, p in changed})
    over = (total_lines > scfg["max_diff_lines"]
            or total_files > scfg["max_changed_files"])
    if over:
        if overflow_ok and approver:
            res.add("SCOPE-OVER-OK",
                    f"diff 超预算已获人工批准：{total_lines} 行 / {total_files} 文件"
                    f"（批准人：{approver}）")
        else:
            res.status = L.FAIL
            res.add("SCOPE-OVER",
                    f"diff 超预算：{total_lines} 行（≤{scfg['max_diff_lines']}）、"
                    f"{total_files} 文件（≤{scfg['max_changed_files']}）；"
                    f"超出部分须人工批准（overflow_approved + approver）")
    elif res.status == L.PASS:
        res.summary = f"diff {total_lines} 行 / {total_files} 文件，在预算内且已声明"

    # 4) 测试删除告警
    deleted_paths = [p for c, p in changed if "D" in c]
    test_hits = [p for p in deleted_paths if L.glob_match(p, scfg.get("test_globs", []))]
    reg_rows = 0
    regfile = scfg.get("registry_file")
    if regfile:
        rc, diff = L.git(root, "diff", "HEAD", "--", regfile)
        for line in diff.splitlines():
            if line.startswith("-") and line.count("|") >= 2 \
                    and not set(line[1:]) <= set("|-: \t"):
                reg_rows += 1
    if test_hits or reg_rows:
        if behavior_ok and approver:
            res.add("SCOPE-TESTDEL-OK",
                    f"测试删除已获人工批准：{len(test_hits)} 个测试资产 / {reg_rows} 行登记"
                    f"（批准人：{approver}）")
        else:
            res.status = L.FAIL
            for p in test_hits[:5]:
                res.add("SCOPE-TESTDEL", f"测试资产被删除：{p}")
            if reg_rows:
                res.add("SCOPE-TESTDEL", f"登记表 {regfile} 疑似删除 {reg_rows} 行")
            res.add("SCOPE-FIX",
                    "行为变更须人工批准：task_scope.json 填 behavior_change_approved=true + approver")
    return res


def run(root: Path, rules: dict, base: str | None = None) -> list[L.CheckResult]:
    return [check_scope(root, rules, base)]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI scope 阶段检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    ap.add_argument("--base", default=None,
                    help="diff 基线（如 origin/main、HEAD^）；缺省比对工作树")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules, args.base)
    L.out("== scope 阶段 ==")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
