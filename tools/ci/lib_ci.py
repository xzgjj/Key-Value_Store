#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyKV-CI 公共库：结果模型、git 封装、字数统计、文本规整。

纯 Python 3 标准库（>=3.11，用到 tomllib）。所有检查器（tools/ci/check_*.py）
共用一套结果语义（见 SPEC.md）：

    PASS      通过
    FAIL      可判定问题，自动拦截（退出码 1）
    SKIP      未武装/环境不具备，报告中可见列出，不算绿也不算红
    APPROVAL  需人工批准的改动（退出码 2）
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---- 结果语义 --------------------------------------------------------------
PASS, FAIL, SKIP, APPROVAL = "PASS", "FAIL", "SKIP", "APPROVAL"
EXIT_OK, EXIT_FAIL, EXIT_APPROVAL = 0, 1, 2

# ---- 文本工具 --------------------------------------------------------------
CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")
WORD_RE = re.compile(r"[A-Za-z0-9]+")
FENCED_RE = re.compile(r"```.*?```", re.S)
MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+[\"'][^)]*[\"'])?\)")
TOOL_REF_RE = re.compile(r"\btools/[A-Za-z0-9_\-./]+\.py\b")
# 规范化时剔除的标点（中英文常见标点 + markdown 语法字符）
PUNCT_RE = re.compile(
    r"[\s，。、；：？！“”‘’（）《》〈〉【】·．…—～[,.;:!?()<>{}\[\]\"'`~*_#|+=/\\-]+"
)
BINARY_EXT = {
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".webp", ".pdf",
    ".zip", ".7z", ".gz", ".tar", ".pdb", ".lib", ".a", ".so", ".dll", ".exe",
    ".pyc",
}


def setup_output() -> None:
    """Windows 控制台默认 GBK，统一改成 UTF-8 输出。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:  # 非文本流
            pass


def out(msg: str = "") -> None:
    print(msg, flush=True)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig", errors="replace")


def posix(rel) -> str:
    """统一路径分隔符；接受 str 或 Path。"""
    return str(rel).replace("\\", "/")


def _glob_to_regex(pat: str) -> str:
    """glob → 正则：'**' 跨目录段（'/a/**/b' 可匹配 '/a/b'），'*' 不跨 '/'。"""
    out: list[str] = []
    i, n = 0, len(pat)
    while i < n:
        if pat.startswith("/**", i) and (i + 3 == n or pat[i + 3] == "/"):
            out.append("(?:/.*)?")            # '/**' 结尾或 '/**/'：零或多层目录
            i += 3
            if i < n and pat[i] == "/":
                i += 1
            continue
        if pat.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
            continue
        c = pat[i]
        if c == "*":
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return "^" + "".join(out) + "$"


_GLOB_CACHE: dict[str, re.Pattern] = {}


def glob_match(rel: str, patterns: list[str]) -> bool:
    """支持 '**'（跨目录）、'*'（单段）、'dir/'（目录前缀）。"""
    rel_n = posix(rel)
    for p in patterns:
        p_n = posix(p)
        if p_n.endswith("/"):
            if rel_n == p_n.rstrip("/") or rel_n.startswith(p_n):
                return True
        rx = _GLOB_CACHE.get(p_n)
        if rx is None:
            rx = re.compile(_glob_to_regex(p_n))
            _GLOB_CACHE[p_n] = rx
        if rx.match(rel_n):
            return True
    return False


def iter_md_files(root: Path, exclude_globs: list[str] | None = None):
    """按序产出 (绝对路径, 相对posix路径)，跳过点目录（.git/.idea/.zcode…）与排除项。"""
    exclude_globs = exclude_globs or []
    for p in sorted(root.rglob("*.md")):
        rel_parts = p.relative_to(root).parts
        if any(part.startswith(".") for part in rel_parts[:-1]):
            continue
        rel = posix(p.relative_to(root))
        if glob_match(rel, exclude_globs):
            continue
        yield p, rel


def strip_fenced(text: str) -> str:
    return FENCED_RE.sub("", text)


def doc_word_count(text: str) -> int:
    """字数口径：CJK 字=1、拉丁/数字词=1；围栏代码块不计（规则见 rules.toml）。"""
    text = strip_fenced(text)
    return len(CJK_RE.findall(text)) + len(WORD_RE.findall(text))


def normalize_para(text: str) -> str:
    return PUNCT_RE.sub("", text).lower()


def relative_md_links(text: str) -> list[str]:
    """markdown 相对链接目标（剔除 http/mailto/纯锚点）。"""
    targets = []
    for m in MD_LINK_RE.findall(text):
        if m.startswith(("http://", "https://", "mailto:", "#")):
            continue
        targets.append(m)
    return targets


def tool_refs(text: str) -> list[str]:
    """正文中引用的 tools/*.py 脚本路径。"""
    return TOOL_REF_RE.findall(text)


# ---- 结果模型 --------------------------------------------------------------
@dataclass
class Finding:
    code: str
    message: str


@dataclass
class CheckResult:
    check: str
    status: str = PASS
    summary: str = ""
    findings: list[Finding] = field(default_factory=list)
    detail: str = ""          # 可选的整段补充输出（如子脚本 stdout）

    def add(self, code: str, message: str) -> None:
        self.findings.append(Finding(code, message))


def aggregate_exit(results: list[CheckResult]) -> int:
    """聚合退出码：任一 FAIL → 1；否则任一 APPROVAL → 2；否则 0。"""
    statuses = {r.status for r in results}
    if FAIL in statuses:
        return EXIT_FAIL
    if APPROVAL in statuses:
        return EXIT_APPROVAL
    return EXIT_OK


# ---- git 封装 --------------------------------------------------------------
def git(root: Path, *args: str) -> tuple[int, str]:
    """在工作区 root 上执行 git 命令，返回 (returncode, 合并的 stdout+stderr)。"""
    try:
        p = subprocess.run(
            ["git", "-C", str(root), "-c", "core.quotepath=false", *args],
            capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120,
        )
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except FileNotFoundError:
        return 127, "git 不可用"
    except subprocess.TimeoutExpired:
        return 124, "git 命令超时"


def has_head(root: Path) -> bool:
    rc, _ = git(root, "rev-parse", "--verify", "-q", "HEAD")
    return rc == 0


def changed_files(root: Path) -> list[tuple[str, str]]:
    """git status --porcelain（含未跟踪），返回 (状态码, posix路径)。"""
    rc, outp = git(root, "status", "--porcelain", "--untracked-files=all")
    entries: list[tuple[str, str]] = []
    if rc != 0:
        return entries
    for line in outp.splitlines():
        if len(line) < 4:
            continue
        code, path = line[:2].strip() or "??", line[3:]
        if path.startswith('"') and path.endswith('"'):
            path = path[1:-1].encode("latin-1", "ignore").decode("unicode_escape")
        entries.append((code, posix(path)))
    return entries


def diff_numstat(root: Path) -> tuple[int, int, list[str]]:
    """HEAD ↔ 工作区（已跟踪文件）的 (新增行, 删除行, 文件清单)。"""
    rc, outp = git(root, "diff", "--numstat", "HEAD")
    added = deleted = 0
    files: list[str] = []
    for line in outp.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3:
            files.append(posix(parts[2]))
            if parts[0] != "-":
                added += int(parts[0])
            if parts[1] != "-":
                deleted += int(parts[1])
    return added, deleted, files


def untracked_added_lines(root: Path, changed: list[tuple[str, str]]) -> int:
    """未跟踪文本文件的行数按全部新增计（判定只认实测；二进制按 0 行计）。"""
    total = 0
    for code, rel in changed:
        if "??" not in code:
            continue
        p = root / rel
        if not p.is_file() or p.suffix.lower() in BINARY_EXT:
            continue
        try:
            with open(p, "rb") as f:
                total += sum(1 for _ in f)
        except OSError:
            pass
    return total


def load_rules(root: Path) -> dict:
    import tomllib
    with open(root / "ci" / "rules.toml", "rb") as f:
        return tomllib.load(f)


def filter_ignored(entries: list[tuple[str, str]], rules: dict) -> list[tuple[str, str]]:
    """按 rules.toml [ignore].paths 过滤工具噪声（__pycache__ 等）。"""
    ignore = rules.get("ignore", {}).get("paths", [])
    if not ignore:
        return entries
    return [(c, p) for c, p in entries if not glob_match(p, ignore)]


def load_task_scope(root: Path, rules: dict) -> dict:
    """读取任务声明文件；不存在返回 {}。"""
    path = root / rules["scope"]["task_scope_file"]
    if not path.is_file():
        return {}
    return json.loads(read_text(path))
