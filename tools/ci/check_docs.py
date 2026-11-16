#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""docs 阶段检查器：文档预算 / 重复段落 / 一致性 / 测试登记表（兼容补充）。

对应 SPEC.md "文档管理" 一行：
  * budget    规划文档字数预算（分析/记录文档不设上限，但受重复段落约束）
  * dup_para  跨文档重复段落（agent"事无巨细堆细节"的信号）
  * consistency 相对链接可解析 + 正文引用的 tools/*.py 存在
  * registry  复用既有 tools/check_test_registry.py（登记表 ⇆ 仓库双向对账）

既被 run_ci.py 聚合调用，也可单独运行：
  python tools/ci/check_docs.py [--root <仓库路径>]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402


# ---- registry：既有登记表一致性检查（兼容补充，不重写） ----------------------
def check_registry(root: Path, rules: dict) -> L.CheckResult:
    res = L.CheckResult("registry", L.PASS)
    script = rules.get("registry", {}).get("script", "tools/check_test_registry.py")
    p = root / script
    if not p.is_file():
        res.status = L.SKIP
        res.summary = f"SKIP：登记检查脚本不存在（{script}）"
        return res
    proc = subprocess.run(
        [sys.executable, str(p), "--root", str(root)],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        res.status = L.FAIL
        res.summary = "登记表与仓库不一致"
        res.detail = ((proc.stdout or "") + (proc.stderr or "")).strip()
        res.add("REG-CONSIST", "docs/testing.md 登记表 ⇆ 仓库对账失败（见 detail）")
    else:
        res.summary = "登记表与仓库一致"
    return res


# ---- budget：规划文档字数预算 ------------------------------------------------
def check_budget(root: Path, rules: dict) -> L.CheckResult:
    cfg = rules["doc_budget"]
    res = L.CheckResult("budget", L.PASS)
    planning = {n.lower() for n in cfg["planning_files"]}
    limit = int(cfg["planning_max_chars"])
    counts: list[tuple[str, int]] = []
    for p, rel in L.iter_md_files(root, cfg.get("reference_freeze", [])):
        name = p.name.lower()
        if name not in planning:
            continue
        n = L.doc_word_count(L.read_text(p))
        counts.append((rel, n))
        if n > limit:
            res.status = L.FAIL
            res.add("DOC-BUDGET", f"规划文档超预算：{rel} = {n} 字 > {limit} 字")
    if counts:
        top = sorted(counts, key=lambda x: -x[1])
        res.summary = ("规划文档 " + "、".join(f"{r}={n}字" for r, n in top)
                       + f"（上限 {limit} 字）")
    else:
        res.summary = "未发现规划文档"
    return res


# ---- dup_para：跨文档重复段落 ------------------------------------------------
def check_dup_para(root: Path, rules: dict) -> L.CheckResult:
    cfg = rules["doc_budget"]
    res = L.CheckResult("dup_para", L.PASS)
    min_chars = int(cfg.get("analysis_min_para_chars", 40))
    max_occ = int(cfg.get("analysis_max_para_occurrences", 1))
    frozen = set(cfg.get("reference_freeze", []))

    seen: dict[str, list[str]] = {}   # 规范化段落 -> 出现位置
    for p, rel in L.iter_md_files(root):
        text = L.strip_fenced(L.read_text(p))
        for block in re.split(r"\n\s*\n", text):
            norm = L.normalize_para(block)
            if len(norm) >= min_chars:
                seen.setdefault(norm, []).append(rel)

    frozen_only = lambda locs: all(L.glob_match(x, frozen) for x in locs)
    reported = 0
    for norm, locs in seen.items():
        if len(locs) <= max_occ or frozen_only(locs):
            continue
        if reported == 0:
            res.status = L.FAIL
        reported += 1
        preview = norm[:40] + ("…" if len(norm) > 40 else "")
        locs_str = "、".join(sorted(set(locs)))
        res.add("DOC-DUP", f"段落重复 {len(locs)} 处（“{preview}”）→ {locs_str}")
        if reported >= 20:  # 防刷屏，其余按计数汇总
            res.add("DOC-DUP", f"…另有 {sum(1 for v in seen.values() if len(v) > max_occ) - reported} 组重复段落")
            break
    if res.status == L.PASS:
        res.summary = f"无重复段落（≥{min_chars} 字，出现 >{max_occ} 次即报）"
    return res


# ---- consistency：链接 + tools 引用 ------------------------------------------
def check_consistency(root: Path, rules: dict) -> L.CheckResult:
    cfg = rules["doc_budget"]
    res = L.CheckResult("consistency", L.PASS)
    n_links = n_tools = 0
    for p, rel in L.iter_md_files(root, cfg.get("reference_freeze", [])):
        text = L.read_text(p)
        for target in L.relative_md_links(text):
            t = target.split("#", 1)[0]
            if not t:
                continue  # 纯锚点
            dest = (p.parent / t).resolve()
            if not dest.exists():
                res.status = L.FAIL
                res.add("DOC-LINK", f"{rel} → {target}（目标不存在）")
            n_links += 1
        for tool in L.tool_refs(text):
            n_tools += 1
            if not (root / tool).is_file():
                res.status = L.FAIL
                res.add("DOC-TOOL", f"{rel} 引用的脚本不存在：{tool}")
    if res.status == L.PASS:
        res.summary = f"链接 {n_links} 个、脚本引用 {n_tools} 处全部可解析"
    return res


def run(root: Path, rules: dict) -> list[L.CheckResult]:
    return [
        check_registry(root, rules),
        check_budget(root, rules),
        check_dup_para(root, rules),
        check_consistency(root, rules),
    ]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI docs 阶段检查")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules)
    L.out(f"== docs 阶段 ==（{root}）")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
        if r.detail:
            L.out(r.detail)
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
