# CI.md — TinyKV-CI 操作手册

> 规则定义见 [SPEC.md](SPEC.md) 与 [ci/rules.toml](ci/rules.toml)；本文只讲怎么跑、怎么读结果、失败后做什么。
> 所有脚本仅用 Python 3.11+ 标准库，Windows / Linux 通用，不依赖构建产物。

## 1. 常用命令

```bash
python tools/ci/run_ci.py                    # 全阶段（提交前必跑，--round 1）
python tools/ci/run_ci.py --round 2          # 第 2 轮修复重试（上限见 rules.toml）
python tools/ci/run_ci.py --stage docs       # 只跑文档阶段
python tools/ci/check_align.py               # 只跑对齐阶段（契约/文档命令/违禁模式）
python tools/ci/run_ci.py --json             # 附加机器可读 JSON（供代理消费）
python tools/ci/selftest.py                  # 检查器自测（改检查器后必跑）
```

单阶段调试：`python tools/ci/check_{docs,scope,guard,code}.py --root <仓库路径>`。

## 2. 退出码契约

| 码 | 含义 | 你该做什么 |
|---|---|---|
| 0 | 绿（含可见的 SKIP） | 可提交 |
| 1 | FAIL，可判定问题被拦 | 修复后重跑并 `--round` 加一；超过上限（2）停下找人工 |
| 2 | APPROVAL，规则资产被改动 | 停下，等人工批准；批准落进 task_scope 后重跑 |

## 3. 六个阶段

| 阶段 | 内容 | 未武装时的表现 |
|---|---|---|
| docs | 文档预算、重复段落、链接/脚本引用、登记表一致性 | 不跳过，始终武装 |
| bugfix | 缺陷回归：触发场景、可执行复现、已登录归测试、用例编号标记（`docs/bugs.md`） | 登记表未建立 → SKIP(NOT_ARMED) |
| guard | 规则资产改动守卫 + 防放松对比 | 无 git 基线 → SKIP(NO_BASELINE) |
| scope | 任务声明、范围白名单、diff 预算、测试删除告警 | 无 git 基线 → SKIP(NO_BASELINE) |
| code | cmake 构建 + `ctest -L fast` + 函数行数/复杂度 + 复制粘贴 | 无 C++ 代码 → SKIP(NOT_ARMED)；代码落地即自动武装 |
| align | 契约集合一致、文档命令可执行性、违禁模式扫描 | 无 CMakePresets.json → doc_cmd 为 SKIP(NO_PRESETS) |

> align 阶段是"代码为主、文档对齐代码"的落点：它只比对**可判定**的三件事——
> C++ 错误变体与 proto 消息是否对称、文档里写的命令对象是否存在、红线条目是否被违反；
> 语义是否一致仍归人工（本文件 §4）。刻意不对称（如尚未上线的错误变体）在
> `ci/rules.toml [align].contract_exempt` 中逐条声明，报告里可见列出，不静默。
>
> bugfix 阶段是"修 bug 必须留下触发场景"的落点：登记表 `docs/bugs.md` 每行要有现象与条件、
> 一条可执行复现命令、一个已在 `docs/testing.md` §4 登记的回归测试，且该测试源码里带
> `// BUG-NNNN:` 标记。四条缺一即红——bug 修好了但没有回归测试，视为没修完。

SKIP 是"未武装"，不是"通过"：报告里可见列出，集成 CI 时据此知道哪些检查尚未生效。

## 4. 失败 → 动作对照

| 发现码 | 含义 | 动作 |
|---|---|---|
| DOC-BUDGET | 规划文档超 3000 字 | 删减或把内容移入分析文档（记录类不设上限） |
| DOC-DUP | 段落跨文档重复 | 保留一处，其余改为引用 |
| DOC-LINK / DOC-TOOL | 链接或引用的脚本不存在 | 修路径；确属历史断链且文件非你所有 → 报告所有人 |
| REG-CONSIST | 登记表与仓库不一致 | 按 docs/testing.md §5 SOP 补登记 |
| BUG-SCENE / BUG-REPRO | 触发场景空泛 / 复现列没有可执行命令 | 写清现象+条件，补 `--seed=`、`ctest -R`、脚本调用等反引号命令 |
| BUG-NOREG / BUG-UNLINKED | 回归测试未登记 / 测试源码缺 `BUG-NNNN` 标记 | 先按 docs/testing.md §5 登记，再在用例上加 `// BUG-NNNN: 一句话` |
| BUG-ROWDEL / BUG-NOROW | 删历史登记行 / 声明 bugfix 却没新增行 | 只增不删，更正另起一行；补一条完整登记 |
| BUG-DATE | 修复列缺日期 | 填 `YYYY-MM-DD`（有 commit 带短哈希） |
| SCOPE-DECL / SCOPE-OUT | 改动未声明 / 超出声明 | 更新 ci/task_scope.json 的 paths 或撤销改动 |
| SCOPE-OVER | diff 超预算 | 拆任务；确需整体提交 → 人工批准 overflow_approved |
| SCOPE-TESTDEL | 测试资产或登记行被删 | 恢复；确属行为变更 → 人工批准 behavior_change_approved |
| GUARD-CHG / GUARD-WEAKEN | 规则资产被改 / 被放松 | 停下等人工批准 |
| CODE-BUILD / CODE-TOOLCHAIN | 构建失败 / 缺工具链 | 修代码或装工具链（判定只认实测，不虚绿） |
| CODE-FUNC-LINES / -CPLX | 函数过长 / 过复杂 | 拆函数 |
| CODE-DUP | 复制粘贴块 | 提取公共实现 |
| ALIGN-MISSING / ALIGN-EXTRA | C++ 错误变体与 proto 消息不对称且未声明 | 补 proto 消息，或在 rules.toml `[align].contract_exempt` 声明刻意不对称（需人工批准） |
| ALIGN-STALE | 豁免条目已失效（变体不存在） | 删除该豁免（属收紧，直接放行） |
| ALIGN-PRESET / -TESTDIR / -LABEL | 文档写的 preset/目录/标签不存在 | 改文档指向真实对象，不要放宽检查 |
| BAN-HIT | 违禁模式命中（detach / 裸数组 new / sleep 对齐启动） | 改写法；确需例外则登记 `banned_exempt` 并留批准痕迹 |

## 5. 任务声明文件 ci/task_scope.json

每个任务开始时写（随任务提交），格式：

```json
{
  "task": "一句话任务描述",
  "paths": ["ci/", "tools/"],
  "reason": "为什么改这些",
  "bugfix": false,
  "fixes": [],
  "overflow_approved": false,
  "behavior_change_approved": false,
  "approver": ""
}
```

`paths` 支持目录与 glob；两项 approved 只有在 `approver` 非空时生效。
修 bug 的任务把 `bugfix` 置 true（可列 `fixes: ["BUG-0001"]`）：bugfix 阶段据此要求本任务必须新增登记行，
未新增即红——声明是自我约束，不是豁免开关。

## 6. 自测与维护

改任何检查器或 rules.toml 阈值后：先 `python tools/ci/selftest.py`（fixture 断言全绿），
再跑 `run_ci.py`；两者都绿才算改完。检查器本身的回归由 selftest 负责。
