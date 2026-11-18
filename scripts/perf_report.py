#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PERF 九项对照表生成器（M8 ⑫，SPEC-7 §7.2.2）。

职责：把九项性能标准（设计文档 §3.4 PERF-01~09 三级 S0/S1/S2）与实测采数对齐成
机器可读 + 人可读两份证据物：
  build/evidence/perf_report.json   —— 每项：实测值/来源/达标档位
  build/evidence/perf_report.md     —— 九行对照表（指标 | 实测 | S0 | S1 | S2 | 达标）

口径纪律（docs/testing.md §3 / D7-4）：**只采数不立门禁**——bench 不做硬失败断言，
本脚本永远因"报告生成成功"而退出 0；退出非 0 只发生在用法错误（输入文件不存在、
JSON 解析失败）。达标档位如实标注，"未达标"不是 CI 红，是给人工判读的事实。

用法：
  python scripts/perf_report.py                       # 用默认输入路径跑
  python scripts/perf_report.py --inputs <json路径>    # 指定输入
  python scripts/perf_report.py --template            # 生成输入模板（stdout）

仅标准库，对齐 CI 脚本纪律（tools/ci 同款约束）。"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INPUTS = REPO_ROOT / "build" / "evidence" / "perf_inputs.json"
OUT_DIR = REPO_ROOT / "build" / "evidence"

# ---- 九项标准（设计文档 §3.4，唯一事实源；None = 该档无要求，str = 非数值口径） ----
# direction: higher = 越大越好（吞吐/倍率），lower = 越小越好（延迟/时间/带宽）
STANDARDS = [
    {"id": "PERF-01", "name": "写 QPS（batch=1，fsync=on）", "unit": "ops/s",
     "direction": "higher", "s0": 2000, "s1": 5000, "s2": 10000,
     "method": "tests/bench/write_bench（注意：引擎级采数，设计口径为 3 节点集群写）"},
    {"id": "PERF-02", "name": "批量写加速比（batch=64，group commit）", "unit": "×PERF-01",
     "direction": "higher", "s0": 8, "s1": 15, "s2": 30,
     "method": "bench/write 换 batch 对比"},
    {"id": "PERF-03", "name": "线性一致读 QPS（ReadIndex）", "unit": "×写共识路径",
     "direction": "higher", "s0": 3, "s1": 5, "s2": 10,
     "method": "tests/bench/read_bench（ReadIndex/Log 同语义对比）"},
    {"id": "PERF-04", "name": "LeaseRead 读 QPS", "unit": "×写共识",
     "direction": "higher", "s0": None, "s1": 10, "s2": 20,
     "method": "bench/read（M3，默认关）"},
    {"id": "PERF-05", "name": "写延迟 P99（batch=1，fsync=on）", "unit": "ms",
     "direction": "lower", "s0": 10, "s1": 5, "s2": 3,
     "method": "bench 输出分位数"},
    {"id": "PERF-06", "name": "崩溃恢复时间（kill -9 @ 1GB raft log）", "unit": "s",
     "direction": "lower", "s0": 30, "s1": 5, "s2": 2,
     "method": "bench/recovery 计时（M8 ⑬ scale_recovery_test）"},
    {"id": "PERF-07", "name": "Split 完成时间（1M key 均匀分布）", "unit": "s",
     "direction": "lower", "s0": 60, "s1": 30, "s2": 15,
     "method": "bench/split 计时（M8 ⑬ scale_split_bench）"},
    {"id": "PERF-08", "name": "空载心跳带宽", "unit": "B/s/peer",
     "direction": "lower", "s0": 1024 * 1024, "s1": 100 * 1024, "s2": 10 * 1024,
     "method": "tests/bench/heartbeat_bandwidth_bench"},
    {"id": "PERF-09", "name": "快照传输吞吐", "unit": "MB/s",
     "direction": "higher", "s0": 10, "s1": 50, "s2": None,  # S2 = 链路极限，非数值
     "method": "tests/bench/snapshot_bench"},
]


def fmt_threshold(value) -> str:
    """标准值 → 表格文本（None = 无要求，特殊口径原样显示）。"""
    if value is None:
        return "—"
    if isinstance(value, str):
        return value
    return f"{value:,}"


def attainment(metric: dict, std: dict) -> str:
    """实测值 → 达标档位（从高到低判；无值/非数值如实标注，不粉饰）。"""
    value = metric.get("value")
    if value is None:
        return "未采数"
    if not isinstance(value, (int, float)):
        return "非数值（人工判读）"
    direction = std["direction"]
    for tier in ("s2", "s1", "s0"):
        bound = std[tier]
        if bound is None or isinstance(bound, str):
            continue  # 非数值档不参与自动判定（如 PERF-09 S2 链路极限）
        if direction == "higher" and value >= bound:
            return tier.upper()
        if direction == "lower" and value <= bound:
            return tier.upper()
    return "未达标"


def build_report(metrics: dict) -> dict:
    """九项标准 × 实测 → 结构化报告（json 直写，md 由 render 引用）。"""
    rows = []
    for std in STANDARDS:
        metric = metrics.get(std["id"], {})
        rows.append({
            "id": std["id"],
            "name": std["name"],
            "unit": std["unit"],
            "value": metric.get("value"),
            "attainment": attainment(metric, std),
            "source": metric.get("source", ""),
            "measured_at": metric.get("date", ""),
            "note": metric.get("note", ""),
        })
    return {"generated_at": date.today().isoformat(),
            "criteria": "设计文档 §3.4（S0 交付底线 / S1 里程碑 / S2 挑战）",
            "policy": "只采数不立门禁（testing.md §3 / D7-4）；达标档位仅供人工判读",
            "rows": rows}


def render_markdown(report: dict) -> str:
    """结构化报告 → 九行对照表（md）。"""
    lines = [
        "# PERF 九项对照表（M8 ⑫ 证据物）",
        "",
        f"> 生成：{report['generated_at']}；口径：{report['criteria']}；",
        f"> {report['policy']}。",
        "",
        "| 指标 | 实测 | S0 | S1 | S2 | 达标档位 | 来源 |",
        "|---|---|---|---|---|---|---|",
    ]
    for std, row in zip(STANDARDS, report["rows"]):
        value = "未采数" if row["value"] is None else f"{row['value']:,}"
        lines.append(
            f"| {row['id']} {row['name']}（{row['unit']}） | {value} "
            f"| {fmt_threshold(std['s0'])} | {fmt_threshold(std['s1'])} "
            f"| {fmt_threshold(std['s2'])} | {row['attainment']} | {row['source']} |")
    lines.append("")
    notes = [f"- {row['id']}：{row['note']}" for row in report["rows"] if row["note"]]
    if notes:
        lines.append("## 判读备注")
        lines.append("")
        lines.extend(notes)
    return "\n".join(lines) + "\n"


def template() -> str:
    """输入文件模板：每项一个 value + source，采数后填入再跑。"""
    sample = {
        "metrics": {
            std["id"]: {"value": None, "source": "", "date": "", "note": ""}
            for std in STANDARDS
        }
    }
    sample["metrics"]["PERF-08"] = {
        "value": 280, "source": "tests/bench/heartbeat_bandwidth_bench（示例）",
        "date": "2026-11-18", "note": "每对端 28B×10HB/s 外推"}
    return json.dumps(sample, ensure_ascii=False, indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="PERF 九项对照表生成器")
    parser.add_argument("--inputs", type=Path, default=DEFAULT_INPUTS,
                        help="实测采数 JSON（默认 build/evidence/perf_inputs.json）")
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR)
    parser.add_argument("--template", action="store_true", help="打印输入模板后退出")
    args = parser.parse_args()
    if args.template:
        sys.stdout.write(template())
        return 0
    if not args.inputs.is_file():
        print(f"输入不存在：{args.inputs}\n先用 --template 生成模板，采数后重跑。", file=sys.stderr)
        return 2
    try:
        metrics = json.loads(args.inputs.read_text(encoding="utf-8-sig"))["metrics"]
    except (json.JSONDecodeError, KeyError) as exc:
        print(f"输入格式错误（需 {{\"metrics\": {{...}}}}）：{exc}", file=sys.stderr)
        return 2
    report = build_report(metrics)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "perf_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.out_dir / "perf_report.md").write_text(
        render_markdown(report), encoding="utf-8")
    filled = sum(1 for r in report["rows"] if r["value"] is not None)
    print(f"perf_report 已生成（{args.out_dir}）：九项中 {filled} 项有实测")
    for row in report["rows"]:
        value = "未采数" if row["value"] is None else row["value"]
        print(f"  {row['id']}: {value} → {row['attainment']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
