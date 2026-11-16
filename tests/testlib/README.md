# tests/testlib/ — 共享测试基建

被多个测试复用的代码放这里；**testlib 下的文件不要求登记**（一致性检查豁免本目录），
它们经由使用它们的测试间接覆盖。

## 规划内容（随阶段落地）

| 里程碑 | 基建 | 说明 |
|---|---|---|
| M0–M1 | `temp_dir.hpp` 等基础 fixture | 临时目录/文件生命周期 RAII |
| M1 | `engine_fixture.hpp` | Storage/Engine 测试脚手架（CF、批量写） |
| M2 | `virtual_clock.hpp` / `virtual_network.hpp` | 确定性模拟器的虚拟时钟与网络（延迟/分区/丢包注入，种子驱动） |
| M2 | `sim_cluster.hpp` | 进程内 N 节点 Raft 集群装配器 |
| M2+ | `lin_check/` | 线性一致性历史检查器封装（porcupine 思路：历史 → 实时序验证） |
| M3+ | `chaos.hpp` / `process_ctl` | 集成测试的故障注入与多进程编排工具 |

## 规则

- 头文件优先（header-only + 必要时配 .cpp），命名不带 `_test` 后缀（避免被误认成测试目标）；
- 只依赖被测库的公共接口，不探内部细节——testlib 用法即"公共 API 该怎么用"的示范；
- 改动 testlib 视同改动全部依赖它的测试，PR 描述需列出受影响的登记行。
