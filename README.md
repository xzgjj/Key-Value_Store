# TinyKV-CPP

> 用现代 C++23 从零实现的 mini-TiKV：单机 KV → Raft → Multi-Raft → Percolator 分布式事务，
> 服务端、命令行、嵌入 API 三件套齐备，正确性由 60 个 ctest 与 CI 门禁背书。

[![TinyKV-CI](https://github.com/xzgjj/Key-Value_Store/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xzgjj/Key-Value_Store/actions/workflows/ci.yml)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![License](https://img.shields.io/badge/license-Apache--2.0-blue)

这是一个可以真正跑起来的系统级项目，不是跟着教程抄一遍的练习。它把 TiKV 的核心链路
——自研存储引擎、Raft 共识、Multi-Raft 分片、Percolator 事务——压缩到一万行量级，
每个模块都能从入口文件读到末尾，每条设计取舍都写进了对应阶段的实现笔记。

## 1. 项目定位

| 维度 | 说明 |
|---|---|
| 做什么 | 单机引擎 → 3 节点 RaftKV → Multi-Raft（Region/Split/成员变更/快照/调度）→ 快照隔离事务，四阶段逐层递进 |
| 形态 | `tinykv-server` 常驻进程 + `tinykv-cli` 命令行 + 可嵌入的 C++ API（见 [examples/embed_example.cpp](examples/embed_example.cpp)） |
| 规模 | `src/` 约 9.1k 行 C++，`tests/` 约 8.5k 行，60 个测试可执行，`ctest` 一条命令跑全量 |
| 不追求 | 与 TiKV/etcd 比生产成熟度：多租户、在线升级、安全鉴权、十万级 Region 调度都不在范围内 |
| 依据 | 每个里程碑有规格文档与验收表，验收项必须指向真实存在的测试才能算完成 |

## 2. 能力清单

| 层 | 已实现的内容 | 入口 |
|---|---|---|
| 存储引擎 | 跳表 memtable + 追加写 WAL + SST，Default/Write/Lock 三列族，`fsync_on_commit` 可配，掉电后按 WAL 重放恢复 | [src/storage/engine.h](src/storage/engine.h) |
| Raft | 选举、日志复制、快照与日志压缩、ReadIndex 线性一致读、LeaseRead（默认关）、单步成员变更 + Learner、Leader Transfer | [src/raft/raft.h](src/raft/raft.h) |
| Multi-Raft | Region 元数据与区间路由、epoch 校验、Split（走共识 + 数据迁移）、4MB 分块快照 + 逐块 CRC、PD-lite size 均衡调度 | [src/raftstore/region.h](src/raftstore/region.h) |
| 分布式事务 | TSO 严格单调授时、Latch 单行原子性、`MvccTxn` 三列族 prewrite/commit/rollback、锁决议三态、GC 安全点 | [src/txn/mvcc.h](src/txn/mvcc.h) |
| 运行时 | `ServerApp` 组合根、`Service` 生命周期（顺序启动/逆序停止）、Worker 泵循环、会话级事务、Interceptor 链、信号优雅关闭 | [src/server/server_app.h](src/server/server_app.h) |
| 客户端 | 本地读写/扫描/状态、集群模式读写、Region 观测（`regions`/`region peers`/`region split`）、region trace 开关、交互式事务 | [src/cli/cli.cpp](src/cli/cli.cpp) |

## 3. 快速开始

### 3.1 环境要求

| 项 | 要求 |
|---|---|
| 编译器 | 支持 C++23：Windows 走 MSYS2 ucrt64 的 g++，Linux 走系统 g++ |
| 构建工具 | CMake ≥ 3.25（preset 文件为 version 6）、Ninja |
| 第三方库 | protobuf、spdlog、asio、unordered_dense；gtest 由 CMake FetchContent 固定版本拉取 |
| 脚本运行 | Python 3.11+（构建与 CI 脚本只用标准库，不额外装依赖） |
| 依赖来源 | 本机用 MSYS2 pacman，CI/MSVC 走 vcpkg 清单（[vcpkg.json](vcpkg.json)，需 `VCPKG_ROOT`） |

### 3.2 构建与测试

```bash
python scripts/build.py --preset win-debug     # Windows；Linux 换成 linux-debug
python scripts/build.py --preset win-debug --label fast    # 只跑快测
python scripts/build.py --preset win-debug --skip-build    # 跳过构建只跑测试
python scripts/build.py --list                 # 列出全部 preset
```

脚本做三件事：`cmake --preset` 配置、`cmake --build --preset` 构建、`ctest` 跑测试，
最后把结果写成 JUnit 报告到 `build/evidence/junit.xml`。也可以直接用 CMake：

```bash
cmake --preset win-debug
cmake --build --preset win-debug
ctest --test-dir build/win-debug -L fast --output-on-failure
```

构建产物在 `build/<preset>/` 下：`src/server/tinykv-server`、`src/cli/tinykv-cli`、
`examples/embed_example`。

### 3.3 跑一个服务端，用 CLI 读写

```bash
# 终端 1：启动 daemon（数据目录 + HTTP 端口，就绪后会打印实际监听地址）
./build/win-debug/src/server/tinykv-server ./data 3380
```

```bash
# 终端 2：本地模式（不需要服务端，直接操作引擎文件）
./build/win-debug/src/cli/tinykv-cli --config demo.yaml put user:1 alice
./build/win-debug/src/cli/tinykv-cli --config demo.yaml get user:1

# 集群模式：状态、region 观测、事务
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 status
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 regions
SID=$(./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 txn begin | sed 's/session=//')
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 txn set "$SID" k v
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 txn commit "$SID"
```

退出码是契约的一部分（0 成功 / 1 用法错误 / 2 运行错误 / 3 key 不存在），脚本化调用只依赖它，
不要解析输出文本。完整子命令、参数与配置加载规则见 [docs/cli.md](docs/cli.md)。

### 3.4 当作组件嵌入

```cpp
auto engine = Engine::open({.dir = "./embed_demo.db", .fsync_on_commit = true});
ModifyVec batch{Modify{Put{.cf = Cf::Default, .key = "k1", .value = "v1"}}};
(*engine)->write(batch);
auto got = (*engine)->get(Cf::Default, std::as_bytes(std::span{"k1", 2}));
```

完整可编译版本见 [examples/embed_example.cpp](examples/embed_example.cpp)——它随 CI 一起编译，
接口改了却忘了改示例，CI 会直接红。

## 4. 架构

```
        tinykv-cli（HTTP）               嵌入 API（C++20/23）
              │                                  │
    ┌─────────▼──────────────────────────────────▼─────────┐
    │ server    StatusServer · TxnSession · Worker · Interceptor │
    ├──────────────────────────────────────────────────────┤
    │ raftstore  Peer · Region 路由/epoch · Split · 快照分块      │
    ├───────────────────────────┬──────────────────────────┤
    │ raft  选举/复制/快照/压缩    │ txn  MVCC · Latch · TSO    │
    ├───────────────────────────┴──────────────────────────┤
    │ storage  Engine：跳表 memtable + 追加写 WAL + SST         │
    └──────────────────────────────────────────────────────┘
              common：错误类型 · 编码 · CRC32C · 日志
```

| 目录 | 职责 |
|---|---|
| [src/common](src/common) | 结构化错误（含"发生了什么/为什么/怎么办"三要素）、变长编码、CRC32C、日志封装 |
| [src/config](src/config) | 最小 YAML 子集解析：单文件显式路径、未知键报错、不做隐式兜底 |
| [src/storage](src/storage) | 存储引擎：memtable、WAL、SST、三列族隔离 |
| [src/raft](src/raft) | Raft 状态机、日志与持久化状态 |
| [src/raftstore](src/raftstore) | Peer 应用循环、Region 区间索引、快照与传输抽象 |
| [src/txn](src/txn) | MVCC 键编码、Latch、事务客户端与执行器 |
| [src/scheduler](src/scheduler) | TSO 授时与 PD-lite 调度器 |
| [src/server](src/server) | 组合根、HTTP 状态/读写端点、Worker、Interceptor、会话管理 |
| [src/cli](src/cli) | 命令行客户端（本地 + 集群） |
| [proto](proto) | 请求/响应与错误消息定义 |
| [tests](tests) | 四层测试：unit / simulation / integration / bench |
| [tools/ci](tools/ci) | 六阶段检查器、登记表对账、自测 |
| [scripts](scripts) | 统一构建入口 |

### 关键设计取舍

- **每 Region 一份独立引擎**：Raft 日志以固定前缀存于 Default 列族，两个 Peer 共用一份引擎
  会写同一把日志键，必然串日志；因此 region 各自 `data_dir/regions/<id>`，初始 region 用根目录兼容旧布局。
- **读走 ReadIndex，LeaseRead 默认关闭**：读不进日志是线性一致读的性能前提；LeaseRead 作为可选降级，
  避免时钟假设默认生效。
- **单步成员变更 + Learner，不做 Joint Consensus**：与 TiKV 同路线，复杂度低一个量级，
  安全性由"变更期间不丢已提交数据"的测试守住。
- **MVCC 三列族 + primary key 决议**：Default 存数据、Write 存提交记录、Lock 存未提交锁，
  冲突即回滚自身，不做死锁检测。
- **生命周期显式化**：组件顺序启动、逆序停止、失败回滚；线程用 joinable + 原子停止标志 + 条件变量，
  不用 detached 线程，也不靠 sleep 对齐启动。
- **粗粒度串行闸**：全部 Peer/Storage 访问由一把锁串行化——这是课程级复杂度下的自觉取舍，
  并发优化留到确有数据支撑时再做。

## 5. 工程质量

提交前跑一条命令：`python tools/ci/run_ci.py`。它按六个阶段检查，退出码 0 绿 / 1 可判定问题 / 2 需要人工批准。

| 阶段 | 检查什么 |
|---|---|
| docs | 文档预算、跨文档重复段落、相对链接与脚本引用、测试登记表对账 |
| bugfix | 修 bug 必须留下触发场景、可执行复现命令、已登记的回归测试与 `BUG-NNNN` 标记 |
| guard | 规则资产被改动即拦截，防"顺手把门禁关掉" |
| scope | 任务改动声明、路径白名单、diff 预算、测试删除告警 |
| code | CMake 构建（`-Wall -Wextra -Werror`）、`ctest -L fast`、函数长度/复杂度、复制粘贴检测 |
| align | C++ 错误变体 ⇄ proto 消息对称性、文档里写的命令对象是否存在、违禁模式扫描 |

测试分四层，用 ctest 标签选择，登记表是唯一事实来源：

| 层 | 内容 | 触发 |
|---|---|---|
| unit | 单模块行为断言，进程内，单测 ≤ 10s | 每次提交 |
| simulation | 确定性模拟：虚拟时钟 + 虚拟网络，种子可复现 | 每次提交 |
| integration | 多进程真网络 + 故障注入 + 离线线性一致性检查 | 夜跑；里程碑交付需连续 150 轮无 FAIL |
| bench | QPS/延迟/恢复时间/split 耗时/快照吞吐采数，出报告不做硬失败 | 夜跑 + 手动 |

新增测试必须同时改 [docs/testing.md](docs/testing.md) §4 登记表，双向对账脚本会拦下漏登；
修 bug 必须把触发场景留成回归测试并登记进 [docs/bugs.md](docs/bugs.md)。细节见
[CI.md](CI.md)（怎么跑、失败怎么办）与 [ci/rules.toml](ci/rules.toml)（阈值与规则的机器可读来源）。

## 6. 仓库导航

| 文档 | 内容 |
|---|---|
| [docs/README.md](docs/README.md) | 上游 Talent Plan TinyKV 白皮书四篇笔记（Project1–4）的阅读入口 |
| [docs/notes-m0.md](docs/notes-m0.md) ~ [notes-m6](docs/notes-m6.md) | 逐阶段实现笔记：架构决策、踩过的坑、已知限制 |
| [docs/SPEC-0-foundation.md](docs/SPEC-0-foundation.md) ~ [SPEC-4](docs/SPEC-4-transaction.md) | 分阶段规格与验收表（需求 → 验收口径 → 登记测试 → 证据物） |
| [docs/testing.md](docs/testing.md) | 测试策略与登记表，测试相关问题的唯一事实来源 |
| [docs/cli.md](docs/cli.md) | CLI 对外契约：子命令、参数、退出码、配置加载 |
| [docs/bugs.md](docs/bugs.md) | 缺陷回归登记（只增不删） |
| [docs/review-log.md](docs/review-log.md) / [docs/reuse-register.md](docs/reuse-register.md) | 评审记录与参考项目复用台账 |
| [tests/README.md](tests/README.md) | 测试目录结构与各层跑法 |
| [CI.md](CI.md) | CI 操作手册：命令、退出码、发现码 → 处置动作 |

> 需求总纲（`TinyKV-CPP-设计思路与需求.md`）与 CI 规则人读镜像（`SPEC.md`）只保留在开发机上，
> 未纳入版本库；对外以 `docs/` 与 `ci/rules.toml` 为准。

## 7. 里程碑与分支

分支号即阶段号，`main` 始终与最新交付分支同步。

| 里程碑 | 内容 | 分支 | 规格 |
|---|---|---|---|
| M0 基建 | 骨架、common 三库、proto、CI 六阶段、测试登记机制 | `0` | [SPEC-0](docs/SPEC-0-foundation.md) |
| M1 单机 | 存储引擎、Raw API、本地 CLI、嵌入示例 | `1` | [SPEC-1](docs/SPEC-1-standalone.md) |
| M2 Raft | 3 节点 RaftKV、快照与日志压缩、客户端去重 | `2` | [SPEC-2](docs/SPEC-2-raft-kv.md) |
| M3 读优化 | ReadIndex / LeaseRead、读基准与线性一致读验证 | `3` | [SPEC-2](docs/SPEC-2-raft-kv.md) 下篇 |
| M4 Multi-Raft | Region 路由、Split、成员变更、快照分块、调度器、region 观测命令 | `3` → `4` | [SPEC-3](docs/SPEC-3-multi-raft.md) |
| M5 事务 | Percolator 快照隔离全套：TSO、Latch、MVCC、锁决议、GC | `4` | [SPEC-4](docs/SPEC-4-transaction.md) |
| M6 运行时 | daemon 组合根、会话级事务、Interceptor 链、region→peer 动态注册表 | `4`（最新） | [notes-m6](docs/notes-m6.md) |

## 8. 已知限制

如实记录，不伪装完成度。这些是当前实现的边界，也是后续工作的起点：

- **跨 Region 事务**：单个事务的 primary 与写入键若落在不同 Region，会分别路由到不同 Peer，
  课程级未提供跨 Region 的 2PC 协调器；当前保证单 Region 事务正确。
- **Split 元数据不持久化**：重启后按单 Region 重建路由，已 split 出的子目录数据不会被自动纳管。
- **GC 安全点**取活跃会话的最小 start_ts，无会话时取当前 TSO；锁冲突解析后的版本裁剪语义见 SPEC-4。
- **并发与安全边界**：一把粗粒度串行闸，无鉴权、无 TLS，端口默认只绑 `127.0.0.1`，适合本机实验与学习。
- **非目标**：Region Merge、完整 PD 调度策略、Joint Consensus、悲观事务、RDMA/GPU 传输。

## 9. 参与与约定

- **文档先于实现**：CLI 子命令、退出码、配置项以 [docs/cli.md](docs/cli.md) 为准，改实现前先改文档。
- **任务先声明**：动手前写 `ci/task_scope.json`（任务、路径、理由），超出声明范围或 diff 超预算会被拦下。
- **测试先登记**：新增或调整测试，同一次提交里改 [docs/testing.md](docs/testing.md) §4。
- **缺陷先复现**：修 bug 的第一件事是把触发场景写成能重跑的命令，并留成回归测试。
- **CI 必须绿**：`python tools/ci/run_ci.py` 退出码非 0 不提交；修复重试超过两轮就停下找人。
- **代码风格**：[.clang-format](.clang-format) 与 [.clang-tidy](.clang-tidy) 是唯一标准，缩进、命名不必另行讨论。

## 10. 许可

Apache License 2.0，见 [LICENSE](LICENSE)。上游 [Talent Plan TinyKV](https://github.com/tidb-incubator/tinykv)
与 [TinyKV 白皮书](https://github.com/Smith-Cruise/TinyKV-White-Paper) 为学习材料与设计参照，
本项目是独立的 C++ 重实现，不复用其源码。
