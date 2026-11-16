# CI.md — TinyKV-CI 操作手册

> 规则定义见 [SPEC.md](SPEC.md) 与 [ci/rules.toml](ci/rules.toml)；本文只讲怎么跑、怎么读结果、失败后做什么。
> 所有脚本仅用 Python 3.11+ 标准库，Windows / Linux 通用，不依赖构建产物。

## 1. 常用命令

```bash
python tools/ci/run_ci.py                    # 全阶段（提交前必跑，--round 1）
python tools/ci/run_ci.py --round 2          # 第 2 轮修复重试（上限见 rules.toml）
python tools/ci/run_ci.py --stage docs       # 只跑文档阶段
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

## 3. 四个阶段

| 阶段 | 内容 | 未武装时的表现 |
|---|---|---|
| docs | 文档预算、重复段落、链接/脚本引用、登记表一致性 | 不跳过，始终武装 |
| guard | 规则资产改动守卫 + 防放松对比 | 无 git 基线 → SKIP(NO_BASELINE) |
| scope | 任务声明、范围白名单、diff 预算、测试删除告警 | 无 git 基线 → SKIP(NO_BASELINE) |
| code | cmake 构建 + `ctest -L fast` + 函数行数/复杂度 + 复制粘贴 | 无 C++ 代码 → SKIP(NOT_ARMED)；代码落地即自动武装 |

SKIP 是"未武装"，不是"通过"：报告里可见列出，集成 CI 时据此知道哪些检查尚未生效。

## 4. 失败 → 动作对照

| 发现码 | 含义 | 动作 |
|---|---|---|
| DOC-BUDGET | 规划文档超 3000 字 | 删减或把内容移入分析文档（记录类不设上限） |
| DOC-DUP | 段落跨文档重复 | 保留一处，其余改为引用 |
| DOC-LINK / DOC-TOOL | 链接或引用的脚本不存在 | 修路径；确属历史断链且文件非你所有 → 报告所有人 |
| REG-CONSIST | 登记表与仓库不一致 | 按 docs/testing.md §5 SOP 补登记 |
| SCOPE-DECL / SCOPE-OUT | 改动未声明 / 超出声明 | 更新 ci/task_scope.json 的 paths 或撤销改动 |
| SCOPE-OVER | diff 超预算 | 拆任务；确需整体提交 → 人工批准 overflow_approved |
| SCOPE-TESTDEL | 测试资产或登记行被删 | 恢复；确属行为变更 → 人工批准 behavior_change_approved |
| GUARD-CHG / GUARD-WEAKEN | 规则资产被改 / 被放松 | 停下等人工批准 |
| CODE-BUILD / CODE-TOOLCHAIN | 构建失败 / 缺工具链 | 修代码或装工具链（判定只认实测，不虚绿） |
| CODE-FUNC-LINES / -CPLX | 函数过长 / 过复杂 | 拆函数 |
| CODE-DUP | 复制粘贴块 | 提取公共实现 |

## 5. 任务声明文件 ci/task_scope.json

每个任务开始时写（随任务提交），格式：

```json
{
  "task": "一句话任务描述",
  "paths": ["ci/", "tools/"],
  "reason": "为什么改这些",
  "overflow_approved": false,
  "behavior_change_approved": false,
  "approver": ""
}
```

`paths` 支持目录与 glob；两项 approved 只有在 `approver` 非空时生效。

## 6. 自测与维护

改任何检查器或 rules.toml 阈值后：先 `python tools/ci/selftest.py`（fixture 断言全绿），
再跑 `run_ci.py`；两者都绿才算改完。检查器本身的回归由 selftest 负责。
