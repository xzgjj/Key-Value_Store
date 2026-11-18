# TinyKV-CPP 测试策略与登记表

> **本页地位**：本项目测试策略的**唯一事实来源**（single source of truth）。
> 任何"有哪些测试、怎么跑、跑多快、要不要特殊环境"的问题，以 §4 登记表为准；
> 本页与仓库实际不一致时，以一致性检查（`tools/check_test_registry.py`）**明确失败**的形式暴露，不允许静默漂移。
>
> **配套材料**：设计总纲见 [TinyKV-CPP-设计思路与需求.md](../TinyKV-CPP-设计思路与需求.md) §8（测什么、达到什么）；
> 登记表机器解析标记为 §4 标记区内的一对 HTML 注释（registry:begin v1 … registry:end），
> 仅供检查脚本使用；本文其他位置一律不以字面形式书写该标记，人读以表格为准。

---

## 1. 参考项目调研结论（本方案的来源）

调研对象：[KVstorageBaseRaft-cpp](https://github.com/youngyangyang04/KVstorageBaseRaft-cpp)（教学 Raft-KV）与
[Mooncake](https://github.com/kvcache-ai/Mooncake)（KVCache 分离式存储，Apache-2.0）。结论按"继承 / 避开"归档。

### 1.1 KVstorageBaseRaft-cpp —— 反面教材：无框架、无注册的代价

其 `test/` 目录全部内容仅 4 个文件：`run.cpp`（boost 序列化 demo）、`defer_run.cpp`（defer 宏 demo）、
`format.cpp`（格式化 demo）、`测试文件运行说明.md`（手写"用 g++ 手动编译"的说明）。根 `CMakeLists.txt`
中没有任何 test 相关目标。真正的 Raft 行为验证靠 README 指导的手动流程：单机 fork 多进程 + `sleep` 对齐启动 + 人眼观察。

| 缺陷 | 后果 | 本项目对策 |
|---|---|---|
| 测试无框架（无断言库） | "测试"只是可运行 demo，失败无信号 | GoogleTest 全覆盖，断言可累积、可过滤 |
| 无注册（无 add_test/ctest） | 无法一条命令跑全部；CI 无从谈起 | 一切测试进 ctest，登记表管全量 |
| 无元数据 | 无法分层执行（快/慢、需网络/本地混跑） | 登记表 Metadata 列 + ctest LABEL 同源 |
| 真行为验证靠手动 fork+sleep | 白皮书作者被迫以"150 轮手跑"为纪律 | 集成随机测试脚本化、进夜跑 CI |

> 正面遗产仅两条：`.clang-format` / `format.sh` 的风格纪律，以及"测试文件运行说明"这种**把跑法写进文档**的意识——
> 本方案把该意识制度化成了登记表本身。

### 1.2 Mooncake —— 正面机制：ctest 中心化 + 元数据内建

Mooncake 是多模块大仓（store / transfer-engine / common / integration…），每个模块带自己的 `tests/` 目录，
测试体系围绕 **gtest + CTest** 展开：

1. **一测试文件一可执行，统一注册**：每模块 `tests/CMakeLists.txt` 用封装函数
   （`add_store_test` / `add_ha_test`）统一"建可执行 + 链接依赖 + `add_test(NAME x COMMAND x)`"，
   消除逐个手写链接库的漂移。
2. **ctest 属性即元数据**：`TIMEOUT 90`、`ENVIRONMENT "MC_USE_TENT=1"`、`SKIP_RETURN_CODE 77`
   （硬件缺失时测试自己返回 77 → ctest 记为 skipped 而非 failed）、`if(USE_CUDA)` 条件注册。
3. **同一二进制多种跑法 = 多条登记**：如 `replica_selection_test` 以默认 env 跑一遍，再以
   `--gtest_filter=...` + `MC_STORE_REPLICA_SCORING=1` 注册为独立的 `replica_selection_env_opt_in_test`。
   **Runner/Args 与测试代码解耦**——这正是"一条登记 = 一种执行方式"的范式。
4. **LABELS 供 CI 筛选**：`rdma`、`gpu`、`nvlink_gpu`、`integration` 等标签，CI 用
   `ctest --label-exclude` / `-L` 分流（无 RDMA 网卡的 PR 机器自动排除 `rdma` 标签）。
5. **目录按层级分**：`tests/e2e/`、`tests/ha/`、`tests/conf/`、`tests/utils/`、`tests/fault-tolerant/`，
   层级一眼可辨；共享 fixture/helper 以头文件与 test-peer 库形式复用
   （`master_service_test_fixture.h`、`test_server_helpers.h`）。
6. **chaos 工具族**：`chaosctl`（拉起多 master/client，随机 kill/重启，结束后出报告）、
   `chaos_test`（预定义故障场景）、`e2e_rand_test`（小时级随机长稳）——分布式正确性测试的成熟形态。
7. **硬件依赖分层外移**：需要 A10 GPU 服务器的 E2E 用例放 `scripts/tone_tests/`，在外部 T-One 平台跑，
   本地 CI 只保留可模拟部分。

### 1.3 本方案 = Mooncake 骨架 + 登记表补上它缺的一环

Mooncake 的事实来源仍然是**散落在各模块 CMakeLists.txt 的 add_test 调用**（200+ 处），
"哪些测试存在、为什么这么跑"没有集中视图，跨模块查测试要 grep。本方案在其骨架上加一层：

> **登记表（docs/testing.md §4）是唯一事实来源，CMake 注册是它的投影；两者由检查脚本双向对账，不一致即失败。**

---

## 2. 测试目录框架（目标布局）

```
TinyKV-White-Paper-main/
├── docs/
│   └── testing.md              # 本页：策略 + 登记表（唯一事实来源）
├── tools/
│   └── check_test_registry.py  # 仓库 ⇆ 登记表一致性检查（CI 门槛）
└── tests/                      # 测试总目录（M0 落地时创建）
    ├── testlib/                # 共享测试基建：fixture、模拟网络/时钟接口、线性一致性检查器封装
    ├── unit/                   # L1 单元：gtest，进程内，无网络无磁盘 IO（或临时目录）
    ├── simulation/             # L2 确定性模拟：虚拟时钟 + 虚拟网络，进程内，种子可复现
    ├── integration/            # L3 集成：多进程真网络，故障注入 + 操作历史落盘 + lin-check
    │   └── scripts/            #   集成测试 runner 脚本（bash/python，被 add_test 调起）
    └── bench/                  # L4 基准：QPS/延迟/恢复时间，产出数字，不做硬失败断言
```

**取代关系**：设计文档 §4.3 草稿中顶层 `bench/` 与 `tests/` 的拆分，自本文档起统一为 `tests/bench/`，
以便"所有可执行测试资产一个登记表管全"。设计文档处已加注记。

### 命名与注册规范（硬规则）

| 规则 | 内容 |
|---|---|
| 文件命名 | `<module>_<topic>_test.{cpp,sh,py}`，如 `raft_step_test.cpp`、`wal_crash_test.sh` |
| 一文件一职责 | 一个 `_test.cpp` 编译为一个可执行（gtest main）；一个 runner 脚本对应一类集成场景 |
| 注册入口 | CMake 中只允许通过项目封装函数 `add_tinykv_test(NAME <name> ...)` 注册（M0 落地，语义等同 Mooncake 的 `add_store_test`） |
| 一登记一跑法 | 同一二进制换参数/换环境 = 新起一行登记（继承 Mooncake 的 env_opt_in 范式），不共用登记 |
| 登记先行 | **新增测试文件或 add_test 的 PR，必须同时修改 §4 登记表**，并通过 §7 一致性检查 |

---

## 3. Level 分层定义（受控词表）

| Level | 定义 | 依赖 | 时长预算 | 触发 | 质量门槛 |
|---|---|---|---|---|---|
| `unit` | 单模块行为断言：Raft 状态机消息序列、WAL 记录编解码、CF 前缀编码、事务 CF 操作 | 进程内；临时目录可用 | 单测 ≤ 10s | 每次 PR | 全绿，零容忍 flaky |
| `simulation` | 确定性模拟器（虚拟时钟+虚拟网络：延迟/分区/丢包/重启），P2/P3 全特性先在此跑通 | 进程内，随机种子入参 | 单场景 ≤ 60s | 每次 PR | 全绿；失败可用种子精确复现 |
| `integration` | 多进程 3~5 节点真网络：随机 kill/分区/恢复，客户端历史落盘 → 离线线性一致性检查 | 独占端口/目录，需隔离 | 分钟~小时级 | 夜跑（`nightly`）；PR 只跑冒烟子集 | **里程碑交付：连续 150 轮无 FAIL**（设计文档 §8.1） |
| `bench` | 性能采数：批量提交曲线、ReadIndex 对比、崩溃恢复时间、split 耗时 | 独占机器防抖动 | 分钟级 | 夜跑 + 手动 | 不做硬失败；与设计文档 §3.4 PERF 指标对照出报告 |

Level 与 ctest LABEL 一一对应（`add_tinykv_test` 内部把 Level 写成 LABEL），
CI 用 `ctest -L unit|simulation|integration|bench` 选择，无需解析本页。

---

## 4. 登记表（唯一事实来源）

> **表格语法约定**（检查脚本按此解析）：
> - 每行 = 一个 ctest 测试 = 一种执行方式；列内多值用 `;` 分隔。
> - `Scope`：测试资产路径（相对仓库根，文件或目录；目录则覆盖其下全部 `*_test.*`）。
>   **Scope 列不加反引号**——检查脚本按字面路径匹配 Scope（仅 Runner 列用反引号提取 ctest 名）。
> - `Level`：§3 词表之一。
> - `Runner`：**首个反引号 token = 该行的 ctest 测试名（全表唯一）**，其后可跟 ` = ` 与 COMMAND 说明：
>   - gtest 可执行直跑（ctest 名 = 可执行名）：`` `raft_step_test` ``；
>   - 同一二进制的参数/脚本变体：`` `sim_election_s42` = sim_election_test ``、
>     `` `raft_chaos_n150` = bash tests/integration/scripts/run_chaos.sh ``。
> - `Args`：传给 COMMAND 的参数与环境变量（`VAR=x --flag=v`），无则写 `-`。
> - `Metadata`：§6 词表中的标签，`;` 分隔，必含一个速度标签（fast/slow/nightly）。

<!-- registry:begin v1 -->
| Scope | Level | Runner | Args | Metadata |
|---|---|---|---|---|
| tests/unit/common/strong_id_test.cpp | unit | `strong_id_test` | - | fast |
| tests/unit/common/encode_test.cpp | unit | `encode_test` | - | fast |
| tests/unit/common/common_log_test.cpp | unit | `common_log_test` | - | fast |
| tests/unit/common/compat_test.cpp | unit | `compat_test` | - | fast |
| tests/unit/proto/proto_roundtrip_test.cpp | unit | `proto_roundtrip_test` | - | fast |
| tests/unit/common/kv_error_test.cpp | unit | `kv_error_test` | - | fast |
| tests/unit/storage/memtable_test.cpp | unit | `memtable_test` | - | fast |
| tests/unit/storage/wal_test.cpp | unit | `wal_test` | - | fast |
| tests/unit/storage/sst_test.cpp | unit | `sst_test` | - | fast |
| tests/unit/storage/wal_crash_test.cpp | unit | `wal_crash_test` | - | fast |
| tests/unit/storage/wal_group_commit_test.cpp | unit | `wal_group_commit_test` | - | fast |
| tests/unit/raft/entry_batch_test.cpp | unit | `entry_batch_test` | - | fast |
| tests/unit/raft/confchange_test.cpp | unit | `confchange_test` | - | fast |
| tests/unit/raftstore/region_route_test.cpp | unit | `region_route_test` | - | fast |
| tests/unit/raftstore/split_test.cpp | unit | `split_test` | - | fast |
| tests/unit/raftstore/snapshot_chunk_test.cpp | unit | `snapshot_chunk_test` | - | fast |
| tests/unit/raftstore/meta_store_test.cpp | unit | `meta_store_test` | - | fast |
| tests/unit/storage/engine_get_put_test.cpp | unit | `engine_get_put_test` | - | fast |
| tests/unit/storage/engine_scan_test.cpp | unit | `engine_scan_test` | - | fast |
| tests/unit/storage/engine_cf_test.cpp | unit | `engine_cf_test` | - | fast |
| tests/unit/storage/engine_scan_concurrent_test.cpp | unit | `engine_scan_concurrent_test` | - | fast |
| tests/unit/cli/cli_local_smoke_test.cpp | unit | `cli_local_smoke` | - | fast |
| tests/bench/write_bench_test.cpp | bench | `write_bench` | - | nightly |
| tests/unit/raft/raft_log_test.cpp | unit | `raft_log_test` | - | fast |
| tests/unit/raft/applied_recovery_test.cpp | unit | `applied_recovery_test` | - | fast |
| tests/unit/storage/engine_compaction_test.cpp | unit | `engine_compaction_test` | - | fast |
| tests/unit/storage/durability_guard_test.cpp | unit | `durability_guard_test` | - | fast |
| tests/unit/raft/readindex_path_test.cpp | unit | `readindex_path_test` | - | fast |
| tests/unit/raft/raft_step_test.cpp | unit | `raft_step_test` | - | fast |
| tests/unit/raft/raft_kv_test.cpp | unit | `raft_kv_test` | - | fast |
| tests/unit/raft/transfer_leader_test.cpp | unit | `transfer_leader_test` | - | fast |
| tests/simulation/sim_chaos_test.cpp | simulation | `sim_chaos_test` | - | fast |
| tests/simulation/sim_confchange_test.cpp | simulation | `sim_confchange_test` | - | fast |
| tests/simulation/sim_scheduler_test.cpp | simulation | `sim_scheduler_test` | - | fast |
| tests/simulation/sim_transfer_test.cpp | simulation | `sim_transfer_test` | - | fast |
| tests/integration/tcp_cluster_test.cpp | integration | `tcp_cluster_test` | - | nightly |
| tests/integration/raft_chaos_n150_test.cpp | integration | `raft_chaos_n150` | - | nightly;serial |
| tests/integration/mr_chaos_test.cpp | integration | `mr_chaos_n150` | - | nightly;serial |
| tests/integration/lincheck_si_test.cpp | integration | `lincheck_si_test` | - | nightly |
| tests/integration/txn_chaos_test.cpp | integration | `txn_chaos_n150` | - | nightly;serial |
| tests/unit/cli/cli_status_test.cpp | unit | `cli_status_test` | - | fast |
| tests/unit/raft/readindex_test.cpp | unit | `readindex_test` | - | fast |
| tests/bench/read_bench_test.cpp | bench | `read_bench` | - | nightly |
| tests/integration/lincheck_readindex_test.cpp | integration | `lincheck_readindex` | - | nightly |
| tests/unit/cli/cli_cluster_get_test.cpp | unit | `cli_cluster_get_test` | - | fast |
| tests/unit/cli/cli_region_test.cpp | unit | `cli_region_test` | - | fast |
| tests/unit/cli/cli_txn_smoke_test.cpp | unit | `cli_txn_smoke` | - | fast |
| tests/unit/txn/tso_test.cpp | unit | `tso_test` | - | fast |
| tests/unit/txn/latch_concurrency_test.cpp | simulation | `latch_concurrency_test` | - | fast |
| tests/unit/txn/mvcc_test.cpp | unit | `mvcc_test` | - | fast |
| tests/unit/txn/si_semantics_test.cpp | unit | `si_semantics_test` | - | fast |
| tests/unit/txn/txn_resolve_test.cpp | unit | `txn_resolve_test` | - | fast |
| tests/unit/txn/txn_peer_test.cpp | unit | `txn_peer_test` | - | fast |
| tests/unit/server/lifecycle_test.cpp | unit | `lifecycle_test` | - | fast |
| tests/unit/server/worker_test.cpp | unit | `worker_test` | - | fast |
| tests/unit/server/server_app_test.cpp | unit | `server_app_test` | - | fast |
| tests/unit/server/interceptor_test.cpp | unit | `interceptor_test` | - | fast |
| tests/unit/server/peer_registry_test.cpp | unit | `peer_registry_test` | - | fast |
| tests/unit/server/http_frame_test.cpp | unit | `http_frame_test` | - | fast |
| tests/unit/server/multi_replica_test.cpp | unit | `multi_replica_test` | - | fast |
| tests/unit/server/lock_registry_test.cpp | unit | `lock_registry_test` | - | fast |
| tests/unit/server/auth_test.cpp | unit | `auth_test` | - | fast |
| tests/unit/txn/gc_test.cpp | unit | `gc_test` | - | fast |
| tests/integration/snapshot_test.cpp | integration | `snapshot_test` | - | nightly |
| tests/integration/split_meta_persistence_test.cpp | integration | `split_meta_persistence_test` | - | nightly |
| tests/integration/crash_recovery_test.cpp | integration | `crash_recovery_test` | - | nightly |
| tests/bench/cluster_bench_test.cpp | bench | `cluster_bench` | - | nightly |
| tests/bench/tcp_perf_bench_test.cpp | bench | `tcp_perf_bench` | - | nightly |
| tests/bench/split_bench_test.cpp | bench | `split_bench` | - | nightly |
| tests/bench/heartbeat_bandwidth_bench_test.cpp | bench | `heartbeat_bandwidth_bench` | - | nightly |
| tests/bench/snapshot_bench_test.cpp | bench | `snapshot_bench` | - | nightly |
<!-- registry:end -->

> **M0 起步批次**（2026-11-16 落地登记，见上表）：encode / kv_error / strong_id / common_log 四个
> common 单测（SPEC-0 §0.7 验收表）；其余测试随对应里程碑登记。备查原规划：
>
> | Scope | Level | Runner | Args | Metadata |
> |---|---|---|---|---|
> | `tests/unit/common/encode_test.cpp` | unit | `encode_test` | - | fast |
> | `tests/unit/common/kv_error_test.cpp` | unit | `kv_error_test` | - | fast |
> | `tests/unit/wal/wal_record_test.cpp` | unit | `wal_record_test` | - | fast |
> | `tests/unit/raft/raft_step_test.cpp` | unit | `raft_step_test` | - | fast |

**与 SPEC 的证据链联动**：各阶段规格（`docs/SPEC-0~4-*.md` §x.7）引用本表登记名构成
"需求 → 验收口径 → 登记测试 → 证据物"四环节证据链（规范见 SPEC-0 §0.7）。
SPEC 中出现的测试名在对应功能实现时**必须真实登记进本表**方可称为完成；检查脚本保证表与仓库不漂移。

---

## 5. 新增测试 SOP（强制流程）

1. **写测试**：按 §2 规范放入对应 Level 目录，命名 `<module>_<topic>_test.*`。
2. **CMake 注册**：用 `add_tinykv_test()` 封装函数注册进 ctest（禁止散装 `add_test`）。
3. **登记**：在 §4 表格标记区加一行，五列填全；同一二进制的每种跑法各一行。
4. **自检**：跑 `python tools/check_test_registry.py`，必须 PASS 才可提交。
5. **审查**：PR 模板含"已更新 docs/testing.md 登记表"勾选项；CI 中一致性检查失败即 PR 失败。

**示例（P2 时期典型登记）**：

```markdown
| tests/unit/raft/raft_step_test.cpp | unit | `raft_step_test` | - | fast |
| tests/simulation/sim_election_test.cpp | simulation | `sim_election_n3` = sim_election_test | `--seed=42 --nodes=3` | fast |
| tests/simulation/sim_election_test.cpp | simulation | `sim_election_n5` = sim_election_test | `--seed=777 --nodes=5` | fast |
| tests/integration/raft_chaos_test.py | integration | `raft_chaos_n150` = bash tests/integration/scripts/run_chaos.sh | `--nodes=3 --rounds=150` | nightly;needs-linux;serial;timeout:3600s |
```

> 注意：同一 `sim_election_test.cpp` 以不同参数注册为两个 ctest 测试名（`sim_election_n3` / `sim_election_n5`）——
> ctest 要求注册名唯一，"一登记一跑法"由此落地（继承 Mooncake `replica_selection_env_opt_in_test` 的范式）。

---

## 6. Metadata 词表（受控，禁止自造）

| 标签 | 语义 | 消费方 |
|---|---|---|
| `fast` | ≤ 60s，PR 必跑 | CI：PR job |
| `slow` | ≤ 30min，PR 可选 / 合并必跑 | CI：合并 job |
| `nightly` | 小时级或含随机长跑（150 轮纪律在此） | CI：夜跑 job |
| `needs-linux` | 依赖 Linux 语义（fdatasync 等），Windows 跳过 | CI：平台矩阵 |
| `serial` | 独占端口/目录/CPU，禁止与其测试并行（RUN_SERIAL） | ctest |
| `skippable` | 环境缺失时以退出码 77 优雅跳过而非失败（继承 Mooncake SKIP_RETURN_CODE） | ctest |
| `manual` | 交互式工具（chaosctl 类），不进任何 CI | 人 |
| `quarantine` | 已知 flaky 隔离观察中，不计入门槛，修复 PR 必须引用 issue | ctest + 人 |
| `timeout:<N>s` | ctest TIMEOUT 属性（秒） | ctest |

词表扩展规则：新增标签必须先在本节登记语义与消费方，再投入使用；检查脚本对未知标签报错。

---

## 7. 一致性检查（遗漏产生明确失败）

**工具**：`tools/check_test_registry.py`（Python 3 标准库，跨平台，不依赖构建）。

**检查项（双向对账）**：

| # | 方向 | 规则 | 失败信息 |
|---|---|---|---|
| 1 | 仓库 → 表 | 所有 `add_test(NAME ...)` 注册名必须等于某行 Runner 首个反引号 token（ctest 名） | `未登记测试: <name>（注册于 <cmake文件>）` |
| 2 | 仓库 → 表 | `tests/` 下所有 `*_test.{cpp,sh,py}` 必须被某行 Scope 覆盖 | `测试文件未登记: <path>` |
| 3 | 表 → 仓库 | 每行 Scope 指向的文件/目录必须存在 | `幽灵登记（路径不存在）: <scope>` |
| 4 | 表 → 仓库 | Runner 为脚本（`.sh`/`.py`）时文件必须存在 | `Runner 脚本不存在: <runner>` |
| 5 | 词表 | Level ∈ {unit, simulation, integration, bench}；Metadata ∈ §6 词表 | `非法 Level / 未知 Metadata 标签: <值>` |
| 6 | 结构 | 测试名唯一；每行 Metadata 至少含一个速度标签；标记区表格可解析 | 对应语法错误提示 |

**调用**：

```bash
python tools/check_test_registry.py            # 检查当前仓库
python tools/check_test_registry.py --root <路径>   # 检查任意仓库副本（CI/脚本复用）
```

**CI 挂接（M0 落地）**：
- PR job：编译前先跑本检查（秒级），失败即 PR 失败——遗漏在进入构建之前就被拦下；
- 夜跑 job：跑测试前再跑一次，防止夜跑窗口期间登记漂移；
- M0 时可加 CMake 自定义 target `check_test_registry` 便于本地 `cmake --build build --target check_test_registry`。

**失败输出示例**：

```
FAIL: 登记表与仓库不一致 (docs/testing.md §4)
  [E1] 未登记测试: sim_split_test（注册于 tests/simulation/CMakeLists.txt:41）
  [E2] 测试文件未登记: tests/unit/txn/prewrite_test.cpp
  [E3] 幽灵登记（路径不存在）: tests/unit/raft/raft_old_test.cpp
  [E5] 未知 Metadata 标签: very-fast（第 12 行）
共 4 处不一致；修复后重跑 python tools/check_test_registry.py
```

---

## 8. CI 分发策略（消费登记表的方式）

| Job | 选择器 | 内容 |
|---|---|---|
| PR（Windows + Linux 矩阵） | `ctest -L fast` + 平台排除 `needs-linux` | unit + simulation 全部、integration 冒烟子集 |
| 合并 | `+ slow` | 补跑慢速集成场景 |
| 夜跑（Linux） | `-L nightly` | 150 轮随机一致性测试、长稳、全量 bench 采数 |
| 手动 | `manual` | chaosctl 类交互工具，本机调试用 |

---

## 9. 缺陷回归测试（bugfix 阶段）

修 bug 留下的触发场景，最终形态就是本页 §4 登记过的一个回归测试。链条固定为：

`docs/bugs.md 一行（现象+条件+复现命令）` → `tests/ 下的用例（源码带 // BUG-NNNN: 标记）` → `本页 §4 一行登记` → `run_ci.py` 的 bugfix 与 registry 双绿

- **编号**：`BUG-NNNN` 四位递增、永不复用；`docs/bugs.md` 只增不删（更正另起一行写明取代关系）。
- **一个 bug 至少一个回归测试**；断言在不同层级（unit 抓不住就上 simulation，再不行 integration 随机注入），可登记多行。
- **复现命令写进登记表的「复现」列**，不要只留在对话里——上下文一断，没人能重跑那个场景。
- **同一次提交**：新增登记行 + 用例标记 + 本页登记行，三者缺一即 CI 红（bugfix / registry 任一处拦下）。
- 判定口径：`ci/rules.toml [bugfix]`、`tools/ci/check_bugfix.py`、[CI.md](../CI.md) §4 发现码表。

---

## 9a. 编译器与语言标准矩阵（支持面）

默认档是 **C++23 + g++**（`win-debug` / `linux-debug`）。兼容档的存在不是为了"多支持几个标准"，
而是为了暴露两类只在换工具链时才出现的问题：标准库设施是否存在（`std::expected`/`std::span`）、
以及不同编译器对同一段代码的告警判定是否一致。

| 档位 | 配置方式 | 覆盖点 |
|---|---|---|
| C++23（默认） | `cmake --preset win-debug` / `linux-debug` | `std::expected`、`std::span`、`std::unreachable`、`<=>` 原生可用；`-Wpedantic` 全开 |
| C++20 | `-DTINYKV_CXX_STANDARD=20`（preset `win-cxx20`/`linux-cxx20`） | 无 `std::expected`/`std::unreachable`，走 `src/common/compat.h`；`<=>` 原生可用 |
| C++17 | `-DTINYKV_CXX_STANDARD=17`（preset `win-cxx17`/`linux-cxx17`） | `expected`/`span`/`unexpected`/`starts_with`/`unreachable` 全部走 compat；`atomic<shared_ptr>` 降级为短锁 |
| 第二编译器 | `cmake --preset win-clang23` / `linux-clang23`（或 `CC=clang CXX=clang++`） | 告警口径差异（如 `-Wunused-private-field`）；protoc 生成代码单独降噪 |

约定与边界：

1. **源码只有一份**。上层只写 `tinykv::expected` / `tinykv::span` / `tinykv::unexpected` /
   `tinykv::as_bytes` / `tinykv::starts_with` / `tinykv::unreachable`，由 compat 决定直通 std 还是走兜底。
2. **17 档关 `-Wpedantic`**：指定初始化器（`.field = v`）在 17 档是编译器扩展，
   开着它等于把"能编译"改成"不能编译"。这是有意的、写进根 `CMakeLists.txt` 的取舍。
3. **新增代码不得引入 23/20 独有设施而不走 compat**：CI 的兼容矩阵会在 17 档直接编译失败。
4. **不允许"只在某个标准下跳过测试"**：三档跑同一份 `ctest`（含 `-L fast` 与 nightly）。

本地三条命令（等价于 CI 的 compat job）：

```bash
cmake --preset linux-cxx17
cmake --build --preset linux-cxx17 && ctest --test-dir build/linux-cxx17 -L fast
```

---

## 10. 变更记录

| 日期 | 变更 | 依据 |
|---|---|---|
| 2026-11-16 | 初版：调研两参考项目测试体系，确立"登记表唯一事实来源 + 双向一致性检查"范式 | KVstorageBaseRaft-cpp `test/` 反例调研 + Mooncake `*/tests/` 机制调研（§1） |
| 2026-11-16 | 新增 §9 缺陷回归测试：场景 → 用例 → 登记 → CI 四段链，bugfix 阶段强制执行 | 用户指令（修 bug 必须把触发场景留成回归测试）+ `ci/rules.toml [bugfix]` |
| 2026-11-18 | 新增 §9a 编译器与语言标准矩阵：C++17/20/23 × g++/clang++，兼容层 `src/common/compat.h` | 用户指令（拓宽适用性：默认 23，17 兜底）+ 三档实测 66/66 |
