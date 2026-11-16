# tests/integration/ — L3 集成随机测试

多进程 3~5 节点真网络：随机注入 kill/分区/恢复，客户端操作历史落盘，
离线做线性一致性检查（porcupine 思路）；Split/ConfChange 与读写并发叠加。

## 约定

- Python/bash 编排脚本放 `scripts/`，被 `add_test` 调起；被测历史与产物写独立临时目录；
- 端口/目录独占（Metadata 记 `serial`），禁止与其他测试并行；
- 依赖 Linux 语义的场景记 `needs-linux`（Windows CI 跳过）；
- **质量门槛：里程碑交付前连续 150 轮随机测试无 FAIL**（设计文档 §8.1），对应 Metadata `nightly` 进夜跑 CI；
- PR 只跑冒烟子集（Metadata `fast`/`slow` 的短轮数变体，独立登记）。

## 目录

- `scripts/` — runner 编排脚本（拉起集群、注入故障、收集历史、调 lin_check）
- 此目录本身的 `*_test.py` 为场景定义与断言入口

## 典型登记示例（P2 时期）

| Scope | Level | Runner | Args | Metadata |
|---|---|---|---|---|
| `tests/integration/raft_chaos_test.py` | integration | `raft_chaos_n150` = bash tests/integration/scripts/run_chaos.sh | `--nodes=3 --rounds=150` | nightly;needs-linux;serial;timeout:3600s |
| `tests/integration/raft_chaos_test.py` | integration | `raft_chaos_smoke` = bash tests/integration/scripts/run_chaos.sh | `--nodes=3 --rounds=3` | fast;needs-linux;serial;timeout:300s |
