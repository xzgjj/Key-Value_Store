#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""guard 阶段检查器：规则资产守卫 + 防放松分析。

对应 SPEC.md "顺手关闭 CI 规则" 一行：
  * 规则资产（rules.toml / 检查器 / 协议与规格文档）出现已跟踪改动
    → 状态 APPROVAL（退出码 2），人工批准后才能继续；
  * 防放松：对比 HEAD 与工作区的 rules.toml，数值阈值调大（预算/上限/重试轮数）、
    白名单加条目、保护清单删条目 → 在报告中明确标注"放松"，同样需要批准；
    收紧标注"收紧"，随本次批准一并放行（收紧本身不额外拦截）；
  * 未跟踪的新规则文件 = 首次引入即基线，只提示不拦截。

无 git 基线时整阶段 SKIP(NO_BASELINE)。
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402

# 数值阈值语义：high = 越大越松；low = 越小越松
NUMERIC_RULES: dict[tuple[str, str], str] = {
    ("retry", "max_rounds"): "high",
    ("doc_budget", "planning_max_chars"): "high",
    ("doc_budget", "analysis_max_para_occurrences"): "high",
    ("doc_budget", "analysis_min_para_chars"): "low",
    ("bugfix", "min_scene_chars"): "low",
    ("scope", "max_diff_lines"): "high",
    ("scope", "max_changed_files"): "high",
    ("code", "func_max_lines"): "high",
    ("code", "func_max_lines_test"): "high",
    ("code", "func_max_complexity"): "high",
    ("code", "dup_code_window"): "high",
    ("code", "dup_code_max_copies"): "high",
}
# 列表语义：add = 加条目即放松；remove = 删条目即放松
LIST_RULES: dict[str, str] = {
    "always_allowed": "add",
    "reference_freeze": "add",
    "planning_files": "remove",
    "test_globs": "remove",
    "rule_files": "remove",
    "cxx_flags": "remove",
    "arm_globs": "remove",
}


def analyze_loosening(old: dict, new: dict) -> tuple[list[str], list[str]]:
    """返回 (放松清单, 收紧清单)。仅针对 rules.toml；解析不出的项忽略。"""
    weaken: list[str] = []
    tighten: list[str] = []

    for (sec, key), direction in NUMERIC_RULES.items():
        o = old.get(sec, {}).get(key)
        n = new.get(sec, {}).get(key)
        if o is None and n is None:
            continue
        if o is not None and n is None:
            weaken.append(f"{sec}.{key} 被移除（原值 {o}）")
            continue
        if o is None and n is not None:
            tighten.append(f"{sec}.{key} 新增（{n}）")
            continue
        bigger, smaller = (n, o) if n > o else (o, n) if n < o else (None, None)
        if bigger is None:
            continue
        rule = f"{sec}.{key}: {o} → {n}"
        weakened = (direction == "high" and n > o) or (direction == "low" and n < o)
        (weaken if weakened else tighten).append(rule)

    for key, mode in LIST_RULES.items():
        found = False
        for src in (old, new):
            for sec, body in src.items():
                if isinstance(body, dict) and key in body and isinstance(body[key], list):
                    o_set = set(map(str, old.get(sec, {}).get(key, [])))
                    n_set = set(map(str, new.get(sec, {}).get(key, [])))
                    added, removed = n_set - o_set, o_set - n_set
                    if mode == "add" and added:
                        weaken.append(f"{sec}.{key} 新增条目：{sorted(added)}")
                    if mode == "remove" and removed:
                        weaken.append(f"{sec}.{key} 移除条目：{sorted(removed)}")
                    if mode == "add" and removed:
                        tighten.append(f"{sec}.{key} 移除条目：{sorted(removed)}")
                    if mode == "remove" and added:
                        tighten.append(f"{sec}.{key} 新增条目：{sorted(added)}")
                    found = True
                    break
            if found:
                break

    return weaken, tighten


def _rule_approval(root: Path, rules: dict) -> tuple[bool, str]:
    """读取任务声明里的规则资产批准留痕；文件缺失/解析失败一律按"未批准"处理。"""
    try:
        ts = L.load_task_scope(root, rules)
    except (OSError, ValueError):
        return False, ""
    approver = str(ts.get("approver", "")).strip()
    return bool(ts.get("rule_asset_approved")), approver


def check_guard(root: Path, rules: dict, base: str | None = None) -> L.CheckResult:
    res = L.CheckResult("guard", L.PASS)
    base = base or L.resolve_base()
    if not L.has_head(root):
        res.status = L.SKIP
        res.summary = "SKIP(NO_BASELINE)：仓库尚无基线提交，规则守卫未武装"
        return res

    patterns = rules["guard"]["rule_files"]
    changed = L.filter_ignored(L.changed_files(root, base), rules)
    if base:
        res.summary = f"比对基线 {base}"

    new_files = [p for c, p in changed if "??" in c and L.glob_match(p, patterns)]
    if new_files:
        res.add("GUARD-NEW", "新增规则文件（首次引入即基线，随基线提交）：" + "、".join(new_files))

    hits = [(c, p) for c, p in changed if "??" not in c and L.glob_match(p, patterns)]
    if not hits:
        if res.status == L.PASS:
            res.summary = ("规则资产无改动" if not base
                           else f"与基线 {base} 相比规则资产无改动")
        return res

    res.status = L.APPROVAL
    for c, p in hits[:20]:
        res.add("GUARD-CHG", f"[{c}] {p}")
    if len(hits) > 20:
        res.add("GUARD-CHG", f"…另有 {len(hits) - 20} 个规则文件改动")

    # 审批留痕放行（SPEC.md §5）：task_scope.json 里 rule_asset_approved=true 且填了
    # approver，才把"需人工批准"落成"已获批"。没有这条通道，规则资产一旦改动 CI 永远红，
    # 守卫就退化成"谁都不敢动规则"，而不是"动规则要留痕"。
    approved, approver = _rule_approval(root, rules)
    if approved and approver:
        res.status = L.PASS
        res.summary = f"规则资产改动 {len(hits)} 个（已获批放行）"
        res.add("GUARD-APPROVED", f"规则资产改动已获批（批准人：{approver}），放行")
    else:
        res.add("GUARD-APPROVE", "规则资产改动需人工批准后方可继续（退出码 2）："
                                 f"在 {rules['scope']['task_scope_file']} 填 "
                                 "rule_asset_approved=true + approver")

    # 防放松分析（仅 rules.toml 有旧版本可比时）：base 模式下旧版本取自基线提交
    rules_rel = "ci/rules.toml"
    if L.glob_match(rules_rel, patterns) and any(p == rules_rel for _, p in hits):
        old_ref = base or "HEAD"
        rc, old_text = L.git(root, "show", f"{old_ref}:{rules_rel}")
        if rc == 0:
            try:
                old = tomllib.loads(old_text)
                new = L.load_rules(root)
                weaken, tighten = analyze_loosening(old, new)
                for w in weaken:
                    res.add("GUARD-WEAKEN", f"放松：{w}")
                for t in tighten:
                    res.add("GUARD-TIGHTEN", f"收紧：{t}")
            except tomllib.TOMLDecodeError as e:
                res.add("GUARD-ANALYZE", f"rules.toml 新旧对比失败：{e}")
    return res


def run(root: Path, rules: dict, base: str | None = None) -> list[L.CheckResult]:
    return [check_guard(root, rules, base)]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI guard 阶段检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    ap.add_argument("--base", default=None,
                    help="diff 基线（CI 用 PR base / HEAD^）；缺省比对工作树")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules, args.base)
    L.out("== guard 阶段 ==")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
