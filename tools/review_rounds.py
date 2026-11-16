#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""审查轮次持久化机制：把"三轮检查"落成可追溯、可门禁的台账。

为什么要有它
------------
项目纪律要求"每阶段结束 review 三次通过"，但轮次结论原先只存在于对话里——
上下文一断就失忆，谁也无法证明"三轮"真的跑过。本工具把轮次写进
`docs/review-log.md` 的机器可读标记区（复用 docs/testing.md 的 registry 范式）：
人读是表格，机读是同一份表格，随提交进入历史，跨会话可查。

阶段识别
--------
缺省 stage = 当前分支名映射（分支 `0` → 阶段 M0），与"SPEC 编号 = 阶段编号 =
分支编号"的约定同源；不在整数分支上时须显式 --stage，避免记到错误阶段。

用法
----
    python tools/review_rounds.py status                 # 看本轮阶段已记录的轮次
    python tools/review_rounds.py record --focus "可判定性" --verdict PASS
    python tools/review_rounds.py gate --rounds 3        # 三轮全 PASS 才允许收尾

退出码：0 通过；1 未达标（轮次不足或有 FAIL）；2 环境/参数问题。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

LOG_REL = "docs/review-log.md"
BEGIN = "<!-- review:begin v1 -->"
END = "<!-- review:end -->"
HEADER = (
    "| 阶段 | 轮次 | 重点 | 结论 | 备注 |\n"
    "|---|---|---|---|---|\n"
)
TEMPLATE = f"""# 审查台账（review-log）

> 每阶段收尾的"三轮检查"记录在此，随提交入库、跨会话可查。
> 写入走 `python tools/review_rounds.py record`，门禁走 `gate`；**不要手工改标记区**，
> 表格列宽由工具维护，手改会破坏解析。
> 每轮设不同重点：可判定性 → 契约与复用 → 文档与红线（见 AGENT_EXECUTION_PROTOCOL.md §8）。

## 轮次记录

{BEGIN}
{HEADER}{END}

## 约定

- 「结论」只允许 PASS / FAIL；FAIL 轮次保留在台账里，作为修复依据，不删除。
- 三轮全 PASS 后跑对齐检查（`python tools/ci/check_align.py`），两者都过才可进入推送环节。
- 台账本身不替代证据：每轮「备注」应写清跑了什么命令、产物在哪。
"""


@dataclass
class Round:
    stage: str
    n: int
    focus: str
    verdict: str
    note: str


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def parse_rounds(text: str) -> list[Round]:
    """解析标记区内的表格行；非法行直接忽略（模板表头/分隔行即因此跳过）。"""
    start, end = text.find(BEGIN), text.find(END)
    if start < 0 or end < start:
        return []
    rounds: list[Round] = []
    for line in text[start:end].splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = _split_row(line)
        if len(cells) < 4 or not cells[1].isdigit() or cells[3] not in {"PASS", "FAIL"}:
            continue
        rounds.append(Round(cells[0], int(cells[1]), cells[2], cells[3],
                            cells[4] if len(cells) > 4 else ""))
    return rounds


def _sanitize(text: str) -> str:
    """表格单元格禁用竖线与换行，避免破坏解析。"""
    return re.sub(r"\s+", " ", text.replace("|", "/")).strip()


def append_round(path: Path, row: Round) -> None:
    text = _read(path)
    cells = "| " + " | ".join(_sanitize(x) for x in
                              (row.stage, str(row.n), row.focus, row.verdict, row.note)) + " |\n"
    end = text.find(END)
    if end < 0:
        raise SystemExit(f"{LOG_REL} 缺少结束标记 {END}，请恢复标记区后再写入")
    path.write_text(text[:end] + cells + text[end:], encoding="utf-8")


def current_stage(root: Path) -> str:
    """分支 `0` → `M0`；非整数分支返回空串（调用方须显式指定）。"""
    try:
        p = subprocess.run(["git", "-C", str(root), "branch", "--show-current"],
                           capture_output=True, text=True, timeout=30)
        name = (p.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        return ""
    return f"M{name}" if name.isdigit() else ""


def cmd_status(rounds: list[Round], stage: str) -> int:
    mine = [r for r in rounds if r.stage == stage]
    if not mine:
        print(f"阶段 {stage}：尚无轮次记录")
        return 0
    for r in mine:
        print(f"  第 {r.n} 轮  [{r.verdict}] {r.focus}  {r.note}")
    passed = sum(1 for r in mine if r.verdict == "PASS")
    print(f"阶段 {stage}：共 {len(mine)} 轮，PASS {passed} 轮")
    return 0


def cmd_gate(rounds: list[Round], stage: str, need: int) -> int:
    mine = [r for r in rounds if r.stage == stage]
    tail = mine[-need:]
    if len(tail) < need:
        print(f"FAIL：阶段 {stage} 仅 {len(mine)} 轮记录，需最近 {need} 轮")
        return 1
    bad = [r for r in tail if r.verdict != "PASS"]
    if bad:
        print(f"FAIL：最近 {need} 轮中第 {bad[0].n} 轮为 FAIL（{bad[0].focus}）")
        return 1
    print(f"通过：阶段 {stage} 最近 {need} 轮全 PASS（轮次 "
          f"{tail[0].n}–{tail[-1].n}）")
    print("下一步：对齐检查 → python tools/ci/check_align.py")
    return 0


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="审查轮次持久化（三轮检查台账）")
    ap.add_argument("action", choices=["status", "record", "gate"])
    ap.add_argument("--stage", default=None, help="阶段标识（如 M0）；缺省由分支名推断")
    ap.add_argument("--focus", default="", help="record：本轮检查重点")
    ap.add_argument("--verdict", default="PASS", choices=["PASS", "FAIL"], help="record：本轮结论")
    ap.add_argument("--note", default="", help="record：命令与产物位置")
    ap.add_argument("--rounds", type=int, default=3, help="gate：需要连续通过的轮数")
    args = ap.parse_args()

    path = root / LOG_REL
    if not path.is_file():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(TEMPLATE, encoding="utf-8")
        print(f"已创建 {LOG_REL}（含空台账模板）")

    stage = args.stage or current_stage(root)
    if not stage:
        print("FAIL：当前分支不是整数阶段分支，请用 --stage 显式指定阶段")
        return 2

    rounds = parse_rounds(_read(path))
    if args.action == "status":
        return cmd_status(rounds, stage)
    if args.action == "gate":
        return cmd_gate(rounds, stage, args.rounds)

    next_n = max((r.n for r in rounds if r.stage == stage), default=0) + 1
    append_round(path, Round(stage, next_n, args.focus, args.verdict, args.note))
    print(f"已记录：阶段 {stage} 第 {next_n} 轮 [{args.verdict}] {args.focus}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
