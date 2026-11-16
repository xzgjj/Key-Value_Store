# TinyKV-CPP 设计思路、需求与目标（v0.2）

> **输入材料**
> 1. [TinyKV 白皮书](README.md)（PingCAP Talent Plan TinyKV 四个 Project 的完整实现笔记）
> 2. [KVstorageBaseRaft-cpp](https://github.com/youngyangyang04/KVstorageBaseRaft-cpp)（C++ 教学 Raft-KV，已做行级调研，见 §7）
> 3. [Mooncake](https://github.com/kvcache-ai/Mooncake)（KVCache 分离式存储，Apache-2.0，已做组件级调研，见 §7）
> 4. 工作区 [现代C++API设计原则](../docs/现代C++API设计原则.md)（接口设计硬约束，见 §4.5）
>
> **版本**：v0.2 —— 相比 v0.1：新增 Why/What 定位章节、用户场景、易用性需求（UX）、三级性能标准（S0/S1/S2），
> 全部需求编号化，里程碑与验收单直接引用编号。

---



## 0. 速览（一屏版）

| 问题 | 答案 |
|---|---|
| **做什么** | 现代 C++（C++23）实现的 mini-TiKV：Raft 强一致 KV → Multi-Raft → Percolator 事务，四阶段演进 |
| **为什么** | 领域知识只在实现里；两个参考项目之间正好空着"正确性站得住、代码读得透的小型 C++ 分布式 KV"（§1） |
| **做到什么** | 正确性有 150 轮随机故障注入背书；性能有三级量化标准；每个字节自己能解释。不追 TiKV/etcd 的生产成熟度（§2） |
| **正确性底线** | 线性一致、crash-safe、快照/成员变更/Split 安全、SI 事务（§3.5 REL-01~06） |
| **性能标准** | 三级量化：S0 交付底线 / S1 里程碑目标 / S2 挑战，含测量方法（§3.4 PERF-01~09） |
| **易用性** | 5 分钟上手、CLI、三要素错误信息、可观测性（§3.3 UX-01~07） |
| **关键决策** | C++23 + asio/protobuf + spdlog/gtest + 自研跳表引擎 + 追加写 WAL + ReadIndex + 单步成员变更（§6） |
| **里程碑** | M0 基建 → M1 单机 → M2 Raft → M3 读优化 → M4 Multi-Raft → M5 事务（§9） |

---



## 1. 为什么做（Why）

### 1.1 这个领域的知识只存在于实现里

白皮书作者的路线图本身就是证据：6.824 做完了，TinyKV 仍花了 119 小时，其中 Project3 单独三周。
看论文、读文档只能到"知道原理"；只有亲手处理过快照链路、被 split 的随机 BUG 折磨过，才真正拥有这些知识。
做 C++ 版比照着 Go 版走学得更彻底——崩溃恢复、内存生命周期、锁顺序、跨平台 fsync，
Go 帮你藏起来的东西全部要自己面对。

### 1.2 两个参考项目之间正好空着一个位置

| 项目 | 它是什么 | 它不是什么 |
|---|---|---|
| KVstorageBaseRaft-cpp | "入门理解 Raft"的教学演示：能跑、有完整共识流程 | crash-safe 实际是坏的（Persister 构造即清空持久化文件，恢复必读空）；读走完整共识；无成员变更、无分片 |
| Mooncake | 生产级 LLM KVCache 分离式存储 | 不可变对象、best-effort 副本、租约过期读会失败——根本不是通用强一致 KV |

两者之间空着的正是：**一个正确性站得住、代码规模读得透的小型 C++ 分布式 KV**。没有现成的，就自己造。

### 1.3 做完的东西有真实归属权

- 工作区现有项目（mempool、unordered_dense、ledger）都是组件级，这是第一个系统级项目，
  并把已有组件真的用起来（unordered_dense 进路由/去重热点路径）；
- 产物可嵌入：给其他项目当元数据存储、KV 层，将来想学 Mooncake 加 KVCache 接口也有骨架；
- 每个决定都有依据（为什么读走 ReadIndex、为什么 WAL 不能全量重写、为什么单步成员变更），
  这是"能讲 40 分钟的项目"，不是"跑通了某个教程"。

---

## 2. 做到什么（What）

### 2.1 有把握做到的（每项有证据，不是感觉）

| 能力 | 程度 | 证据方式 |
|---|---|---|
| 正确性 | 线性一致、crash-safe、Split、成员变更、SI 事务 | 随机故障注入连续 150 轮无 FAIL（§8）——与教学实现的本质区别就在这条 |
| 性能 | 达成 §3.4 的 S1 档量化标准 | 基准曲线（bench/ 进仓库，可复跑） |
| 易用性 | 5 分钟上手、CLI 可用、错误信息可自助排障 | UX-01~07 验收清单（§3.3） |
| 可读性 | 1~2 万行，每个模块能从头讲清楚 | 不需要证明，它本身就是目的 |

### 2.2 明确做不到、也不追求的

| 不追求 | 原因 |
|---|---|
| 与 TiKV/etcd 比生产成熟度 | 多租户、在线升级、安全、十万级 region 调度是几十人年的工程 |
| RocksDB 量级的磁盘效率 | 自研引擎的目标是"贴合自己的上层"，不是 I/O 极限 |
| 解决 Mooncake 的问题 | 不做 GPU 集群内存池、LLM 缓存语义（但保留将来加接口的插槽） |

### 2.3 一句话定位

不是做产品，是做一个**每个字节都能自己解释、正确性有测试背书的 mini-TiKV**。
最终效果：把对分布式存储的理解从"会用 etcd"推进到"能造 TiKV 的内核骨架"，
同时手里多一个真正属于自己的可复用组件。

---

## 3. 需求规格

> 需求编号规则：`REL-*` 可靠性 / `FUNC-*` 功能 / `UX-*` 易用性 / `PERF-*` 性能 / `ENG-*` 工程规范。
> 优先级：**P0 必须**（不达标不算完成）/ **P1 应该**（里程碑验收项）/ **P2 可选**（stretch）。

### 3.1 用户与使用场景

| 用户 | 场景 | 核心诉求 | 对应需求 |
|---|---|---|---|
| 学习者（开发者本人及后来者） | 读代码、改代码、做实验、复现 BUG | 代码清晰、文档随阶段、确定性模拟器 | UX-06、§5 Phase2 模拟器、ENG-* |
| 嵌入开发者 | 把 TinyKV 当组件嵌进别的程序 | API 干净、错误结构化、嵌入 ≤20 行示例能跑 | UX-03、UX-07、§4.5 |
| 集群使用者 | 部署 3~5 节点、读写、排障 | 快速上手、CLI、集群健康一屏可见 | UX-01/02/04/05 |

### 3.2 功能需求（FUNC，按阶段）

| 编号 | 功能 | 优先级 | 里程碑 |
|---|---|---|---|
| FUNC-01 | 单机引擎：Get/Put/Delete/Scan + CF（Column Family） | P0 | M1 |
| FUNC-02 | 引擎掉电恢复（WAL 可配 fsync） | P0 | M1 |
| FUNC-03 | Raft 共识：选举/日志复制/快照/日志压缩 | P0 | M2 |
| FUNC-04 | 3 节点 RaftKV：写走共识、客户端去重重试幂等 | P0 | M2 |
| FUNC-05 | 线性一致读：ReadIndex | P0 | M3 |
| FUNC-06 | LeaseRead（读优化二档，默认关） | P2 | M3 |
| FUNC-07 | Multi-Raft：Region 元数据/路由/epoch 校验 | P0 | M4 |
| FUNC-08 | Region Split（含 split-check） | P0 | M4 |
| FUNC-09 | 单步成员变更 + Learner 流水线 | P0 | M4 |
| FUNC-10 | Leader Transfer | P1 | M4 |
| FUNC-11 | 快照全链路：生成/分块传输/安装/GC | P0 | M4 |
| FUNC-12 | 调度器 PD-lite：心跳收集 + size 均衡 + AskSplit | P1 | M4 |
| FUNC-13 | Percolator 事务：Prewrite/Commit/Rollback（SI） | P0 | M5 |
| FUNC-14 | 锁解析三件套：CheckTxnStatus/BatchRollback/ResolveLock | P0 | M5 |
| FUNC-15 | Latch（key 粒度单行原子性） | P0 | M5 |
| FUNC-16 | TSO 中心授时（调度器兼任，批量授时） | P0 | M5 |
| FUNC-17 | 事务 GC（安全点以下旧版本清理） | P2 | M5+ |

### 3.3 易用性需求（UX）

| 编号 | 需求 | 验收方式 | 优先级 | 里程碑 |
|---|---|---|---|---|
| UX-01 | **5 分钟上手**：clone → 构建 → 一条脚本拉起 3 节点 → CLI 完成读写 | 干净机器录一遍耗时 ≤5 分钟 | P0 | M2 |
| UX-02 | **CLI 工具 `tinykv-cli`**：get/put/delete/scan/txn/status，人类可读输出 | 每个子命令有 `--help` 与示例 | P0 | M2（txn 于 M5） |
| UX-03 | **错误信息三要素**：发生了什么 + 为什么 + 建议动作；结构化错误带修复线索（如 `NotLeader` 附 leader hint 供重定向） | 错误清单逐一评审三要素齐全 | P0 | M2 起持续 |
| UX-04 | **可观测性**：spdlog 分级日志 + region 维度 trace 开关；`/status` 或 `/metrics` 端点（每 region QPS/延迟、term/leader/commit/apply lag、WAL 大小）；`tinykv-cli status` 一屏看清集群健康 | 排障演练：人为注入一个 BUG，仅靠 CLI+日志定位 | P1 | M2 端点 / M4 完整 |
| UX-05 | **配置即文档**：单文件配置（YAML/TOML），默认值可直接跑通本地演示，每项带注释说明取值影响 | 新人只改节点地址即可启动集群 | P0 | M1 |
| UX-06 | **文档随阶段交付**：每完成一个阶段写一篇实现笔记（继承白皮书精神）+ quickstart + 一张架构图 | 每里程碑合并一篇笔记 | P1 | 持续 |
| UX-07 | **客户端 API 即文档**：C++ API 类型自注释（遵循工作区 13 条清单），README 示例 ≤20 行可编译运行 | 示例进 CI 编译检查 | P0 | M1 |

### 3.4 性能标准（PERF，三级）

**参考环境**（所有数字的测量基准）：3 台 4C8G 容器/VM、NVMe SSD、千兆内网、value=1KB、batch 参数逐项标注。

> 校准规则：M2 落地后先用纯写基准测一组实测基线，允许对下表数字 ±50% 修订；分级结构（S0/S1/S2）不变。

| 编号 | 指标 | S0 交付底线 | S1 目标 | S2 挑战 | 测量方法 |
|---|---|---|---|---|---|
| PERF-01 | 写 QPS（batch=1，fsync=on） | ≥2k | ≥5k | ≥10k | bench/write，3 节点 |
| PERF-02 | 批量写加速比（batch=64，group commit） | ≥8×PERF-01 | ≥15× | ≥30× | bench/write 换 batch 对比 |
| PERF-03 | 线性一致读 QPS（ReadIndex） | ≥3× 写共识路径 | ≥5× | ≥10× | bench/read vs bench/write |
| PERF-04 | LeaseRead 读 QPS | — | ≥10× 写共识 | ≥20× | bench/read（M3，默认关） |
| PERF-05 | 写延迟 P99（batch=1，fsync=on） | ≤10ms | ≤5ms | ≤3ms | bench 输出分位数 |
| PERF-06 | 崩溃恢复时间（kill -9 @ 1GB raft log） | ≤30s | ≤5s | ≤2s | bench/recovery 计时 |
| PERF-07 | Split 完成时间（1M key 均匀分布） | ≤60s | ≤30s | ≤15s | bench/split 计时 |
| PERF-08 | 空载心跳带宽 | ≤1MB/s/peer | ≤100KB/s | ≤10KB/s | 无写负载时消息字节数统计（证明心跳不带日志） |
| PERF-09 | 快照传输吞吐 | ≥10MB/s | ≥50MB/s | 链路极限 | bench/snapshot |

分级含义：
- **S0 = 交付底线**：达不到说明实现有结构性问题（如心跳携带全量日志会直接击穿 PERF-08）；
- **S1 = 里程碑验收**：§9 各里程碑按此验收；
- **S2 = 挑战项**：优化阶段的努力方向，未达成不影响交付。

### 3.5 可靠性需求（REL，全部 P0）

| 编号 | 不变量 | 说明 |
|---|---|---|
| REL-01 | 线性一致性 | 写走共识日志；读走 ReadIndex（不进日志），LeaseRead 为可选降级 |
| REL-02 | 崩溃恢复（crash-safe） | 任意时刻 kill -9 后重启，已确认提交的数据不丢、Raft 状态不错乱。（KVstorageBaseRaft-cpp 此点是坏的：Persister 构造即清空文件——绝不重蹈） |
| REL-03 | 快照一致性 | 日志压缩后落后节点可经快照追赶；生成/传输/安装/清理全链路无窗口期错乱 |
| REL-04 | 成员变更安全 | 单步变更期间不丢已提交数据；Leader Transfer 不丢提案 |
| REL-05 | Split 安全 | 分裂前后每个 key 恰好属一个 Region；stale epoch 请求被拒并重定向 |
| REL-06 | 事务 SI | Snapshot Isolation：写写冲突检测、primary key 决议、锁超时回滚；无死锁（冲突即回滚自身） |

### 3.6 工程规范需求（ENG，全部 P0）

| 编号 | 需求 |
|---|---|
| ENG-01 | 接口设计逐条过工作区 [API 设计指南 §10 的 13 条清单](../docs/现代C++API设计原则.md)（见 §4.5 落地映射） |
| ENG-02 | 跨平台 Windows + Linux；CMake ≥ 3.22；vcpkg/Conan 管依赖 |
| ENG-03 | 日志用 spdlog（教学实现热路径 `std::cout` 是反例）；测试用 GoogleTest |
| ENG-04 | 无 `detach` 线程、无裸 `new/delete` 队列、无 sleep 对齐启动 |
| ENG-05 | 正确性交付门槛：随机故障注入连续 **150 轮**无 FAIL（继承白皮书作者纪律，脚本进 CI 夜跑） |

### 3.7 非目标

- Region Merge（白皮书也未做）；
- 完整 PD 调度（复杂策略、热点迁移——只做 size 均衡最小调度器）；
- 联合共识（Joint Consensus）——用单步成员变更 + Learner（TiKV 同路线，复杂度低一个量级）；
- RDMA/GPU 传输（只预留 Transport 抽象插槽）；
- 悲观事务、行级悲观锁（只做 Percolator/SI）。

---

## 4. 总体架构

### 4.1 分层图

```
┌─────────────────────────────────────────────────────────┐
│ Client/Clerk：ClientId+Seq 去重、路由缓存、重试           │
├─────────────────────────────────────────────────────────┤
│ RPC 层：protobuf 消息 + asio 传输（可插拔）              │
├─────────────────────────────────────────────────────────┤
│ 服务层：Raw API / Txn API、请求去重表、Latch、TSO 接入    │
├─────────────────────────────────────────────────────────┤
│ Multi-Raft 层：Region 路由（epoch 校验）、Peer、RaftStore │
│   ├ Transport：Raft 消息通道 + Snapshot 分块流式传输      │
│   └ Worker：批量 drain 消息 → HandleMsg → HandleRaftReady │
├─────────────────────────────────────────────────────────┤
│ Raft 共识层（线性状态机，无并发心智负担）                  │
│   RawNode（Tick/Step/Ready/Advance）→ Raft → RaftLog     │
├─────────────────────────────────────────────────────────┤
│ Storage 抽象层：KV 引擎（P1 自研）+ raft log WAL + 元数据 │
└─────────────────────────────────────────────────────────┘
```

### 4.2 两条顶层设计原则（分别继承自 TinyKV 与 Mooncake）

1. **Raft 是纯线性状态机**（TinyKV/etcd 路线）：一端输入（`Step(msg)` / `Tick()`），另一端输出（`Ready`），
   内部无锁无并发，并发问题全部推到外层 Worker。心智负担最低，可用模拟器做确定性测试。
2. **控制面/数据面分离**（Mooncake 最大优点）：调度器（PD-lite）只动元数据、不承载数据流；
   Snapshot 这类大数据走独立的分块流式传输通道，不阻塞 Raft 消息通道。

### 4.3 模块与目录规划

```
tinykv/
├── proto/            # 消息定义即接口文档（继承 KVstorageBaseRaft-cpp 的 proto 风格）
│   ├── raft.proto        # RequestVote/AppendEntries/InstallSnapshot/...
│   ├── kv.proto          # RawGet/RawPut/RawDelete/RawScan + Txn API
│   └── raft_cmdpb.proto  # RaftCmdRequest（requests 数组！批量一等公民）
├── src/
│   ├── storage/      # P1：Storage 抽象接口 + 自研引擎（MemTable+WAL+SST）+ CF 前缀编码
│   ├── raft/         # P2：RaftLog / Raft / RawNode（纯状态机，可脱离 IO 单测）
│   ├── wal/          # raft log 持久化：段文件 + 索引 + checksum + group commit
│   ├── raftstore/    # P2/P3：RaftStore / Peer / PeerStorage / Router / Worker / Transport
│   ├── scheduler/    # P3：PD-lite（心跳收集 + size 均衡 + AskSplit + TSO）
│   ├── txn/          # P4：MvccTxn（Lock/Write/Default CF）、Prewrite/Commit/Resolve
│   ├── server/       # 服务层：去重表、Latch、请求分发
│   ├── client/       # Clerk：路由缓存、重试、去重
│   ├── cli/          # tinykv-cli（UX-02）
│   └── common/       # 强类型 ID、expected 错误、编码工具、spdlog 封装
├── tests/            # 测试总目录（布局规范、登记表、一致性检查见 docs/testing.md）
│   ├── testlib/      # 共享测试基建：fixture、虚拟时钟/网络接口、线性一致性检查器封装
│   ├── unit/         # gtest：Raft 状态机可用消息序列直接断言
│   ├── simulation/   # 确定性模拟器：虚拟时钟 + 虚拟网络（参考 etcd rafttest）
│   ├── integration/  # 多进程集成：故障注入 + 线性一致性历史检查
│   └── bench/        # 性能基准（PERF-01~09 的测量程序；自顶层 bench/ 并入，统一登记）
└── docs/             # quickstart + 每阶段实现笔记（UX-06）
```

### 4.4 并发模型

- **P2**：每 RaftStore 单线程 Worker（参考白皮书 `raftWorker.run()` 的批量 drain 模式：
  先一次性取空队列 → 按 region 分组 `HandleMsg` → 每个 peer 一次 `HandleRaftReady`）。
- **P3**：多个 Region 共享 Worker 线程池（worker 数 = min(核数, 4) 起步），region 间天然并行。
- **Apply**：M2/M3 同步 apply（正确性优先）；M4 起独立 apply 线程，region 内按 commit 序保序，
  apply 完成回调唤醒等待中的提案回调（callback registry）。
- **网络**：收发各一个线程，消息经无锁队列投递到对应 region 的 Worker。

### 4.5 C++ API 约定（ENG-01 的落地映射）

| 13 条清单条目 | 本项目落地 |
|---|---|
| 强类型（#2/#10） | `Term` / `LogIndex` / `RegionId` / `PeerId` / `ClientId` / `SeqNo` 全部强类型——Raft 的 index 与 term 是最经典的"两个 uint64 可互换"事故源 |
| 错误语义（#5/#6） | 全接口 `[[nodiscard]] std::expected<T, KvError>`；`KvError` 用 `std::variant` 承载 `KeyNotInRegion` / `StaleEpoch` / `NotLeader{leader_hint}` 等结构化错误（同时服务 UX-03 的三要素错误信息） |
| ptr+len（#4） | key/value 统一 `std::span<const std::byte>` / `std::string_view` |
| 所有权（#3/#7） | 存储层返回 `std::unique_ptr<Reader>`（RAII，析构关 txn）；Engine 以值/unique_ptr 独占传递 |
| 一件事一个函数（#8） | 不提供 `CreateReader + CloseReader` 对，Reader 自身 RAII；`pop` 返回 `std::optional<T>` |
| Options 聚合（#1/#11） | 多参操作一律 `XxxOptions` 结构体 + 指定初始化器 |

骨架示例（Storage 抽象，P1 定型后被 RaftStore 复用）：

```cpp
namespace tinykv {

enum class Cf : uint8_t { Default, Write, Lock };   // P4 事务直接复用这三个 CF

struct GetResult { std::optional<std::string> value; };

class Storage {
public:
    virtual ~Storage() = default;
    virtual std::expected<GetResult, KvError>
    get(Cf cf, std::span<const std::byte> key) = 0;
    virtual std::expected<void, KvError>
    write(std::span<const Modify> batch) = 0;       // 批量是一等公民
    virtual std::expected<std::unique_ptr<Reader>, KvError>
    create_reader() = 0;                            // RAII
};

struct Reader {
    virtual std::expected<GetResult, KvError> get(Cf cf, std::span<const std::byte> key) = 0;
    // iterator 返回 RAII 对象，seek/valid/next 三段式封装成 input_iterator
    virtual ~Reader() = default;
};

}  // namespace tinykv
```

---

## 5. 分阶段实现需求

### Phase 0 基建（先行，约 1 周）

- CMake 工程骨架（vcpkg 依赖：protobuf、asio、spdlog、gtest、unordered_dense——工作区已有，用于去重表/Region 路由）；
- 强类型 ID / `KvError` / 编码工具（CF 前缀：`cf_id + user_key`，继承白皮书 Project1）；
- proto 消息骨架；CLI 骨架与配置文件框架（UX-02/05 先行定型）；
- CI（Windows/Linux 双平台编译 + 单测）。

### Phase 1 Standalone（对应 Project1，约 1 周）

**需求**：FUNC-01/02 + UX-05/07
1. `Storage` 接口（见 §4.5）+ 自研引擎：内存跳表 MemTable + WAL 追加 + 只读 SST 分层
   （LSM 雏形；白皮书介绍了 WiscKey 的 key/value 分离思路，可作为 SST value 分离的后续优化，不进首版）；
2. CF 前缀编码与 `Scan(start_key, limit)` 范围迭代器；
3. Raw API 服务（Get/Put/Delete/Scan），单进程 + CLI 本地模式可用。

**建议决策**：
- 引擎首版**自研**而非包 RocksDB：跳表 MemTable 是 P4 事务 CF 操作和 P3 split-check（按序扫描找 split key）的基础，
  白皮书 Project1/3 都依赖引擎的有序遍历能力；自研才能保证接口贴合。后续提供 RocksDB 适配器作为对照实现。
- `Modify`（Put/Delete）+ `Write(batch)` 与上层 `raft_cmdpb` 的 requests 数组对齐，一次 apply 一次落盘。

### Phase 2 Raft + 单 Region RaftKV（对应 Project2，约 3 周）

**需求**：FUNC-03/04 + REL-01/02 + PERF-01/02/05/06/08（行为细节以白皮书 Project2 笔记为准）
1. **三件套**：`RaftLog`（committed/applied/stabled 四指针模型 + dummy entry）、`Raft`（Step/Tick 状态机，
   消息集：Hup/Beat/Propose/Append/Snapshot/AppendResp/RequestVote/VoteResp/Heartbeat/HeartbeatResp）、
   `RawNode`（Ready/Advance/SoftState/HardState）。
   行为红线：AE 拒绝回退按冲突 term 整段回退；Follower 对已 compact 的 prevLogIndex 回 committed 而非 reject；
   逐条比对防旧消息误截断；只有当前 term 有日志被多数派复制才推进 commit（论文 §5.4.2）。
2. **WAL 层**（替代教学实现的"全量 trunc 重写"）：追加写 + 段文件 + 每条记录 CRC + 段索引；
   fsync 策略可配置（`fsync_on_commit` / group commit 批量刷盘）；**启动只读不写、绝不 truncate**。
3. **快照与日志压缩**：apply 驱动的 `CompactLog` Admin 命令（走共识，继承 TinyKV 设计）→ 异步生成快照
   → `InstallSnapshot` 分块流式传输（借鉴白皮书切片发送 + Mooncake 的 Batch 传输语义：submit/status/取消）；
   领导者对落后超过快照点的节点自动转快照模式。
4. **状态机对接**：Peer 侧 proposals 环形队列 + callback registry（白皮书"Proposal 回复处理"的五分支逻辑）；
   客户端去重表（ClientId, SeqNo）→ 最近回复缓存，保证重试幂等。
5. **线性一致读**：ReadIndex（记 read_index → 广播心跳确认 quorum → 等 applied ≥ read_index → 读状态机）；
   LeaseRead 作为可选优化（FUNC-06，默认关，注释明确 clock drift 风险）。
6. **Transport 抽象**：`Transport` 接口（`send(RegionId, RaftMessage)` + snapshot 流式句柄），
   语义参照 Mooncake TE 的 `TransferRequest/TransferStatus/Batch` 简化版；首版 asio+protobuf TCP 实现，
   预留多传输后端插槽（RDMA 等）。

**建议决策**：
- 状态机消息集直接对齐白皮书（MessageId/Term/Index/LogTerm/Entries/Commit/To 七字段复用），
  不自创新协议——白皮书笔记对这些消息的处理细节（含疑难杂症）覆盖完整，照着走少踩坑；
- 选举超时随机化 + "记录重置时刻、睡到点"的 ticker 模式（教学实现的正确做法，保留）。

### Phase 3 Multi-Raft（对应 Project3，约 3–4 周）

**需求**：FUNC-07~12 + REL-03/04/05 + PERF-07/08/09
1. **元数据模型**：Region（id/start_key/end_key/peers/`RegionEpoch{conf_ver, version}`）、
   RaftStore 的 `StoreMeta`（regionRanges btree + regions map + RWMutex）、peer 路由表；
   每个 Region 一条 Raft Group（一个 store 上一个 region 至多一个 peer）。
2. **Split**：定时 split-check（按序扫描估计 size/keys）→ 向调度器 AskSplit（分配新 region/peer id）
   → Admin Split 提案 → apply 时 clone region、切 end_key、新建 peer 注册路由、双写 regionRanges。
   需求红线：split 提案必须校验 epoch 与 split_key ∈ (start, end)；apply 后立即 notify 调度器（白皮书
   "no region"问题的解法）；Put/Delete apply 时维护 `SizeDiffHint`（白皮书 Delete 阶段卡死问题的解法）。
3. **成员变更（单步）**：`AddLearner → AddVoter → RemoveNode` 流水线；apply 时同步 `ApplyConfChange`；
   被移除者是自己时 destroy peer；**两节点集群移除 Leader** 的边界（先 TransferLeader 再移除，白皮书疑难杂症①）。
4. **Leader Transfer**：`TransferLeader` Admin 命令 → 同步日志至目标 → `TimeoutNow`；转移期间拒绝新提案；
   一个 election timeout 内未完成则放弃。
5. **快照全链路**（Multi-Raft 版）：生成（region 任务队列）/ 传输（分块+校验）/ 安装（先清旧数据、
   **必须 `isInitialized()` 判断**——白皮书 1/15 概率 BUG 的根因）/ GC 清理。
6. **调度器（PD-lite）**：独立进程；收集 region 心跳（epoch 比较规则：version 与 conf_ver 均大者新）；
   最小调度：size 最大的 store 挑 region 迁往 size 最小的 store（MovePeer 三步：AddLearner→AddVoter→Remove 旧 peer）；
   AskSplit 接口。
7. **Client 路由**：region 路由缓存 + epoch 校验失败时回源调度器刷新 + `NotLeader{leader_hint}` 重定向。

**建议决策**：本阶段是全项目最大坑（白皮书作者 119h 里 P3 占 3 周），需求上要求：
- 每个子特性先在**确定性模拟器**（虚拟时钟+虚拟网络）里跑通再上真网络；
- 日志分级 + region 维度的 trace 开关（UX-04，随机复现 BUG 靠日志考古，白皮书反复强调）。

### Phase 4 事务（对应 Project4，约 1–2 周）

**需求**：FUNC-13~17 + REL-06
1. Percolator 三 CF（Default/Write/Lock），时间戳编码 key（`EncodeKey(key, ts)` 大端）；
2. **Prewrite**：检查（a）最新 Write.commitTs > start_ts → 写写冲突 abort；（b）任意 ts 的 Lock 存在 → abort；
   写入 Data + Lock（含 primary key）；
3. **Commit**：primary 优先（lock 归属校验 → 写 Write → 删 Lock），secondary 可异步并发；
4. **读**：遇 Lock 等待（TTL 超时 → CheckTxnStatus 决议回滚）；按 `commitTs ≤ start_ts` 最新 Write 读 Data；
5. **Lock 解析三件套**：CheckTxnStatus / BatchRollback / ResolveLock；
6. **Latch**：server 侧 key 粒度锁，保证 percolator 的"单行原子性"前提（白皮书明确：TinyKV 无单行事务，
   靠 Latch 补——这是测试测不出但语义必需的部分）；
7. TSO：由调度器（PD-lite）兼任中心授时（batch 授时窗口）；
8. GC：安全点（最小活跃 start_ts）以下旧版本清理（Admin 命令走共识，P2 可选）。

---

## 6. 关键技术决策汇总

| 决策点 | 选择 | 理由 / 放弃的替代 |
|---|---|---|
| 语言标准 | C++23（`std::expected` 等），最低 C++20 | 工作区指南已在 g++ 16.1 C++20/23 验证过用法 |
| RPC | asio + protobuf 自研轻 RPC（Transport 可插拔） | 跨平台（muduo 仅 Linux 排除）；gRPC 构建重、Windows 依赖链长，作为备选 |
| 序列化 | protobuf（raft 消息/WAL 记录可自定义二进制） | proto 即接口文档；WAL 用紧凑自定义编码省空间 |
| 日志 / 测试 | spdlog / GoogleTest + 自研确定性模拟器 | 教学实现裸 cout 反例 |
| KV 引擎 | 自研跳表 MemTable + WAL + SST，RocksDB 适配器后置 | 见 Phase 1 决策 |
| WAL | 追加段文件 + CRC + group commit，fsync 可配置 | REL-02 底线，见 §3.5 |
| 成员变更 | 单步 + Learner | 放弃 Joint Consensus（复杂度不成比例） |
| 读一致性 | ReadIndex 必做（FUNC-05），LeaseRead 可选（FUNC-06） | 教学实现"读走日志"吞吐被共识封顶（PERF-03 的意义） |
| 并发 | per-store Worker 线程池 + region 亲和 | Raft 状态机内部保持零锁 |
| 哈希表 | 复用工作区 unordered_dense | 去重表/路由表热点路径 |
| CLI / 配置 | tinykv-cli + 单文件 YAML 配置 | UX-02 / UX-05 |

---

## 7. 参考项目复用矩阵

| 主题 | KVstorageBaseRaft-cpp（教学 Raft-KV） | Mooncake（KVCache 分离式存储，Apache-2.0） |
|---|---|---|
| **可借鉴** | 分层边界与 `applyChan` 窄接口（RPC/Raft/状态机/持久化解耦）；etcd 式 Ready 模型的 C++ 对接方式；随机化选举 ticker；冲突 term 整段回退 nextIndex；"当前 term 才推进 commit"；ClientId+Seq 去重思路；proto 即接口文档 | **控制面/数据面分离**（Master 只动元数据，数据点对点直传）；Transfer 抽象（Segment/Batch/Request/Status 四层接口语义，本项目的 Transport/快照传输照此设计）；两阶段 Put 防脏读（PutStart/PutEnd）；快照+OpLog 的元数据恢复；Master（调度器）HA 用外部协调组件选主的思路 |
| **必须避开** | Persister 构造即清空文件 → 恢复必坏；每 persist 全量重写、无 fsync、无校验；心跳携带全量日志（PERF-08 直接击穿）；读走共识无 ReadIndex（PERF-03 无法达标）；`waitApplyCh` 裸 new/delete + detach 线程；热路径 cout；单机 fork 多进程 + sleep 对齐启动 + 硬编码节点配置；无成员变更/分片；无一致性测试 | 不可变对象模型（可变 KV 需要 MVCC，本项目 P4 自建）；best-effort 副本（强一致必须 Raft/Quorum）；租约过期读失败语义（通用 KV 不可接受）；纯 hash 路由无序遍历（本项目需要范围扫描，引擎自建有序结构） |
| **复用方式** | 学设计、不搬代码（教学代码质量低，行级调研见调研报告） | Apache-2.0，Transfer 接口语义与两阶段 Put 可直接参照其头文件改写；代码级复用限于工具层，避免拖入 GPU/加速器条件编译 |

> 复用 KVstorageBaseRaft-cpp 代码前先确认其 LICENSE（调研报告未确认，教学项目通常宽松但需查证）。

---

## 8. 测试与验收

> **执行细则、测试目录框架与登记表以 [docs/testing.md](docs/testing.md) 为准（测试策略唯一事实来源）**：
> 本节描述"测什么、达到什么"，该文档规定"怎么组织、怎么跑、怎么防止遗漏"——
> 所有测试须按其 SOP 登记（Scope/Level/Runner/Args/Metadata），并通过 `tools/check_test_registry.py` 一致性检查。

### 8.1 正确性测试（ENG-05 的落地）

1. **单元**：Raft 三件套纯状态机直测（消息序列 → 状态断言，无需网络）；
2. **确定性模拟器**：虚拟时钟 + 虚拟网络（随机延迟/分区/丢包/重启），P2/P3 全部特性在模拟器里先过；
3. **集成随机测试**：多进程 3~5 节点，随机注入 kill/分区/恢复，客户端操作历史落盘，
   离线做线性一致性检查（历史 → 实时序验证，porcupine 思路）；Split/ConfChange 与读写并发叠加；
4. **回归纪律**：每个里程碑交付前连续 150 轮随机测试无 FAIL（脚本进 CI 夜跑）。

### 8.2 性能验收（PERF 的测量程序，进 bench/）

- PERF-01/02/05：写基准（batch=1/8/64 三档 QPS-延迟曲线）；
- PERF-03/04：读基准（读走日志 vs ReadIndex vs LeaseRead 三档对比，证明读优化生效）；
- PERF-06/07/08/09：恢复、split、心跳开销、快照吞吐专项基准。

### 8.3 易用性验收（UX 的验收方式已在 §3.3 逐条给出）

### 8.4 各里程碑验收单

| 里程碑 | 验收 = 引用需求编号 | 用户可见交付物 |
|---|---|---|
| M0 | ENG-01~04 基建定型；双平台 CI 绿 | 仓库骨架（内部） |
| M1 | FUNC-01/02；REL-02（掉电恢复对比 fsync 开/关）；UX-05/07 | 单机 KV + CLI 本地模式 + quickstart |
| M2 | FUNC-03/04；REL-01/02；PERF-01/02/05/06/08 达 S0；UX-01/02/03；150 轮无 FAIL | 3 节点集群 + CLI + status（5 分钟上手达成） |
| M3 | FUNC-05(/06)；PERF-03(/04) 达 S1 | 读性能曲线报告 |
| M4 | FUNC-07~12；REL-03/04/05；PERF-07/08/09 达 S0；UX-04 完整 | Multi-Raft + 调度器 + 排障演练记录 |
| M5 | FUNC-13~16；REL-06；SI 语义测试 | 事务 API + CLI txn 命令 |

---

## 9. 里程碑路线图与工作量预估

参考白皮书作者纯 IDE 时间 119h（P1: 2h / P2: 2 周 / P3: 3 周 / P4: 5 天）；C++ 版需自建基建与测试设施，
预估（单人，业余节奏）：

```
M0 基建 ─→ M1 Standalone ─→ M2 Raft ─→ M3 读优化 ─→ M4 Multi-Raft ─→ M5 事务
 (1周)      (1周)            (3周)       (1周)        (3–4周)         (1–2周)
```

做题顺序偏离白皮书推荐（1→2→4→3）：事务放最后，因为 P4 的 Latch/TSO 与服务层、
Multi-Raft 集成后测试才有意义；白皮书把 P4 排前面是因为它"独立、简单"，工程集成角度后置更稳。

---

## 10. 风险清单

| # | 风险 | 缓解 |
|---|---|---|
| 1 | P3 快照/split 调用链复杂度爆炸（白皮书最大坑，BUG 随机复现） | 确定性模拟器先行；region 级 trace 日志（UX-04）；每个子特性独立 PR + 模拟器用例 |
| 2 | 两节点移除 Leader 等成员变更边界 | 白皮书疑难杂症已给出方案（先 Transfer 再 Remove + 保底心跳推进），写成 §5 需求红线 |
| 3 | Windows/Linux IO 差异（fsync/fdatasync、无 io_uring） | WAL 层平台隔离接口，Linux 首选 `fdatasync`，Windows `FlushFileBuffers`，行为以测试为准 |
| 4 | LeaseRead 时钟漂移破坏线性一致 | 默认关（FUNC-06 P2）；开启时 election timeout ≥ 心跳间隔×冗余系数，文档标注假设 |
| 5 | 异步 apply 引入乱序/重复 | 严格 commit 序单线程 per region apply；去重表兜底；M4 才引入 |
| 6 | 事务跨 Region 2PC（Prewrite 分散多 region） | 与 TinyKV 一致：client 驱动 2PC，server 只保证单 key 原子性（Latch），失败靠 CheckTxnStatus 决议 |
| 7 | 随机测试耗时（150 轮纪律 vs 开发节奏） | 模拟器跑快路径（分钟级），真网络长跑放 CI 夜跑 |
| 8 | 性能标准数字拍脑袋（硬件差异导致失真） | M2 实测基线后 ±50% 校准（§3.4 校准规则），分级结构不变 |

---

## 11. 参考资料

- [TinyKV-White-Paper](https://github.com/Smith-Cruise/TinyKV-White-Paper)（本项目工作区）四篇 Project 笔记
- [KVstorageBaseRaft-cpp](https://github.com/youngyangyang04/KVstorageBaseRaft-cpp)（行级调研报告，见会话记录）
- [Mooncake](https://github.com/kvcache-ai/Mooncake)（Apache-2.0；设计文档 `docs/source/design/`）
- [etcd/raft 源码](https://github.com/etcd-io/raft)（Raft 状态机参考实现，白皮书 Project2 的源头）
- [Instructors' Guide to Raft](https://thesquareplanet.com/blog/students-guide-to-raft/)（6.824 学生指南）
- Raft 论文 §5.4.2（commit 规则）、§6（成员变更）、§7（日志压缩）；Percolator 论文（OSDI 2010）
- 工作区 [现代C++API设计原则](../docs/现代C++API设计原则.md)（接口评审清单 §10）
