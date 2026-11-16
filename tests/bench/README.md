# tests/bench/ — L4 性能基准

设计文档 §3.4 PERF-01~09 指标的测量程序。**产出数字，不做硬失败断言**；
结果与 PERF 指标对照出报告（进仓库、可复跑）。

## 约定

- 基准可执行命名 `<topic>_bench.cpp`（或 `_bench.py` 编排），同样登记进 ctest；
- 运行需独占机器防抖动：Metadata 记 `serial`；完整采数集进夜跑（`nightly`），本地冒烟记 `fast`；
- 输出统一写 `build/bench/<name>.json`（时间戳 + 环境信息 + 数据），报告生成脚本后续放 `scripts/`。

## 规划的基准套件（对应 PERF 编号）

| 套件 | 测量项 | PERF |
|---|---|---|
| `write_bench` | batch=1/8/64 三档 QPS-延迟曲线、group commit 加速比 | PERF-01/02 |
| `read_bench` | 读走日志 vs ReadIndex vs LeaseRead 三档对比 | PERF-03/04 |
| `recovery_bench` | kill -9 @ 1GB raft log 重启到可服务时间 | PERF-06 |
| `split_bench` | 1M key 均匀分布 split-check + split 完成时间 | PERF-07 |
| `snapshot_bench` | 快照传输吞吐 | PERF-09 |
