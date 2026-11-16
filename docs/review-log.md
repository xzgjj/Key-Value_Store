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
| M3 | 4 | 可判定性：CI 全阶段 + 26 个测试 + 三模式基准 | PASS | run_ci PASS×13；ctest 26/26；read_bench 58.7x/3652.8x |
| M3 | 5 | 契约与复用：对齐三项 + 登记对账 + 自测 | PASS | align 3/3；registry/selftest PASS |
| M3 | 6 | 文档与红线：预算/查重/链接 + guard + 租约默认关 | PASS | docs PASS×4；guard PASS |
| M2 | 7 | 可判定性：29 个测试 + 四项欠账证据 | PASS | ctest 29/29；CI PASS×13；snapshot/crash/bench/cluster-write 全绿 |
| M2 | 8 | 契约与复用：对齐三项 + 登记对账 | PASS | align 3/3；registry PASS |
| M2 | 9 | 文档与红线：预算/查重/链接 + guard | PASS | docs PASS×4；guard PASS |
| M2 | 10 | 可判定性：30 个测试 + 真 TCP 压测驱动 | PASS | ctest 30/30；CI PASS×13；tcp_perf_bench A/B/C 三阶段出数 |
| M2 | 11 | 契约与复用：对齐三项 + 登记对账 | PASS | align 3/3；registry PASS |
| M2 | 12 | 文档与红线：预算/查重/链接 + guard | PASS | docs PASS×4；guard PASS |
| M2 | 13 | A-group commit 可判定性：CI 13 项 + wal_group_commit 3 例 + crash/wal 回归 | PASS | run_ci PASS×13；ctest 31/31（含 wal_group_commit 3 例、crash_recovery、raft_chaos_n150）；单机 write_bench 3184→74728 QPS |
| M2 | 14 | A-group commit 契约与复用：对齐三项 + 登记对账 + 延迟窗口语义核对 | PASS | align 3/3；registry PASS（wal_group_commit_test 已登记）；behavior_change_approved 留痕；proto 契约未改 |
| M2 | 15 | A-group commit 文档与红线：预算/查重/链接 + guard + 打开即持久语义回归 | PASS | docs PASS×4；guard PASS；crash_recovery_test 绿证明进程级崩溃不丢；notes-m2 §8.1 如实记 S0 未达标 |
| M4 | 1 | ①可判定性：CI 13 项 + entry_batch 5 例 + 全量 32 测试 + PERF-01 达标 | PASS | run_ci PASS×13；ctest 32/32；entry_batch_test 5/5；管道化 batch=1 fsync=on 28433/s > S0 2000 |
| M4 | 2 | ①契约与复用：proto 未改 + 对齐三项 + 回执不串/去重/非leader拒绝回归 | PASS | align 3/3；registry PASS；proto 零改动；raft_kv_test 非leader拒绝回归经全量测试抓出并修复 |
| M4 | 3 | ①文档与红线：预算/查重/链接 + scope/guard + Envelope 向后兼容/无 detach | PASS | docs PASS×4；guard PASS；try_unpack 兼容旧 data；committer 无 detach；banned 0 命中 |
| M4 | 4 | ②可判定性：CI 13 项 + region_route 8 例 + 全量 33 测试 | PASS | run_ci PASS×13；ctest 33/33；region_route 8/8（路由/epoch/split原子/并发读/重叠拒绝） |
| M4 | 5 | ②契约与复用：复用 RegionEpoch/RegionId/错误变体 + 对齐三项 | PASS | align 3/3；registry PASS；复用 error.h 的 epoch 双判定与 6 变体错误；std::map 不引新库（语言机制优先） |
| M4 | 6 | ②文档与红线：预算/查重/链接 + scope/guard + REL-05不变量 | PASS | docs PASS×4；guard PASS；每 key 恰好一 region 由区间索引+重叠拒绝双保险；shared_mutex 并发安全 |
| M4 | 7 | ③可判定性：CI 13 + region_route 11 + split 3 + 全量 34 | PASS | ctest 34/34；split 端到端 3/3（切分/边界拒绝/数据保留）；run_ci PASS×13 |
| M4 | 8 | ③契约与复用：复用 SplitRequest proto + epoch + 对齐三项 | PASS | align 3/3；proto 复用现有 SplitRequest 未改；split_region 纯函数复用 RegionEpoch 推进 |
| M4 | 9 | ③文档与红线：预算/查重/链接 + scope/guard + 红线1/2 | PASS | docs/guard PASS；红线1开区间校验+红线2 apply后立即更新路由经 split_test 证明 |
| M4 | 10 | ④a可判定性：CI 13 + confchange 5 + 全量 35 | PASS | ctest 35/35；confchange 5/5（AddLearner/AddVoter/RemoveNode/落盘type/非leader拒绝）；run_ci PASS×13 |
| M4 | 11 | ④a契约与复用：Entry.type proto 已核对 + 复用 Cluster fixture + 对齐三项 | PASS | align 3/3；Entry.type/ConfChange proto 用户核对通过(M4-CONFCHANGE-DRAFT)；复用 add_node 扩展 fixture |
| M4 | 12 | ④a文档与红线：预算/查重/链接 + scope/guard + apply时才生效 | PASS | docs/guard PASS；ConfChange 在 collect_apply 时生效，幂等可重建；已知遗留(learner追平门槛/两节点移除边界)留④b |
| M4 | 13 | ④b可判定性：CI13项 + sim_confchange 3例 + 全量36 + confchange追平升级 | PASS | run_ci PASS×13；ctest 36/36；sim_confchange 3/3（未追平拒绝/两节点移除leader拒绝/三节点移除允许）；confchange AddVoterPromotes 升级为真实追平后提升 |
| M4 | 14 | ④b契约与复用：proto未改 + 对齐三项 + 登记对账 + 门槛语义核对 | PASS | align 3/3（contract/doc_cmd/banned）；registry 36/36；proto未改；InvalidArgument复用现成变体未扩契约；D3-5=差<100持续5s按tick可复现 |
| M4 | 15 | ④b文档与红线：预算/查重/链接 + scope/guard + 红线5与learner不计majority | PASS | docs PASS×4；guard PASS；scope 278行/6文件在预算；红线5两节点移除leader守卫经用例证明；learner回执不调advance_commit不污染多数派 |
<!-- review:end -->

## 约定

- 「结论」只允许 PASS / FAIL；FAIL 轮次保留在台账里，作为修复依据，不删除。
- 三轮全 PASS 后跑对齐检查（`python tools/ci/check_align.py`），两者都过才可进入推送环节。
- 台账本身不替代证据：每轮「备注」应写清跑了什么命令、产物在哪。
