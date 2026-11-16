# tests/ — 测试总目录

> 本目录的**布局规范、登记表、一致性检查**以 [docs/testing.md](../docs/testing.md) 为准（唯一事实来源）。
> 新增任何测试前先读该文档 §5 的 SOP；测试未登记 = CI 失败。

## 目录结构与层级

| 目录 | Level | 内容 | 预算 | CI 触发 |
|---|---|---|---|---|
| `testlib/` | — | 共享测试基建：fixture、虚拟时钟/网络接口、线性一致性检查器封装（不单独登记） | — | — |
| `unit/` | `unit` | gtest 单测：Raft 状态机消息序列、WAL 编解码、CF 前缀编码、事务 CF 操作 | 单测 ≤ 10s | 每次 PR |
| `simulation/` | `simulation` | 确定性模拟器：虚拟时钟 + 虚拟网络（延迟/分区/丢包/重启），种子可复现 | 单场景 ≤ 60s | 每次 PR |
| `integration/` | `integration` | 多进程真网络：随机 kill/分区/恢复 + 操作历史落盘 + 线性一致性检查 | 分钟~小时级 | 夜跑（150 轮纪律） |
| `bench/` | `bench` | 性能采数：QPS/延迟/恢复时间/split 耗时（PERF-01~09 的测量程序） | 分钟级 | 夜跑 + 手动 |

## 硬规则（速记版，完整版见 docs/testing.md §2）

1. **命名**：`<module>_<topic>_test.{cpp,sh,py}`，如 `raft_step_test.cpp`、`wal_crash_test.sh`；
2. **注册**：CMake 只经 `add_tinykv_test()` 封装函数注册进 ctest，禁止散装 `add_test`；
3. **登记**：每个测试在 `docs/testing.md` §4 表中占一行（Scope/Level/Runner/Args/Metadata 五列填全）；
   同一二进制的每种参数/环境变体 = 独立一行 + 独立 ctest 名；
4. **自检**：提交前 `python tools/check_test_registry.py` 必须 PASS。

## 各目录 README

- [testlib/README.md](testlib/README.md)
- [unit/README.md](unit/README.md)
- [simulation/README.md](simulation/README.md)
- [integration/README.md](integration/README.md)
- [bench/README.md](bench/README.md)
