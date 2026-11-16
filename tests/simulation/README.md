# tests/simulation/ — L2 确定性模拟测试

虚拟时钟 + 虚拟网络（延迟/分区/丢包/重启随机注入），进程内跑真实 Raft 逻辑。
**P2/P3 全部特性先在此层跑通，再上真网络**（设计文档 §4.3 的建议决策）。

## 约定

- 文件命名 `<module>_<topic>_test.cpp`，依赖 `testlib` 的虚拟时钟/网络/集群装配器；
- **随机种子是显式入参**：失败可用种子精确复现（本层存在的意义）；
- 单场景预算 ≤ 60s，Metadata 记 `fast` 进 PR CI；
- 同一二进制的多个种子/规模组合 = 多行登记、独立 ctest 名（`sim_election_n3` / `sim_election_n5`）。

## 典型登记示例（P2 时期）

| Scope | Level | Runner | Args | Metadata |
|---|---|---|---|---|
| `tests/simulation/sim_election_test.cpp` | simulation | `sim_election_n3` = sim_election_test | `--seed=42 --nodes=3` | fast |
| `tests/simulation/sim_election_test.cpp` | simulation | `sim_election_n5` = sim_election_test | `--seed=777 --nodes=5` | fast |
