#!/usr/bin/env python3
# 文件用途：构建产物与日志的分级清理 + 体积门禁（build/ 曾膨胀到 4GB 的收口工具）。
# 归属：TinyKV-CPP（scripts/ 工程脚本，不在 code 阶段静态检查的 arm_globs 范围内）。
#
# 默认只报告不删除；删除必须显式 --apply。
#
# 用法：
#   python scripts/clean.py                       # 报告各组占用与可回收空间
#   python scripts/clean.py --check               # 体积门禁：超预算返回 1
#   python scripts/clean.py --apply --only logs   # 只清日志（零代价，随时可跑）
#   python scripts/clean.py --apply --only build  # 清构建树（下次全量重编）
#   python scripts/clean.py --apply --all         # 全清（含 .deps，需重新拉依赖）
#
# 分区口径：
#   logs  —— 运行时日志（根 log/、logs/、*.log）与构建日志（build/logs/、build 下 *.log）
#   build —— 构建树 build/<preset>、build/live/bin、CMake 临时文件
#   cache —— Python 缓存（__pycache__、*.pyc、.pytest_cache）
#   deps  —— build/.deps（FetchContent 三方源码，删后需重新拉取）
#
# 永不清除：build/evidence（CI 证据链根，SPEC-0 §0.7）、build/.deps（除非 --only deps）。

from __future__ import annotations

import argparse
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"

# 体积预算（MB）：超限即提示 / --check 返回 1
# 口径依据：单棵 -g 构建树（73 个 exe、完整调试信息）实测约 2.0GB；
# 再加上第二棵树才是异常（曾出现 4.0GB），故预算卡在"单树 + 余量"。
BUDGET_BUILD_MB = 2500
BUDGET_LOGS_MB = 200

PROTECTED = ("build/evidence",)


@dataclass
class Group:
    name: str
    desc: str
    paths: list[Path] = field(default_factory=list)

    def size(self) -> int:
        total = 0
        for p in self.paths:
            if p.is_file():
                total += p.stat().st_size
            elif p.is_dir():
                for f in p.rglob("*"):
                    try:
                        if f.is_file():
                            total += f.stat().st_size
                    except OSError:
                        pass
        return total


def rel(p: Path) -> str:
    try:
        return p.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(p)


def protected(p: Path) -> bool:
    r = rel(p)
    return any(r == x or r.startswith(x + "/") for x in PROTECTED)


def group_logs() -> Group:
    g = Group("logs", "运行时日志 + 构建日志")
    for name in ("log", "logs", "build/logs"):
        p = REPO_ROOT / name
        if p.is_dir() and not protected(p):
            g.paths.append(p)
    g.paths.extend(REPO_ROOT.glob("*.log"))
    if BUILD_DIR.is_dir():
        for p in BUILD_DIR.rglob("*.log"):
            if not protected(p) and p.is_file():
                g.paths.append(p)
    return g


def group_build() -> Group:
    g = Group("build", "构建树 / 可执行体 / CMake 临时文件")
    if not BUILD_DIR.is_dir():
        return g
    for child in BUILD_DIR.iterdir():
        if child.name in (".deps", "evidence", "logs"):
            continue
        if child.name == "live":
            binp = child / "bin"
            if binp.is_dir():
                g.paths.append(binp)
            continue
        g.paths.append(child)
    for name in ("build.ninja", "compile_commands.json", "CMakeCache.txt"):
        p = REPO_ROOT / name
        if p.exists():
            g.paths.append(p)
    return g


def group_cache() -> Group:
    g = Group("cache", "Python / 工具缓存")
    for p in REPO_ROOT.rglob("__pycache__"):
        if ".deps" not in rel(p):
            g.paths.append(p)
    for p in REPO_ROOT.rglob("*.pyc"):
        if ".deps" not in rel(p) and p.parent.name != "__pycache__":
            g.paths.append(p)
    cache = REPO_ROOT / ".pytest_cache"
    if cache.is_dir():
        g.paths.append(cache)
    return g


def group_deps() -> Group:
    g = Group("deps", "FetchContent 三方源码（删后需重新拉取）")
    p = BUILD_DIR / ".deps"
    if p.is_dir():
        g.paths.append(p)
    return g


BUILDERS = {"logs": group_logs, "build": group_build, "cache": group_cache, "deps": group_deps}


def collect(names: list[str]) -> list[Group]:
    groups = []
    for name in names:
        g = BUILDERS[name]()
        uniq: list[Path] = []
        for p in sorted(g.paths, key=lambda x: len(str(x))):
            if not any(u == p or u in p.parents for u in uniq):
                uniq.append(p)
        g.paths = uniq
        groups.append(g)
    return groups


def human(n: int) -> str:
    return f"{n / 1048576:,.1f} MB"


def print_report(groups: list[Group]) -> None:
    print(f"仓库：{REPO_ROOT}")
    print(f"{'分区':<7}{'占用':>12}  说明")
    print("-" * 68)
    for g in groups:
        print(f"{g.name:<7}{human(g.size()):>12}  {g.desc}")
        for p in g.paths:
            print(f"        - {rel(p)}")
    total = sum(g.size() for g in groups)
    print("-" * 68)
    print(f"{'合计':<7}{human(total):>12}")


def do_check(groups: list[Group]) -> int:
    size_of = {g.name: g.size() for g in groups}
    failed = False
    build_mb = size_of.get("build", 0) / 1048576
    logs_mb = size_of.get("logs", 0) / 1048576
    if build_mb > BUDGET_BUILD_MB:
        print(f"[超预算] 构建树 {build_mb:,.0f} MB > {BUDGET_BUILD_MB} MB —— 跑 --apply --only build")
        failed = True
    if logs_mb > BUDGET_LOGS_MB:
        print(f"[超预算] 日志 {logs_mb:,.0f} MB > {BUDGET_LOGS_MB} MB —— 跑 --apply --only logs")
        failed = True
    if not failed:
        print(f"[通过] 构建树 {build_mb:,.0f} MB / 日志 {logs_mb:,.0f} MB 均在预算内")
    return 1 if failed else 0


def do_apply(groups: list[Group]) -> int:
    removed = 0
    for g in groups:
        for p in g.paths:
            if protected(p):
                print(f"跳过（受保护）：{rel(p)}")
                continue
            try:
                if p.is_dir():
                    shutil.rmtree(p)
                elif p.is_file():
                    p.unlink()
                print(f"已删除：{rel(p)}")
            except OSError as exc:
                print(f"删除失败：{rel(p)} —— {exc}")
                return 1
        removed += g.size() if False else 0  # 删除后 size() 归零，故下面单独统计
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="TinyKV 构建产物 / 日志清理与体积门禁")
    ap.add_argument("--only", choices=sorted(BUILDERS), help="只处理指定分区")
    ap.add_argument("--all", action="store_true", help="处理全部分区（含 .deps）")
    ap.add_argument("--apply", action="store_true", help="真正删除；不带此参数只报告")
    ap.add_argument("--check", action="store_true", help="体积门禁模式：超预算返回 1")
    args = ap.parse_args()

    if args.all:
        names = ["logs", "build", "cache", "deps"]
    elif args.only:
        names = [args.only]
    else:
        names = ["logs", "build", "cache"]

    groups = collect(names)
    before = sum(g.size() for g in groups)
    print_report(groups)
    print()

    if args.check:
        return do_check(groups)
    if not args.apply:
        print(f"以上为报告模式（未删除任何内容），可回收约 {human(before)}。")
        print("确认后加 --apply；先清日志用：--apply --only logs")
        return 0

    print(f"开始删除，预计回收 {human(before)} ……")
    rc = do_apply(groups)
    if rc == 0:
        remain = sum(g.size() for g in collect(names))
        print(f"完成：回收 {human(before - remain)}，剩余 {human(remain)}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
