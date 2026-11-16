#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TinyKV-CI 检查器自测：在临时目录构造 fixture 仓库，断言各检查器的预期结果。

"测试本身也需要维护"——CI 规则如果咬不住坏行为（或乱咬好行为）就是坏的。
本脚本覆盖：

    docs     预算超限/合规、重复段落（含冻结豁免）、断链、脚本引用缺失
    bugfix   登记表缺失 SKIP、合规 PASS、场景过短/无复现命令/未登记/缺编号标记/漏日期 FAIL、
             删历史行 FAIL、声明 bugfix 却无新增行 FAIL
    scope    无基线 SKIP、未声明 FAIL、声明 PASS、超预算 FAIL/获批 OK、
             删测试 FAIL/获批 OK、删登记行告警
    guard    无改动 PASS、放松标注、收紧标注、新增规则文件只提示
    code     未武装 SKIP、函数过长、复杂度超限、复制粘贴块；字符串剥离单测
    run_ci   重试轮数上限拒绝；聚合退出码契约

用法：python tools/ci/selftest.py        # 全部通过退出 0，否则 1
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SELF_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SELF_DIR))
import lib_ci as L  # noqa: E402
import check_bugfix  # noqa: E402
import check_code  # noqa: E402
import check_docs  # noqa: E402
import check_guard  # noqa: E402
import check_scope  # noqa: E402

REAL_ROOT = Path(__file__).resolve().parent.parent.parent
RULES_SRC = REAL_ROOT / "ci" / "rules.toml"

FAILURES: list[str] = []


# ---- fixture 基建 ------------------------------------------------------------
def make_repo(base: Path, name: str, commit_baseline: bool = True) -> Path:
    root = base / name
    (root / "ci").mkdir(parents=True)
    shutil.copy(RULES_SRC, root / "ci" / "rules.toml")
    if commit_baseline:
        L.git(root, "init", "-b", "main")
        commit_all(root, "baseline")
    else:
        L.git(root, "init", "-b", "main")
    return root


def commit_all(root: Path, msg: str) -> None:
    L.git(root, "add", "-A")
    L.git(root, "-c", "user.name=t", "-c", "user.email=t@example.com",
          "commit", "-m", msg, "-q")


def write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def codes(res: L.CheckResult) -> list[str]:
    return [f.code for f in res.findings]


def expect(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        L.out(f"  ✓ {name}")
    else:
        FAILURES.append(f"{name}{('：' + detail) if detail else ''}")
        L.out(f"  ✗ {name}  {detail}")


def find_res(results: list[L.CheckResult], check: str) -> L.CheckResult:
    return next(r for r in results if r.check == check)


# ---- docs --------------------------------------------------------------------
CJK40 = "这是一段用于重复检测的中文段落，长度超过四十个规范化字符阈值，用于验证重复段落检查器。"


def test_budget(tmp: Path) -> None:
    L.out("[docs] budget")
    root = make_repo(tmp, "budget")
    write(root, "SPEC.md", "# 规格\n" + "测" * 3200)
    write(root, "CI.md", "# 手册\n短文档，" + "ok " * 30)
    over = find_res(check_docs.run(root, L.load_rules(root)), "budget")
    expect("超预算 FAIL", over.status == L.FAIL and "DOC-BUDGET" in codes(over),
           f"status={over.status} codes={codes(over)}")
    write(root, "SPEC.md", "# 规格\n" + "测" * 1200 + " ok")
    ok = find_res(check_docs.run(root, L.load_rules(root)), "budget")
    expect("预算内 PASS", ok.status == L.PASS, f"status={ok.status} codes={codes(ok)}")


def test_dup_para(tmp: Path) -> None:
    L.out("[docs] dup_para")
    root = make_repo(tmp, "dup")
    write(root, "notes_a.md", f"# A\n\n{CJK40}\n")
    write(root, "notes_b.md", f"# B\n\n{CJK40}\n")
    dup = find_res(check_docs.run(root, L.load_rules(root)), "dup_para")
    expect("跨文档重复 FAIL", dup.status == L.FAIL and "DOC-DUP" in codes(dup),
           f"status={dup.status}")
    # 冻结文档之间的重复不算；但一旦沾上非冻结文档就要报
    write(root, "docs/README.md", f"# R\n\n{CJK40}\n")
    write(root, "docs/Project1-Standalone-KV.md", f"# P\n\n{CJK40}\n")
    still = find_res(check_docs.run(root, L.load_rules(root)), "dup_para")
    expect("冻结+非冻结重复仍 FAIL", still.status == L.FAIL, f"status={still.status}")
    write(root, "notes_a.md", "# A\n独特内容，绝不与他文重复的段落。")
    write(root, "notes_b.md", "# B\n另一段完全不同的中文内容，说另外一件事，长度足够。")
    ok = find_res(check_docs.run(root, L.load_rules(root)), "dup_para")
    expect("冻结间重复豁免 PASS", ok.status == L.PASS, f"status={ok.status} codes={codes(ok)}")


def test_links(tmp: Path) -> None:
    L.out("[docs] consistency")
    root = make_repo(tmp, "links")
    write(root, "README.md", "看 [手册](CI.md) 与 [缺失](nope.md)，脚本见 `tools/foo.py`。")
    res = find_res(check_docs.run(root, L.load_rules(root)), "consistency")
    expect("断链+缺脚本 FAIL", res.status == L.FAIL
           and "DOC-LINK" in codes(res) and "DOC-TOOL" in codes(res), f"codes={codes(res)}")
    write(root, "nope.md", "占位")
    write(root, "CI.md", "# 手册\n")
    write(root, "tools/foo.py", "print('x')\n")
    ok = find_res(check_docs.run(root, L.load_rules(root)), "consistency")
    expect("修复后 PASS", ok.status == L.PASS, f"codes={codes(ok)}")


# ---- bugfix ------------------------------------------------------------------
TEST_REG_MD = (
    "# 测试\n\n<!-- registry:begin v1 -->\n"
    "| Scope | Level | Runner | Args | Metadata |\n|---|---|---|---|---|\n"
    "| tests/unit/demo_test.cpp | unit | `demo_test` | - | fast |\n"
    "<!-- registry:end -->\n"
)
TEST_SRC_MARKED = "// BUG-0001: kill leader 后写入返回 Unavailable\nTEST(Demo, Crash) { EXPECT_EQ(1, 1); }\n"
BUGS_HEAD = "| Bug | 触发场景 | 复现 | 回归测试 | 修复 |\n|---|---|---|---|---|\n"
ROW_OK = ("| BUG-0001 | 3 节点 kill leader 后写入返回 Unavailable，重启节点丢失最后两条日志 "
          "| `--seed=42 --kill=leader` | `demo_test` | 2026-11-16（1a2b3c4） |")


def bugs_md(*rows: str) -> str:
    return "<!-- bugs:begin v1 -->\n" + BUGS_HEAD + "".join(r + "\n" for r in rows) + "<!-- bugs:end -->\n"


def bugfix_repo(tmp: Path, name: str) -> Path:
    root = make_repo(tmp, name)
    write(root, "docs/testing.md", TEST_REG_MD)
    write(root, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(root, "docs/bugs.md", bugs_md(ROW_OK))
    commit_all(root, "baseline with one bug")
    return root


def bugfix_result(root: Path) -> L.CheckResult:
    return check_bugfix.check_bugfix(root, L.load_rules(root))


def test_bugfix(tmp: Path) -> None:
    L.out("[bugfix] 场景 / 复现 / 登记 / 编号标记 / 只增")
    # 登记表未建立且无基线 → SKIP（首次引入前不误伤）
    fresh = make_repo(tmp, "bug_nobase", commit_baseline=False)
    r = bugfix_result(fresh)
    expect("无登记表 SKIP", r.status == L.SKIP and "NOT_ARMED" in r.summary, r.summary)

    ok_root = bugfix_repo(tmp, "bug_ok")
    r = bugfix_result(ok_root)
    expect("合规 PASS", r.status == L.PASS, f"status={r.status} codes={codes(r)}")

    # 场景过短：换成"修了个 bug"式描述
    short = make_repo(tmp, "bug_scene")
    write(short, "docs/testing.md", TEST_REG_MD)
    write(short, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(short, "docs/bugs.md",
          bugs_md("| BUG-0001 | 修了个 bug | `--seed=42` | `demo_test` | 2026-11-16 |"))
    r = bugfix_result(short)
    expect("场景过短 FAIL", r.status == L.FAIL and "BUG-SCENE" in codes(r), f"codes={codes(r)}")

    # 复现列没有可执行命令
    norepro = make_repo(tmp, "bug_repro")
    write(norepro, "docs/testing.md", TEST_REG_MD)
    write(norepro, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(norepro, "docs/bugs.md",
          bugs_md("| BUG-0001 | 集群重启后客户端连不上，三节点全部不可达且状态异常 "
                  "| 复现步骤见对话 | `demo_test` | 2026-11-16 |"))
    r = bugfix_result(norepro)
    expect("无复现命令 FAIL", r.status == L.FAIL and "BUG-REPRO" in codes(r), f"codes={codes(r)}")

    # 回归测试没登记进 testing.md §4
    unreg = make_repo(tmp, "bug_unreg")
    write(unreg, "docs/testing.md", TEST_REG_MD)
    write(unreg, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(unreg, "docs/bugs.md",
          bugs_md("| BUG-0001 | 集群重启后客户端连不上，三节点全部不可达且状态异常 "
                  "| `--seed=42` | `ghost_test` | 2026-11-16 |"))
    r = bugfix_result(unreg)
    expect("回归测试未登记 FAIL", r.status == L.FAIL and "BUG-NOREG" in codes(r),
           f"codes={codes(r)}")

    # 登记了但源码里没有编号标记（防止"登记了但没真跑"）
    unlinked = make_repo(tmp, "bug_unlinked")
    write(unlinked, "docs/testing.md", TEST_REG_MD)
    write(unlinked, "tests/unit/demo_test.cpp", "TEST(Demo, Basic) { EXPECT_EQ(1, 1); }\n")
    write(unlinked, "docs/bugs.md", bugs_md(ROW_OK))
    r = bugfix_result(unlinked)
    expect("缺编号标记 FAIL", r.status == L.FAIL and "BUG-UNLINKED" in codes(r),
           f"codes={codes(r)}")

    # 缺日期
    nodate = make_repo(tmp, "bug_date")
    write(nodate, "docs/testing.md", TEST_REG_MD)
    write(nodate, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(nodate, "docs/bugs.md",
          bugs_md("| BUG-0001 | 3 节点 kill leader 后写入返回 Unavailable，重启丢最后两条日志 "
                  "| `--seed=42` | `demo_test` | 已修复 |"))
    r = bugfix_result(nodate)
    expect("缺日期 FAIL", r.status == L.FAIL and "BUG-DATE" in codes(r), f"codes={codes(r)}")

    # 删除历史行 → FAIL（只增不删）
    rowdel = bugfix_repo(tmp, "bug_rowdel")
    write(rowdel, "docs/bugs.md", bugs_md())
    r = bugfix_result(rowdel)
    expect("删历史行 FAIL", r.status == L.FAIL and "BUG-ROWDEL" in codes(r), f"codes={codes(r)}")

    # 声明 bugfix 却无新增登记行
    declared = make_repo(tmp, "bug_norow")
    write(declared, "docs/testing.md", TEST_REG_MD)
    write(declared, "tests/unit/demo_test.cpp", TEST_SRC_MARKED)
    write(declared, "docs/bugs.md", bugs_md())
    commit_all(declared, "baseline empty")
    write(declared, "ci/task_scope.json",
          '{"task":"修 bug","paths":["src/"],"reason":"r","bugfix":true}')
    r = bugfix_result(declared)
    expect("声明 bugfix 无新增行 FAIL", r.status == L.FAIL and "BUG-NOROW" in codes(r),
           f"codes={codes(r)}")


# ---- scope -------------------------------------------------------------------
def base_project(root: Path) -> None:
    write(root, "docs/testing.md",
          "# 测试\n\n| Scope | Level | Runner | Args | Metadata |\n|---|---|---|---|---|\n"
          "| tests/unit/demo_test.cpp | unit | `demo_test` | - | fast |\n")
    write(root, "tests/unit/demo_test.cpp", "TEST(Demo, Basic) { EXPECT_EQ(1, 1); }\n")
    write(root, "README.md", "# demo\n")
    commit_all(root, "init project")


def scope_result(root: Path) -> L.CheckResult:
    return check_scope.check_scope(root, L.load_rules(root))


def test_scope(tmp: Path) -> None:
    L.out("[scope] 声明 / 预算 / 删除告警")
    # 无基线 → SKIP
    no_base = make_repo(tmp, "scope_nobase", commit_baseline=False)
    write(no_base, "x.txt", "x\n")
    r = scope_result(no_base)
    expect("无基线 SKIP", r.status == L.SKIP, f"status={r.status}")
    # 干净工作区 → PASS
    clean = make_repo(tmp, "scope_clean")
    base_project(clean)
    r = scope_result(clean)
    expect("干净 PASS", r.status == L.PASS, f"status={r.status} codes={codes(r)}")
    # 有改动无声明 → FAIL
    undecl = make_repo(tmp, "scope_undecl")
    base_project(undecl)
    write(undecl, "new_file.txt", "hello\n")
    r = scope_result(undecl)
    expect("未声明 FAIL", r.status == L.FAIL and "SCOPE-DECL" in codes(r), f"codes={codes(r)}")
    # 声明后 PASS（__pycache__ 噪声被 ignore，不算范围外）；范围外另有一个文件 → FAIL
    write(undecl, "tools/ci/__pycache__/lib_ci.pyc", "\x00fake")
    write(undecl, "ci/task_scope.json",
          '{"task":"t","paths":["new_file.txt"],"reason":"r"}')
    r = scope_result(undecl)
    expect("声明后 PASS", r.status == L.PASS, f"status={r.status} codes={codes(r)}")
    write(undecl, "rogue.txt", " rogue\n")
    r = scope_result(undecl)
    expect("范围外 FAIL", r.status == L.FAIL and "SCOPE-OUT" in codes(r), f"codes={codes(r)}")
    # diff 预算：超限 FAIL → 获批 OK
    over = make_repo(tmp, "scope_over")
    base_project(over)
    write(over, "big.txt", "\n".join(f"line{i}" for i in range(700)))
    write(over, "ci/task_scope.json", '{"task":"t","paths":["big.txt"],"reason":"r"}')
    r = scope_result(over)
    expect("超预算 FAIL", "SCOPE-OVER" in codes(r), f"codes={codes(r)}")
    write(over, "ci/task_scope.json",
          '{"task":"t","paths":["big.txt"],"reason":"r","overflow_approved":true,"approver":"用户"}')
    r = scope_result(over)
    expect("超预算获批 OK", r.status == L.PASS and "SCOPE-OVER-OK" in codes(r), f"codes={codes(r)}")
    # 删测试 → FAIL；获批 → OK
    deleter = make_repo(tmp, "scope_deltest")
    base_project(deleter)
    L.git(deleter, "rm", "-q", "tests/unit/demo_test.cpp")
    r = scope_result(deleter)
    expect("删测试未批 FAIL", r.status == L.FAIL and "SCOPE-TESTDEL" in codes(r),
           f"codes={codes(r)}")
    write(deleter, "ci/task_scope.json",
          '{"task":"t","paths":["tests/"],"reason":"行为变更",'
          '"behavior_change_approved":true,"approver":"用户"}')
    r = scope_result(deleter)
    expect("删测试获批 OK", r.status == L.PASS and "SCOPE-TESTDEL-OK" in codes(r),
           f"codes={codes(r)}")
    # 删登记行 → 告警
    regdel = make_repo(tmp, "scope_delreg")
    base_project(regdel)
    p = regdel / "docs" / "testing.md"
    p.write_text("# 测试\n\n| Scope | Level | Runner | Args | Metadata |\n|---|---|---|---|---|\n",
                 encoding="utf-8")
    r = scope_result(regdel)
    expect("删登记行告警", r.status == L.FAIL and "SCOPE-TESTDEL" in codes(r),
           f"codes={codes(r)}")


# ---- guard -------------------------------------------------------------------
def guard_result(root: Path) -> L.CheckResult:
    return check_guard.check_guard(root, L.load_rules(root))


def test_guard(tmp: Path) -> None:
    L.out("[guard] 规则守卫 / 防放松")
    root = make_repo(tmp, "guard")
    write(root, "README.md", "# r\n")
    commit_all(root, "init")
    r = guard_result(root)
    expect("无改动 PASS", r.status == L.PASS, f"status={r.status} codes={codes(r)}")
    # 放松：预算 3000 → 9999
    p = root / "ci" / "rules.toml"
    p.write_text(p.read_text(encoding="utf-8").replace(
        "planning_max_chars = 3000", "planning_max_chars = 9999"), encoding="utf-8")
    r = guard_result(root)
    expect("放松 → APPROVAL + WEAKEN", r.status == L.APPROVAL
           and "GUARD-WEAKEN" in codes(r), f"status={r.status} codes={codes(r)}")
    # 收紧：3000 → 2500
    p.write_text(p.read_text(encoding="utf-8").replace(
        "planning_max_chars = 9999", "planning_max_chars = 2500"), encoding="utf-8")
    r = guard_result(root)
    expect("收紧 → APPROVAL + TIGHTEN", r.status == L.APPROVAL
           and "GUARD-TIGHTEN" in codes(r) and "GUARD-WEAKEN" not in codes(r),
           f"codes={codes(r)}")
    L.git(root, "checkout", "--", "ci/rules.toml")
    # 新增规则文件 → 只提示不拦截
    write(root, "tools/ci/new_rule.py", "# x\n")
    r = guard_result(root)
    expect("新规则文件 PASS+提示", r.status == L.PASS and "GUARD-NEW" in codes(r),
           f"status={r.status} codes={codes(r)}")
    # 移除保护条目 → 放松
    p.write_text(p.read_text(encoding="utf-8").replace('    "SPEC.md",\n', ''),
                 encoding="utf-8")
    r = guard_result(root)
    expect("移除保护条目 → WEAKEN", r.status == L.APPROVAL and "GUARD-WEAKEN" in codes(r),
           f"codes={codes(r)}")


# ---- code --------------------------------------------------------------------
LONG_FUNC = "int f() {\n" + "\n".join(f"  x += {i};" for i in range(100)) + "\n  return x;\n}\n"
CPLX_FUNC = ("int g(int a) {\n  int s = 0;\n"
             + "\n".join(f"  if (a > {i}) {{ s += {i}; }}" for i in range(20))
             + "\n  return s;\n}\n")
DUP_BLOCK = "\n".join(f"  values.push_back({i} * 3 + 7);" for i in range(10))


def test_code(tmp: Path) -> None:
    L.out("[code] 武装 / 静态 / 复制粘贴")
    root = make_repo(tmp, "code_plain")
    r = check_code.run(root, L.load_rules(root))[0]
    expect("未武装 SKIP", r.status == L.SKIP and "NOT_ARMED" in r.summary, r.summary)

    root = make_repo(tmp, "code_bad")
    write(root, "src/bad.cpp", "#include <vector>\n" + LONG_FUNC)
    write(root, "src/cplx.cpp", CPLX_FUNC)
    write(root, "src/dup1.cpp", "void a() {\n" + DUP_BLOCK + "\n}\n")
    write(root, "src/dup2.cpp", "void b() {\n" + DUP_BLOCK + "\n}\n")
    files = check_code.armed_files(root, L.load_rules(root))
    expect("武装文件收集", sorted(files) == ["src/bad.cpp", "src/cplx.cpp",
                                             "src/dup1.cpp", "src/dup2.cpp"], str(files))
    rules = L.load_rules(root)
    stat = check_code.check_static(root, rules, files)
    expect("函数过长/复杂度 FAIL", stat.status == L.FAIL
           and "CODE-FUNC-LINES" in codes(stat) and "CODE-FUNC-CPLX" in codes(stat),
           f"codes={codes(stat)}")
    dup = check_code.check_dup_code(root, rules, files)
    expect("复制粘贴 FAIL", dup.status == L.FAIL and "CODE-DUP" in codes(dup),
           f"codes={codes(dup)}")

    root = make_repo(tmp, "code_ok")
    write(root, "src/good.cpp",
          "// 注释里的 if、&&、? 不应计入\nint add(int a, int b) {\n  return a + b;\n}\n"
          'const char* msg = "字符串里的 { } if &&";\n')
    rules = L.load_rules(root)
    files = check_code.armed_files(root, rules)
    stat = check_code.check_static(root, rules, files)
    dup = check_code.check_dup_code(root, rules, files)
    expect("干净代码 PASS", stat.status == L.PASS and dup.status == L.PASS,
           f"stat={codes(stat)} dup={codes(dup)}")


def test_strip() -> None:
    L.out("[code] 字符串/注释剥离")
    src = ('// if (x) { &&\n'
           '/* block } */\n'
           'const char* s = "}{ if && ?";\n'
           'char c = \'?\';\n'
           'const char* raw = R"(**)(unusual )end**)";\n'
           'int a = 1;\n')
    clean = check_code.strip_comments_strings(src)
    expect("注释剥离", "//" not in clean and "/*" not in clean, clean)
    expect("字符串内容剥离", '"}{ if && ?"' not in clean and '""' in clean, clean)
    expect("裸字符串剥离", "unusual" not in clean, clean)
    expect("正文字符保留", "int a = 1;" in clean, clean)


# ---- run_ci ------------------------------------------------------------------
def test_runner(tmp: Path) -> None:
    L.out("[run_ci] 轮数上限 / 退出码契约")
    proc = subprocess.run(
        [sys.executable, str(REAL_ROOT / "tools" / "ci" / "run_ci.py"),
         "--stage", "docs", "--round", "3", "--root", str(REAL_ROOT)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    expect("超轮数拒绝", proc.returncode == L.EXIT_FAIL and "重试上限" in (proc.stdout + proc.stderr),
           f"rc={proc.returncode} out={proc.stdout[:200]}")
    expect("退出码契约", L.aggregate_exit([L.CheckResult("a", L.PASS)]) == L.EXIT_OK
           and L.aggregate_exit([L.CheckResult("a", L.SKIP),
                                 L.CheckResult("b", L.APPROVAL)]) == L.EXIT_APPROVAL
           and L.aggregate_exit([L.CheckResult("a", L.FAIL),
                                 L.CheckResult("b", L.APPROVAL)]) == L.EXIT_FAIL)


def main() -> int:
    L.setup_output()
    L.out("== TinyKV-CI selftest ==")
    with tempfile.TemporaryDirectory(prefix="tinykv_ci_selftest_") as td:
        tmp = Path(td)
        test_budget(tmp)
        test_dup_para(tmp)
        test_links(tmp)
        test_bugfix(tmp)
        test_scope(tmp)
        test_guard(tmp)
        test_code(tmp)
        test_strip()
        test_runner(tmp)
    if FAILURES:
        L.out(f"\nFAIL：{len(FAILURES)} 项未通过")
        for f in FAILURES:
            L.out(f"  - {f}")
        return 1
    L.out("\nPASS：全部自测通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
