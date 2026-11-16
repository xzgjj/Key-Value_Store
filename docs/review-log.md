# 审查台账（review-log）

> 每阶段收尾的"三轮检查"记录在此，随提交入库、跨会话可查。
> 写入走 `python tools/review_rounds.py record`，门禁走 `gate`；**不要手工改标记区**，
> 表格列宽由工具维护，手改会破坏解析。
> 每轮设不同重点：可判定性 → 契约与复用 → 文档与红线（见 AGENT_EXECUTION_PROTOCOL.md §8）。

## 轮次记录

<!-- review:begin v1 -->
| 阶段 | 轮次 | 重点 | 结论 | 备注 |
|---|---|---|---|---|
| M0 | 1 | 可判定性：CI 全阶段 + 单元测试 | PASS | run_ci PASS×13；ctest 5/5 |
| M0 | 2 | 契约与复用：对齐检查 + 登记对账 + 检查器自测 | PASS | align 三项全 PASS；registry PASS；selftest PASS |
| M0 | 3 | 文档与红线：预算/查重/链接 + scope/guard + SPEC-0 红线抽查 | PASS | docs PASS×4；scope/guard PASS；src 无空目录、StrongId 无隐式转换 |
| M1 | 1 | 可判定性：CI 全阶段 + 全部测试 | PASS | run_ci PASS×13；ctest 15/15（含 bench） |
| M1 | 2 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 检查器自测 | PASS | align 3/3；registry PASS；bugfix PASS；selftest PASS |
| M1 | 3 | 文档与红线：预算/查重/链接 + guard + 恢复路径不改写/无 detach 抽查 | PASS | docs PASS×4；guard PASS；src 无空目录；恢复路径无 truncate |
| M2 | 1 | 可判定性：CI 全阶段 + 19 个测试可执行 | PASS | run_ci PASS×13；ctest 19/19 |
| M2 | 2 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 自测 | PASS | align 3/3；registry/bugfix/selftest 全 PASS |
| M2 | 3 | 文档与红线：预算/查重/链接 + guard + 心跳不带日志/无空目录 | PASS | docs PASS×4；guard PASS；红线抽查通过 |
| M2 | 4 | 可判定性：CI 全阶段 + 22 个测试可执行 + 150 轮长跑 | PASS | run_ci PASS×13；ctest 22/22（含 raft_chaos_n150） |
| M2 | 5 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 自测 | PASS | align 3/3；registry/bugfix/selftest 全 PASS |
| M2 | 6 | 文档与红线：预算/查重/链接 + guard + 心跳不带日志抽查 | PASS | docs PASS×4；guard PASS；无空目录 |
| M3 | 1 | 可判定性：CI 全阶段 + 23 个测试可执行 | PASS | run_ci PASS×13；ctest 23/23（含 readindex_test） |
| M3 | 2 | 契约与复用：对齐三项 + 登记对账 + 自测 | PASS | align 3/3；registry/selftest PASS |
| M3 | 3 | 文档与红线：预算/查重/链接 + guard + 租约默认关抽查 | PASS | docs PASS×4；guard 无改动；lease 默认 false |
<!-- review:end -->

## 约定

- 「结论」只允许 PASS / FAIL；FAIL 轮次保留在台账里，作为修复依据，不删除。
- 三轮全 PASS 后跑对齐检查（`python tools/ci/check_align.py`），两者都过才可进入推送环节。
- 台账本身不替代证据：每轮「备注」应写清跑了什么命令、产物在哪。
