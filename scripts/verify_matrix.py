#!/usr/bin/env python3
# 文件用途：多标准/多编译器兼容矩阵——"编得动、测得过就删"，只留 C++23 默认构建树。
# 归属：TinyKV-CPP（scripts/ 工程脚本，不在 code 阶段静态检查的 arm_globs 范围内）。
#
# 背景（2026-11-18）：一份源码要同时满足 C++17/20/23 与 g++/clang++，兼容验证必然要
# 多棵构建树；但每棵树 1.3~2.5GB，验证完不删就会累积（曾到 13GB）。
# 本脚本把"建 → 测 → 删"收敛成一条命令：默认只保留 build/win-debug（g++ · C++23）。
#
# 用法：
#   python scripts/verify_matrix.py                  # 全矩阵 17/23 × g++/clang++，测完删非 23 树
#   python scripts/verify_matrix.py --std 17         # 只验 17 档
#   python scripts/verify_matrix.py --label unit     # 只跑 unit（默认 fast = unit+simulation）
#   python scripts/verify_matrix.py --keep           # 保留本次建出的树（排查兼容问题时用）
#   python scripts/verify_matrix.py --clean-only     # 不构建，只清历史遗留矩阵树
#
# 树命名：g++ → build/cxx<标准>；clang++ → build/clang<标准>；
#        g++ · C++23 直接复用默认预设 win-debug 的 build/win-debug，不再单开一棵。
# 证据：每次跑完写 build/evidence/matrix.json（各组合 build/ctest 结果与产物体积）。

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
EVIDENCE = BUILD_DIR / "evidence" / "matrix.json"
DEFAULT_TREE = "win-debug"  # 唯一常驻树：g++ · C++23

# 矩阵树命名：(编译器, 标准) → 目录名；None 表示复用默认预设
MATRIX = {
    ("gcc", "23"): None,
    ("gcc", "17"): "cxx17",
    ("clang", "23"): "clang23",
    ("clang", "17"): "clang17",
}

COMPILER_BIN = {"gcc": ("gcc", "g++"), "clang": ("clang", "clang++")}


def tree_path(name: str | None) -> Path:
    return BUILD_DIR / (name or DEFAULT_TREE)


def tree_size(path: Path) -> int:
    if not path.is_dir():
        return 0
    total = 0
    for f in path.rglob("*"):
        try:
            if f.is_file():
                total += f.stat().st_size
        except OSError:
            pass
    return total


def recently_active(path: Path, seconds: int = 120) -> bool:
    """构建中判定：ninja 日志 / 构建文件在近 N 秒内被写过，就认为有人在用这棵树。

    不做进程枚举（Windows 下要外部依赖），用写入痕迹判断更稳且跨平台。
    """
    marks = [path / ".ninja_log", path / "build.ninja"]
    now = time.time()
    for m in marks:
        try:
            if m.is_file() and now - m.stat().st_mtime < seconds:
                return True
        except OSError:
            pass
    return False


def run(cmd: list[str], env: dict[str, str] | None = None) -> tuple[int, str]:
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    try:
        proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True,
                              errors="replace", env=full_env)
    except FileNotFoundError as exc:
        return 127, f"找不到命令 {exc.filename}"
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def configure(cc: str, std: str, name: str | None) -> tuple[int, str]:
    if name is None:
        # 默认树走预设，保证与日常构建完全同参数
        return run(["cmake", "--preset", "win-debug"])
    c, cxx = COMPILER_BIN[cc]
    return run(["cmake", "-S", str(REPO_ROOT), "-B", str(BUILD_DIR / name), "-G", "Ninja",
                f"-DCMAKE_BUILD_TYPE=Debug", f"-DTINYKV_CXX_STANDARD={std}",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], env={"CC": c, "CXX": cxx})


def build(name: str | None) -> tuple[int, str]:
    target = tree_path(name)
    return run(["cmake", "--build", str(target)])


def test(name: str | None, label: str) -> tuple[int, str]:
    cmd = ["ctest", "--test-dir", str(tree_path(name)), "--output-on-failure"]
    if label:
        cmd += ["-L", label]
    return run(cmd)


def purge_matrix_trees(keep: set[str]) -> list[tuple[str, int]]:
    """删掉所有矩阵树（除 keep 与默认树外），返回 [(目录名, 回收字节)]。"""
    removed = []
    if not BUILD_DIR.is_dir():
        return removed
    for child in BUILD_DIR.iterdir():
        if not child.is_dir():
            continue
        name = child.name
        if name.startswith(".") or name in keep or name == DEFAULT_TREE:
            continue
        # 只认矩阵命名，避免误删 live / evidence / parking 等口径容器
        if not (name.startswith("cxx") or name.startswith("clang") or name.startswith("gcc")):
            continue
        if recently_active(child):
            print(f"跳过 {name}：刚有构建写入（疑似其他会话在用），稍后重跑即可")
            continue
        size = tree_size(child)
        shutil.rmtree(child, ignore_errors=True)
        removed.append((name, size))
    return removed


def human(n: int) -> str:
    return f"{n / 1048576:,.0f} MB" if n >= 1048576 else f"{n / 1024:.0f} KB"


def main() -> int:
    ap = argparse.ArgumentParser(description="C++ 标准 / 编译器兼容矩阵：建 → 测 → 删")
    ap.add_argument("--std", choices=["17", "20", "23"], action="append",
                    help="只验指定标准（可重复）；默认 17 与 23")
    ap.add_argument("--cc", choices=["gcc", "clang"], action="append",
                    help="只验指定编译器（可重复）；默认两者")
    ap.add_argument("--label", default="fast", help="ctest 标签过滤（默认 fast；空串=全部）")
    ap.add_argument("--keep", action="store_true", help="保留本次建出的树，不删")
    ap.add_argument("--clean-only", action="store_true", help="不构建，只清历史遗留矩阵树")
    ap.add_argument("--skip", action="append", default=[],
                    help="清树时跳过指定目录名（可重复），用于避开其他会话正在构建的树")
    args = ap.parse_args()

    stds = args.std or ["17", "23"]
    ccs = args.cc or ["gcc", "clang"]
    wanted = [(cc, s) for (cc, s) in MATRIX if cc in ccs and s in stds]

    if args.clean_only:
        removed = purge_matrix_trees(keep=set(args.skip))
        total = sum(sz for _, sz in removed)
        for name, sz in removed:
            print(f"已删除 {name:<12}{human(sz)}")
        print(f"共回收 {human(total)}；保留 {DEFAULT_TREE}"
              + (f" 与 {', '.join(args.skip)}" if args.skip else ""))
        return 0

    results = []
    keep: set[str] = set()
    t0 = time.time()
    for cc, std in wanted:
        name = MATRIX[(cc, std)]
        tree = tree_path(name)
        label = f"{cc} · C++{std}"
        print(f"\n=== {label}（{tree.relative_to(REPO_ROOT).as_posix()}）===", flush=True)

        rc, out = configure(cc, std, name)
        if rc != 0:
            print(out[-1200:])
            results.append({"combo": label, "stage": "configure", "ok": False})
            return 1
        rc, out = build(name)
        if rc != 0:
            print(out[-2000:])
            results.append({"combo": label, "stage": "build", "ok": False})
            return 1
        rc, out = test(name, args.label)
        tail = "\n".join(out.strip().splitlines()[-4:])
        print(tail)
        if rc != 0:
            results.append({"combo": label, "stage": "test", "ok": False})
            return 1
        results.append({"combo": label, "stage": "test", "ok": True,
                        "size_mb": round(tree_size(tree) / 1048576, 1)})
        if name is not None:
            keep.add(name)

    elapsed = time.time() - t0
    EVIDENCE.parent.mkdir(parents=True, exist_ok=True)
    EVIDENCE.write_text(json.dumps({
        "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "label": args.label,
        "elapsed_s": round(elapsed, 1),
        "results": results,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n矩阵全绿：{len(results)} 个组合，用时 {elapsed:.0f}s；证据 {EVIDENCE.relative_to(REPO_ROOT)}")

    if args.keep:
        print(f"按要求保留本次树：{', '.join(sorted(keep))}")
        return 0

    removed = purge_matrix_trees(keep=keep - keep)  # 一个都不留
    total = sum(sz for _, sz in removed)
    for name, sz in removed:
        print(f"已删除 {name:<12}{human(sz)}")
    print(f"回收 {human(total)}；仅保留 {DEFAULT_TREE}（g++ · C++23 默认构建）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
