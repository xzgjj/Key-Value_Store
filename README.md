# TinyKV-CPP

> 用现代 C++23 从零实现的 mini-TiKV：单机 KV → Raft → Multi-Raft → Percolator 分布式事务，  
> 服务端、命令行、嵌入 API 三件套齐备，正确性由 60 个 ctest 与 CI 门禁背书。

![TinyKV-CI](https://github.com/xzgjj/Key-Value_Store/actions/workflows/ci.yml/badge.svg?branch=main)

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)

![License](https://img.shields.io/badge/license-Apache--2.0-blue)

这是一个可以真正跑起来的系统级项目，不是跟着教程抄一遍的练习。它把 TiKV 的核心链路  
——自研存储引擎、Raft 共识、Multi-Raft 分片、Percolator 事务——压缩到一万行量级，  
每个模块都能从入口文件读到末尾，每条设计取舍都写进了对应阶段的实现笔记。

## 1. 项目定位

**做什么**：从单机存储引擎起步，往上叠三节点 RaftKV，再扩成 Multi-Raft（Region 路由、Split、成员变更、快照、调度），最后实现快照隔离事务。四层依次递进，后一层建立在前一层之上，不是四个互不相干的 demo。

**以什么形态交付**：一个 `tinykv-server` 常驻进程、一个 `tinykv-cli` 命令行工具，以及一个可以嵌进别的程序的 C++ API——最小示例见 [examples/embed\_example.cpp](examples/embed_example.cpp)，编译进 CI，接口一改就红。

**规模**：`src/` 约 9.1k 行 C++，`tests/` 约 8.5k 行，60 个测试可执行；`ctest` 一条命令跑全量，不需要手动拼编译参数。

**不追求什么**：不跟 TiKV/etcd 比生产成熟度。多租户、在线升级、安全鉴权、十万级 Region 调度都不在范围内；这个项目要回答的是"内核骨架到底怎么造"，而不是"怎么撑住一个云上的集群"。

**凭什么说完成**：每个阶段都有规格与验收表，验收项必须指向仓库里真实存在的测试才算闭合。做不到的事也会写进已知限制，不拿"跑通了"冒充"做完了"。

## 2. 能力清单

| 层          | 已实现的内容                                                                                     | 入口                                                  |
| ---------- | ------------------------------------------------------------------------------------------ | --------------------------------------------------- |
| 存储引擎       | 跳表 memtable + 追加写 WAL + SST，Default/Write/Lock 三列族，`fsync_on_commit` 可配，掉电后按 WAL 重放恢复      | [src/storage/engine.h](src/storage/engine.h)        |
| Raft       | 选举、日志复制、快照与日志压缩、ReadIndex 线性一致读、LeaseRead（默认关）、单步成员变更 + Learner、Leader Transfer            | [src/raft/raft.h](src/raft/raft.h)                  |
| Multi-Raft | Region 元数据与区间路由、epoch 校验、Split（走共识 + 数据迁移）、4MB 分块快照 + 逐块 CRC、PD-lite size 均衡调度             | [src/raftstore/region.h](src/raftstore/region.h)    |
| 分布式事务      | TSO 严格单调授时、Latch 单行原子性、`MvccTxn` 三列族 prewrite/commit/rollback、锁决议三态、GC 安全点                 | [src/txn/mvcc.h](src/txn/mvcc.h)                    |
| 运行时        | `ServerApp` 组合根、`Service` 生命周期（顺序启动/逆序停止）、Worker 泵循环、会话级事务、Interceptor 链、信号优雅关闭            | [src/server/server\_app.h](src/server/server_app.h) |
| 客户端        | 本地读写/扫描/状态、集群模式读写、Region 观测（`regions`/`region peers`/`region split`）、region trace 开关、交互式事务 | [src/cli/cli.cpp](src/cli/cli.cpp)                  |

## 3. 快速开始

### 3.1 环境要求

| 项    | 要求                                                                           |
| ---- | ---------------------------------------------------------------------------- |
| 编译器  | 支持 C++23：Windows 走 MSYS2 ucrt64 的 g++，Linux 走系统 g++                          |
| 构建工具 | CMake ≥ 3.25（preset 文件为 version 6）、Ninja                                     |
| 第三方库 | protobuf、spdlog、asio、unordered\_dense；gtest 由 CMake FetchContent 固定版本拉取       |
| 脚本运行 | Python 3.11+（构建与 CI 脚本只用标准库，不额外装依赖）                                          |
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

### 3.3 跑服务端，CLI 读写

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

### 3.4 组件嵌入

```cpp
auto engine = Engine::open({.dir = "./embed_demo.db", .fsync_on_commit = true});
ModifyVec batch{Modify{Put{.cf = Cf::Default, .key = "k1", .value = "v1"}}};
(*engine)->write(batch);
auto got = (*engine)->get(Cf::Default, std::as_bytes(std::span{"k1", 2}));
```

完整可编译版本见 [examples/embed\_example.cpp](examples/embed_example.cpp)——它随 CI 一起编译，  
接口改了却忘了改示例，CI 会直接红。

## 4. 架构

一层一层往下看：最上面是两种入口，CLI 走 HTTP，嵌入 API 直接调 C++ 接口；往下是 `server` 层，它把 StatusServer、会话状态、Worker 泵和 Interceptor 链组装成一个常驻进程；再往下是 `raftstore`，每个 Region 由一个或多个 Peer 承载，负责区间路由、epoch 校验、Split 和快照分块；Peer 的两个依赖并行展开——`raft` 提供选举、复制、快照和日志压缩，`txn` 提供 MVCC、Latch 和 TSO；最底下是 `storage`，跳表 memtable 接追加写 WAL，落盘为 SST，三列族在这里物理隔离；`common` 横切全部层次，提供结构化错误、变长编码、CRC32C 和日志封装。

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

目录对应关系一句话一个：`src/common` 放结构化错误与编码；`src/config` 解析最小 YAML 子集，未知键直接报错；`src/storage` 是 memtable、WAL、SST 和三列族隔离；`src/raft` 是状态机与日志持久化；`src/raftstore` 承载 Peer 应用循环、Region 区间索引和传输抽象；`src/txn` 做 MVCC 键编码、Latch 与事务执行器；`src/scheduler` 放 TSO 和 PD-lite 调度器；`src/server` 是组合根、HTTP 端点、Worker 和会话管理；`src/cli` 是本地加集群两种模式的命令行；`proto` 定义请求响应与错误消息；`tests` 按 unit / simulation / integration / bench 四层组织；`tools/ci` 是六阶段检查器与登记表对账；`scripts` 只有统一构建入口。

### 关键设计

**每个 Region 一份独立引擎，是为了不让 Raft 日志互相踩踏。** Raft 日志以固定前缀 `\0re` / `\0lm` 存在 Default 列族里，每个 Peer 的日志都从 index 1 开始。如果两个 Peer 共用一份 Engine，它们会写同一把日志键，后写覆盖先写，回放时必然串日志。因此每个 Region 有自己的 `data_dir/regions/<id>`，初始 Region 1 沿用 data\_dir 根目录以兼容旧布局。Split 成功时，apply 路径上的 observer 同步创建右半 Region 的 Engine 和 Peer，并把落在 `[split_key, end)` 的用户数据迁过去：跳过 `\0` 前缀的内部键，Default/Write 去掉末尾 8 字节大端 ts 得到用户键，Lock 键本身就是用户键。新 Peer 从空日志起步，状态机里只有迁过来的数据，和路由表始终一致。

**读请求走 ReadIndex，LeaseRead 默认关闭。** 线性一致读的代价必须付在正确的地方：写要进 Raft 日志，读不必。ReadIndex 先确认当前 leader 和读索引，再在本地状态机上读，既拿到线性一致性又省掉一次日志提交。LeaseRead 更快，但依赖时钟漂移假设，因此只作为显式开启的降级选项，不让默认行为建立在未经验证的前提上。

**成员变更用单步变更加 Learner，不做 Joint Consensus。** 新增节点先以 Learner 身份追日志，追平后再提升为 Voter；移除节点同样走单步变更，apply 时才生效。这条路线与 TiKV 一致，比联合共识低一个复杂度量级，而安全性——变更期间不丢已提交数据、Leader Transfer 不丢提案——由对应测试守住。

**事务把一致性问题收在三列族里。** Default 存最新数据，Write 存提交记录，Lock 存未提交锁。Prewrite 先写 Lock 和 Default，Commit 搬一条 Write 记录并清锁，冲突按 primary key 决议：写写冲突直接回滚自身，不做死锁检测。Latch 在 key 粒度上串行化单行写入，补上"单行原子性"这个 Percolator 的前提；TSO 严格单调授时保证快照一致；锁超时由 CheckTxnStatus / BatchRollback / ResolveLock 三件套收敛。GC 取活跃会话的最小 start\_ts 作为安全点，广播到所有 Region 各自走共识清理旧版本。

**生命周期显式化，不让线程自己管自己。** `Service` 契约规定顺序启动、逆序停止、失败回滚，`ServerApp` 是唯一创建并连接全部组件的组合根：config → TSO → Storage×N → Peer×regions → Worker → StatusServer。收到 SIGINT/SIGTERM 时先让 HTTP 拒绝新请求，再依次收 Worker、Peer、Engine。后台线程用 joinable 加原子停止标志和条件变量，异常被泵循环吞掉而不是杀死线程；不用 detached 线程，也不靠 sleep 对齐启动顺序。

**一把粗粒度串行闸是当前的自知取舍。** 所有 Peer 和 Storage 访问由 `peer_mu_` 串行化，HTTP handler、后台泵、事务执行器共用它。这把锁让正确性容易论证、死锁面容易排查，代价是吞吐上限明显；并发优化留到有 bench 数据指出瓶颈时再做，而不是先拆锁再找 bug。

## 5. 已知限制

如实记录，不伪装完成度。这些是当前实现的边界，也是后续工作的起点：

- **跨 Region 事务**：单个事务的 primary 与写入键若落在不同 Region，会分别路由到不同 Peer，  
  课程级未提供跨 Region 的 2PC 协调器；当前保证单 Region 事务正确。
- **Split 元数据不持久化**：重启后按单 Region 重建路由，已 split 出的子目录数据不会被自动纳管。
- **GC 安全点**取活跃会话的最小 start\_ts，无会话时取当前 TSO；锁冲突解析后的版本裁剪语义见 SPEC-4。
- **并发与安全边界**：一把粗粒度串行闸，无鉴权、无 TLS，端口默认只绑 `127.0.0.1`，适合本机实验与学习。
- **非目标**：Region Merge、完整 PD 调度策略、Joint Consensus、悲观事务、RDMA/GPU 传输。

## 6. 许可

Apache License 2.0，见 [LICENSE](LICENSE)。上游 [Talent Plan TinyKV](https://github.com/tidb-incubator/tinykv)  
与 [TinyKV 白皮书](https://github.com/Smith-Cruise/TinyKV-White-Paper) 为学习材料与设计参照，  
本项目是独立的 C++ 重实现，不复用其源码。
