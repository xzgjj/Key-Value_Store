# tests/unit/ — L1 单元测试

GoogleTest 单测，进程内运行，不依赖网络与真实磁盘（临时目录可用）。

## 约定

- 文件命名 `<module>_<topic>_test.cpp`，一个文件编译为一个可执行（gtest main）；
- 单测预算 ≤ 10s；超时说明依赖了慢资源，属于设计问题，移去 simulation/integration；
- Raft 三件套（`RaftLog`/`Raft`/`RawNode`）是纯状态机：**消息序列直接喂、状态直接断言**，无需网络
  （设计文档 §4.3，这是本层最重要的测试形态）；
- 共享 fixture 放 `tests/testlib/`，不放这里。

## 规划的首批登记（M0–M2，登记表见 docs/testing.md §4）

| Scope | Level | Runner | Args | Metadata |
|---|---|---|---|---|
| `tests/unit/common/encode_test.cpp` | unit | `encode_test` | - | fast |
| `tests/unit/common/kv_error_test.cpp` | unit | `kv_error_test` | - | fast |
| `tests/unit/wal/wal_record_test.cpp` | unit | `wal_record_test` | - | fast |
| `tests/unit/raft/raft_step_test.cpp` | unit | `raft_step_test` | - | fast |
