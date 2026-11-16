#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""align 阶段检查器：跨语言契约 / 文档命令 / 违禁模式。

对应 SPEC.md §2 表中"可判定"一侧的三类偏差，全部只做可判定比对：

    contract  src/common/error.h 的 KvError 变体集合 ⇄ proto 中的线上错误消息集合
    doc_cmd   Markdown 围栏块里引用的 preset / test-dir / ctest 标签必须真实存在
    banned    src/ 与 tests/ 的违禁模式（ENG-04：detach / 裸数组 new / sleep 对齐启动）

判定口径（与 rules.toml [align] 一一对应）：
  * 缺失但已在 contract_exempt 声明的 → 可见列出，不拦截（刻意不对称不静默）；
  * 未声明的缺失、proto 侧多出的错误类型 → FAIL；
  * 失效豁免（声明的变体已不存在）→ FAIL，迫使清理豁免清单（规则只严不松）；
  * 违禁词出现在注释内不计——禁用规则本身必须能写出这些词。

单独运行：
    python tools/ci/check_align.py                 # 三项全跑
    python tools/ci/check_align.py --only banned   # 只跑违禁扫描（CI workflow 独立步骤用）
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_ci as L  # noqa: E402

CPP_EXT = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx"}
BANNED_SCAN_DIRS = ("src", "tests")
FENCED_BLOCK_RE = re.compile(r"```[^\n]*\n(.*?)```", re.S)
KV_ERROR_RE = re.compile(r"using\s+KvError\s*=\s*std::variant<([^;]*?)>;", re.S)
# 占位符/变量：出现在命令里说明是模板而非可直接执行的命令，跳过
PLACEHOLDER_RE = re.compile(r"[<>$…]|PRESET|\.\.\.")


def _clean_token(tok: str) -> str:
    """去掉引号与行尾续行符，便于与真实名字比较。"""
    return tok.strip().strip("\"'`").rstrip("\\").strip()


def _is_placeholder(tok: str) -> bool:
    return bool(PLACEHOLDER_RE.search(tok))


# ---- contract：C++ 错误变体 ⇄ proto 线上错误消息 -----------------------------
def _kv_variants(root: Path) -> tuple[list[str], str]:
    """从 error.h 提取 KvError 的变体名（保持声明顺序）。"""
    rel = "src/common/error.h"
    src = L.read_text(root / rel)
    m = KV_ERROR_RE.search(src)
    if not m:
        return [], f"{rel}: 未找到 `using KvError = std::variant<...>` 声明"
    names = [x.strip() for x in m.group(1).split(",")]
    return [n for n in names if n], ""


def _proto_error_messages(root: Path) -> tuple[set[str], list[str]]:
    """收集 proto 中 RegionError 内定义的错误消息名。"""
    found: set[str] = set()
    sources: list[str] = []
    for p in sorted((root / "proto").glob("*.proto")):
        text = L.read_text(p)
        m = re.search(r"message\s+RegionError\s*\{(.*?)\n\}", text, re.S)
        if not m:
            continue
        sources.append(L.posix(p.relative_to(root)))
        body = m.group(1)
        found.update(re.findall(r"^\s*message\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", body, re.M))
    return found, sources


def check_contract(root: Path, rules: dict) -> L.CheckResult:
    res = L.CheckResult("contract", L.PASS)
    cfg = rules.get("align", {})
    exempt = set(map(str, cfg.get("contract_exempt", [])))

    cpp, err = _kv_variants(root)
    if err:
        res.status = L.FAIL
        res.add("ALIGN-CONTRACT", err)
        return res
    proto, sources = _proto_error_messages(root)
    if not proto:
        res.status = L.FAIL
        res.add("ALIGN-CONTRACT", "proto 中未找到 RegionError 的错误消息定义")
        return res

    cpp_set = set(cpp)
    missing = [n for n in cpp if n not in proto]          # C++ 有、线上无
    extra = sorted(proto - cpp_set)                        # 线上有、C++ 无
    stale = sorted(exempt - cpp_set)                       # 豁免了但变体已不存在

    for name in missing:
        if name in exempt:
            res.add("ALIGN-EXEMPT", f"{name}：C++ 变体未上线 proto（已声明刻意不对称）")
        else:
            res.status = L.FAIL
            res.add("ALIGN-MISSING", f"{name}：C++ 有该错误变体但 proto 未定义对应消息"
                                      f"（客户端无法得知该失败类别）")
    for name in extra:
        res.status = L.FAIL
        res.add("ALIGN-EXTRA", f"{name}：proto 定义了该错误消息但 KvError 无对应变体")
    for name in stale:
        res.status = L.FAIL
        res.add("ALIGN-STALE", f"{name}：contract_exempt 中的豁免已失效（变体不存在），请删除该条")

    if res.status == L.PASS:
        covered = len(proto & cpp_set)
        res.summary = (f"KvError {len(cpp)} 变体 ⇄ proto {len(proto)} 消息："
                       f"已上线 {covered}、声明豁免 {len(exempt & set(missing))}"
                       f"（{L.posix(sources[0]) if sources else '-'}）")
    return res


# ---- doc_cmd：文档命令引用的对象必须真实存在 ---------------------------------
def _preset_names(root: Path) -> tuple[set[str], set[str]]:
    path = root / "CMakePresets.json"
    if not path.is_file():
        return set(), set()
    data = json.loads(L.read_text(path))
    configure = {p["name"] for p in data.get("configurePresets", [])
                 if p.get("name") and not p.get("hidden")}
    build = {p["name"] for p in data.get("buildPresets", []) if p.get("name")}
    return configure, build


def _iter_commands(text: str):
    """产出 (绝对行号, 命令行)：围栏代码块逐行 + 块外的行内代码 span。

    两层都要扫——命令既可能写在 ``` 块里（操作手册风格），也可能写在表格的
    行内反引号里（命令表风格）；只扫一种会漏掉一半引用点。
    """
    for m in FENCED_BLOCK_RE.finditer(text):
        base = text.count("\n", 0, m.start()) + 2
        for offset, line in enumerate(m.group(1).splitlines()):
            yield base + offset, line

    outside = FENCED_BLOCK_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    for line_no, line in enumerate(outside.splitlines(), start=1):
        for span in re.findall(r"`([^`\n]+)`", line):
            yield line_no, span


def check_doc_cmd(root: Path, rules: dict) -> L.CheckResult:
    res = L.CheckResult("doc_cmd", L.PASS)
    cfg = rules.get("align", {})
    labels = set(map(str, cfg.get("ctest_labels", [])))
    configure, build = _preset_names(root)
    if not configure:
        res.status = L.SKIP
        res.summary = "SKIP(NO_PRESETS)：未找到 CMakePresets.json，无法校验 preset 引用"
        return res
    test_dirs = {L.posix(rules["code"]["build_dir"])} | {f"build/{n}" for n in configure}

    checked = 0
    shorthand = 0
    for path, rel in L.iter_md_files(root, rules["doc_budget"].get("reference_freeze", [])):
        for line_no, line in _iter_commands(L.read_text(path)):
            where = f"{rel}:{line_no}"
            for m in re.finditer(r"cmake\s+(--build\s+)?--preset\s+(\S+)", line):
                tok = _clean_token(m.group(2))
                if _is_placeholder(tok):
                    continue
                checked += 1
                known = build if m.group(1) else configure
                if tok not in known:
                    res.status = L.FAIL
                    res.add("ALIGN-PRESET", f"{where} → preset '{tok}' 不存在于 CMakePresets.json")
            for m in re.finditer(r"--test-dir\s+(\S+)", line):
                tok = _clean_token(m.group(1))
                if _is_placeholder(tok):
                    continue
                checked += 1
                if tok not in test_dirs:
                    res.status = L.FAIL
                    res.add("ALIGN-TESTDIR",
                            f"{where} → test-dir '{tok}' 不可用（应为 build/<preset>）")
            for m in re.finditer(r"(?<!\S)-L\s+(\S+)", line):
                tok = _clean_token(m.group(1))
                if _is_placeholder(tok):
                    continue
                if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", tok):
                    # 组合记法（如 `fast/-L slow`、`unit|simulation`）：非单一标签，不做存在性判定
                    shorthand += 1
                    continue
                checked += 1
                if tok not in labels:
                    res.status = L.FAIL
                    res.add("ALIGN-LABEL", f"{where} → ctest 标签 '{tok}' 不在受控词表内")

    if shorthand:
        res.add("ALIGN-NOTE", f"另有 {shorthand} 处 ctest 标签为组合记法，未做存在性判定")
    if res.status == L.PASS:
        res.summary = f"文档命令引用 {checked} 处全部可解析（preset/test-dir/label）"
    return res


# ---- banned：违禁模式扫描（ENG-04） ------------------------------------------
def _strip_comments(text: str) -> list[str]:
    """去块注释与行注释后按行返回：禁用规则本身要能写出这些词，注释不应命中考。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return [re.sub(r"//.*$", "", line) for line in text.splitlines()]


def check_banned(root: Path, rules: dict) -> L.CheckResult:
    res = L.CheckResult("banned", L.PASS)
    cfg = rules.get("align", {})
    patterns = [re.compile(p) for p in cfg.get("banned_patterns", [])]
    exempt = list(map(str, cfg.get("banned_exempt", [])))
    if not patterns:
        res.status = L.SKIP
        res.summary = "SKIP(NO_RULES)：rules.toml [align].banned_patterns 为空"
        return res

    scanned = 0
    for top in BANNED_SCAN_DIRS:
        base = root / top
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix.lower() not in CPP_EXT:
                continue
            rel = L.posix(p.relative_to(root))
            if L.glob_match(rel, exempt):
                res.add("BAN-EXEMPT", f"{rel}：已声明豁免")
                continue
            scanned += 1
            for line_no, line in enumerate(_strip_comments(L.read_text(p)), start=1):
                for rx in patterns:
                    m = rx.search(line)
                    if m:
                        res.status = L.FAIL
                        res.add("BAN-HIT", f"{rel}:{line_no} → 命中 '{m.group(0).strip()}'（ENG-04）")
    if res.status == L.PASS:
        res.summary = f"扫描 {scanned} 个源文件，违禁模式 0 命中（{len(patterns)} 条规则）"
    return res


# ---- 入口 -------------------------------------------------------------------
CHECKS = {
    "contract": check_contract,
    "doc_cmd": check_doc_cmd,
    "banned": check_banned,
}


def run(root: Path, rules: dict, only: list[str] | None = None) -> list[L.CheckResult]:
    names = only or list(CHECKS)
    return [CHECKS[n](root, rules) for n in names]


def main() -> int:
    L.setup_output()
    ap = argparse.ArgumentParser(description="TinyKV-CI align 阶段检查（契约/文档命令/违禁模式）")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent.parent))
    ap.add_argument("--only", choices=list(CHECKS), action="append",
                    help="只跑指定检查，可重复；缺省全跑")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    rules = L.load_rules(root)
    results = run(root, rules, args.only)
    L.out("== align 阶段 ==")
    for r in results:
        L.out(f"[{r.status}] {r.check}: {r.summary}")
        for f in r.findings:
            L.out(f"    {f.code}  {f.message}")
    return L.aggregate_exit(results)


if __name__ == "__main__":
    sys.exit(main())
