#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyKV-CI 统一入口。

用法：
    python tools/ci/run_ci.py [--stage all|docs|guard|scope|code]
                              [--round N] [--json] [--root <仓库路径>]

退出码契约（判定只认实测）：
    0  绿（PASS / SKIP；SKIP 在报告中可见列出，不算静默通过）
    1  FAIL——可判定问题被拦截
    2  APPROVAL——规则资产被改动，须人工批准后继续
    1  --round 超过 rules.toml [retry].max_rounds 时同样拒绝执行
      （机械实现"限制 CI 重试轮数"，防止一次任务无限循环改到绿）

各阶段：
    docs   文档预算 / 重复段落 / 一致性 / 测试登记表（兼容补充）
    bugfix 缺陷回归：触发场景 + 可执行复现 + 已登记回归测试 + 编号标记（docs/bugs.md）
    guard  规则资产守卫 + 防放松分析
    scope  任务声明 / 范围白名单 / diff 预算 / 测试删除告警
    code   构建 + ctest fast + 复杂度 + 复制粘贴（无 C++ 时 SKIP(NOT_ARMED)）
    align  跨语言契约集合 / 文档命令可执行性 / 违禁模式（只做可判定比对）
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402
import check_align  # noqa: E402
import check_bugfix  # noqa: E402
import check_code  # noqa: E402
import check_docs  # noqa: E402
import check_guard  # noqa: E402
import check_scope  # noqa: E402

STAGE_RUNNERS = {
    "docs": check_docs.run,
    "bugfix": check_bugfix.run,
    "guard": check_guard.run,
    "scope": check_scope.run,
    "code": check_code.run,
    "align": check_align.run,
}

STATUS_ICON = {L.PASS: "✓", L.SKIP: "○", L.FAIL: "✗", L.APPROVAL: "⚠"}


def print_report(results: list[L.CheckResult]) -> None:
    L.out("== TinyKV-CI 报告 ==")
    for r in results:
        L.out(f"  {STATUS_ICON.get(r.status, '?')} [{r.status:<8}] {r.check:<12} {r.summary}")
        for f in r.findings:
            L.out(f"        {f.code}  {f.message}")
        if r.detail:
            L.out("        ---- 子命令输出 ----")
            for line in r.detail.splitlines():
                L.out(f"        {line}")
    counts = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1
    L.out("  汇总：" + "、".join(f"{k}×{v}" for k, v in sorted(counts.items())))


def print_json(results: list[L.CheckResult], stage: str, rnd: int, exit_code: int) -> None:
    payload = {
        "round": rnd,
        "stage": stage,
        "exit_code": exit_code,
        "results": [
            {
                "check": r.check,
                "status": r.status,
                "summary": r.summary,
                "findings": [{"code": f.code, "message": f.message} for f in r.findings],
            }
            for r in results
        ],
    }
    L.out(json.dumps(payload, ensure_ascii=False))


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI 统一入口")
    ap.add_argument("--stage", choices=["all", *STAGE_RUNNERS], default="all")
    ap.add_argument("--round", type=int, default=1,
                    help="本轮修复重试的轮次（1 起），超过 rules.toml [retry].max_rounds 拒绝执行")
    ap.add_argument("--json", action="store_true", help="额外输出机器可读 JSON 行")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()

    if not (root / "ci" / "rules.toml").is_file():
        L.out(f"FAIL: 未找到规则文件 {root / 'ci' / 'rules.toml'}")
        return L.EXIT_FAIL

    rules = L.load_rules(root)
    max_rounds = int(rules["retry"]["max_rounds"])
    if not 1 <= args.round <= max_rounds:
        L.out(f"FAIL: --round {args.round} 超出重试上限（1..{max_rounds}，rules.toml [retry].max_rounds）；"
              f"连续 {max_rounds} 轮未绿须人工介入，不得继续循环。")
        return L.EXIT_FAIL

    stages = list(STAGE_RUNNERS) if args.stage == "all" else [args.stage]
    L.out(f"== TinyKV-CI ==（第 {args.round}/{max_rounds} 轮，阶段：{'+'.join(stages)}）")

    results: list[L.CheckResult] = []
    for stage in stages:
        results.extend(STAGE_RUNNERS[stage](root, rules))

    print_report(results)
    exit_code = L.aggregate_exit(results)
    if exit_code == L.EXIT_APPROVAL:
        L.out("  结论：APPROVAL——存在需人工批准的改动，先停下等批准，不要自行绕过。")
    elif exit_code == L.EXIT_FAIL:
        L.out("  结论：FAIL——修复上述可判定问题后重跑（计入下一轮）。")
    else:
        L.out("  结论：绿。可提交（提交说明见 AGENT_EXECUTION_PROTOCOL.md）。")
    if args.json:
        print_json(results, args.stage, args.round, exit_code)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
