# SPEC-2 — M2 + M3 Raft 共识与 RaftKV（白皮书 Project2 + 读优化）

> 上游：SPEC-1（引擎）；需求：FUNC-03/04/05/06、REL-01/02、PERF-01~06/08、UX-01/02/03/04(端点)/06、ENG-05。
> 状态：待实现。

## 2.0 阶段划分（本册含两个里程碑，各自独立验收）

| 里程碑 | 内容 | 完成判定 |
|---|---|---|
| **M2**（§2.1–2.5 + §2.7 M2 表） | Raft 三件套、raft log WAL、RaftKV、去重、快照/压缩、150 轮纪律 | §2.7.1 全行达 S0 |
| **M3**（§2.6 + §2.7 M3 表） | ReadIndex 必做 + LeaseRead 可选 + 读基准 | §2.7.2 全行达标 |

> 合册原因：SPEC 总数限 2–5 份（用户指令），6 里程碑无法逐一成册；两阶段共享同一架构与状态机代码，
> 拆册反而重复。此为显式偏差（implementation-notes §2 有记录），不接受可指示拆出 SPEC-2b。

## 2.1 功能范围

- Raft 三件套：`RaftLog`（committed/applied/stabled/dummy 四指针）、`Raft`（Step/Tick 纯状态机）、
  `RawNode`（Ready/Advance 输出模型）；
- raft log WAL：段文件 + 记录 CRC + 段索引 + group commit + fsync 可配（**替代教学实现"全量 trunc 重写"**）；
- RaftKV：Peer/RaftStore/Worker/Transport 全链路，写走共识；客户端 `ClientId+SeqNo` 去重（重试幂等）；
- 快照与日志压缩：apply 驱动 `CompactLog`（走共识）→ 异步生成 → `InstallSnapshot` 分块流式传输；
- M3：ReadIndex 线性一致读（必做）+ LeaseRead（可选，默认关）；
- UX-04 M2 部分：`/status` 端点（每 region QPS/延迟、term/leader/commit/apply lag、WAL 大小）。

## 2.2 架构

```
Clerk(ClientId+Seq 去重/重试/leader hint 缓存)
  └► RPC(proto+asio TCP) ─► RaftStore
       ├─ Peer(region)  : proposals 环形队列 / 去重表 / callback registry / PeerStorage
       ├─ Worker(单线程): 批量 drain 消息 → 按 peer 分组 HandleMsg → 每 peer 一次 Ready 循环
       ├─ Transport     : send(To, RaftMessage) + SnapshotStream(分块，控制面/数据面分离)
       ├─ /status 端点  : 读 StoreMeta 计数（QPS/延迟分位/term/commit/apply lag/WAL 大小）
       └─ RawNode ─ Raft(线性状态机, 零锁) ─ RaftLog ─ wal
读路径(M3): ReadIndex = 记 read_index → 广播心跳收 quorum → 等 applied ≥ read_index → 读状态机
并发模型：每 store 一个 Worker 线程（消息无锁队列投递）；Raft 状态机内部零锁；网络收发独立线程。
```

### 2.2.1 Raft 状态转换图（单 peer 视角）

```
                    Hup(超时,无未投更高term票)
   Follower ────────────────────────► Candidate ──(多数派票)──► Leader
      ▲  │ 收到更高 term 的                  │ │                     │ │
      │  │ AppendEntries/Snapshot/投票      │ │ 票被拒或发现新 leader│ │ 发现更高 term
      │  ▼                                 │ ▼                     │ ▼
      └────────────────────────────────────┴───────────────────────┘
   Leader 周期: Beat→广播 Heartbeat(仅元数据)；Propose→append+复制；commit 推进见 §2.5-4
   选举超时: 随机化 [T,2T]；重置时机 = 收到有效 leader 心跳 / 投票 / 自身当选
```

## 2.3 接口设计

### 2.3.1 状态机门面（etcd 式 Ready 模型）

```cpp
namespace tinykv::raft {
class RawNode {                      // 纯状态机对外唯一门面
public:
    void tick();                                     // 时钟驱动
    result<std::optional<Ready>> step(proto::Message msg);  // 消息驱动（错误=协议违例）
    bool has_ready() const;
    Ready ready();                                   // 取输出包
    void advance(Ready const&);                      // 确认已持久化/已发出
};
struct Ready {                       // 一次 Ready = 一次持久化 + 一次广播
    HardState hs;                    // term/vote/commit → WAL 元数据段
    std::span<proto::Entry const> entries;           // 待追加（增量）
    std::span<proto::Entry const> committed;         // 待 apply（顺序应用）
    std::optional<Snapshot> snapshot;
};
class Transport {                    // Mooncake Batch 语义简化版（可插拔，首版 asio TCP）
public:
    virtual result<void> send(PeerId to, proto::Message msg) = 0;
    virtual SnapshotHandle open_snapshot_stream(PeerId to) = 0;   // submit/状态查询/取消
    virtual ~Transport() = default;
};
class RaftLog {                      // 四指针：committed/applied/stabled + dummy 首条
public:
    std::span<const proto::Entry> entries(LogIndex lo, LogIndex hi) const;
    result<void> append(std::span<const proto::Entry> es);
    result<void> compact(LogIndex upto);             // 应用层驱动（Admin CompactLog）
    LogIndex first_index() const;  LogIndex last_index() const;
};
}
```

### 2.3.2 服务层提案（raft_cmdpb，批量一等公民）

```cpp
// raft_cmdpb.proto（字段即契约，变更先与用户核对——CLAUDE.md §15）
message RaftCmdRequest {
  message Header { uint64 region_id; tinykv.commonpb.RegionEpoch epoch; tinykv.raftpb.Peer peer; }
  Header header = 1;
  repeated Request requests = 2;      // Get/Put/Delete/Scan/ReadIndex —— 数组！批量一等公民
  AdminRequest admin = 3;             // Split/TransferLeader/CompactLog（M2 仅 CompactLog 落地）
}
```

Peer 侧回调五分支（白皮书"Proposal 回复处理"）：leader 变更 / term 变更 / region 变更 / 提案被删（冲突/压缩）/ 正常完成——去重表按 (ClientId,SeqNo) 缓存**最近一次**回复保证幂等。

### 2.3.3 错误矩阵（本阶段 KvError 全集与触发点）

| 变体 | 触发点 | hint 语义 |
|---|---|---|
| NotLeader | 提案/查询打到非 leader peer | leader_hint 直接重定向 |
| IoFailed | WAL fsync/write 失败 | 检查磁盘；fsync 失败=提案失败（不确认） |
| Corrupted | WAL CRC / 快照块校验失败 | 从 leader 重新拉取/追赶 |
| InvalidArgument | 空 key、超限 value、非法 batch | 按字段修正 |

## 2.4 核心逻辑骨架

### 2.4.1 ticker 与 step 分支

```
ticker(每 peer): election_timeout 随机 ∈ [T,2T]；重置时刻记录式（不 sleep 累计）
  到点且角色非 Leader 且本 term 未投出反对票 → step(MsgHup)

Hup:            无未投票更高 term → term+1 自投 → 广播 RequestVote(last_index,last_term)
RequestVote:    拒绝时【不重置自己的选举计时器】(白皮书踩坑)；term 落后 → 先更新再判
AppendEntries:  prevLogIndex 不匹配 → 拒绝并回 (index=冲突位置, log_term=冲突 term) 【整段回退依据】
                prevLogIndex 已 compact → 回 committed 而非 reject
                匹配后逐条比对已有 entry：term 相同保留，不同截断并追加——逐条比对防旧消息误截断
推进 commit:    仅统计【当前 term】被多数派复制的 index（论文 §5.4.2）；旧 term 多数派复制不动 commit
```

### 2.4.2 Peer Ready 循环（Worker 内，每 peer 每轮一次）

```
ready = raw.ready()
→ 持久化 hs + entries（WAL group commit；失败=崩溃路径，提案回调全部报错）
→ Transport 广播 messages（增量 entries；落后超快照点 → 转 MsgSnapshot + SnapshotStream）
→ committed 逐条 apply → 数据走 Storage(SPEC-1) → 回复 proposals 五分支
→ read callback（M3）: applied ≥ read_index 的等待者唤醒
→ raw.advance()
```

### 2.4.3 ReadIndex（M3）

```
read(k):
  leader 记 read_index = commit 指针 → 广播 MsgHeartbeat（带本轮 read ctx）→ 收 quorum resp
  → 等 applied ≥ read_index → 读状态机返回（全程不进共识日志）
LeaseRead(默认关): 收到 quorum 心跳即刷新 lease_deadline = now + election_timeout×0.7
  lease 内读直接读状态机（零 RPC）；注释必须声明 clock drift 假设（SPEC-2 D2-4）
```

## 2.5 红线（白皮书疑难杂症逐条对应）

1. 拒票后**不重置**自己选举计时器（重置导致集群不稳定，白皮书实测）；
2. AE 拒绝回退：按冲突 term **整段**回退 nextIndex，禁止逐条退；
3. Follower 对已 compact 的 prevLogIndex：回 **committed**，不走 reject；
4. commit 推进必须"当前 term 多数派复制"，绝不直接推多数派复制的旧 term entry；
5. WAL **启动只读不写、绝不 truncate**（REL-02，Persister 反例）；
6. 心跳只带 index/commit 元数据，**绝不携带日志**（PERF-08 直接击穿点）；
7. 去重表缓存**最近一次** (ClientId,SeqNo)→回复；重复提案不得二次 apply；
8. 日志只发增量 + in-flight 流控窗口（带宽 O(增量)）；提案批量进一个 entry（或少量 entry）；
9. 快照/日志压缩：compact 不得越过 applied；快照生成异步化，不阻塞 Ready 循环。

## 2.6 示例

### M2：状态机直测（unit 层核心形态——无需网络）

```cpp
TEST(RaftElection, LeaderElectedAfterTimeout) {
  auto nodes = make_cluster(3);                 // 内存消息总线直连
  tick_until(nodes, [&](auto& ns){ return count_leaders(ns) == 1; }, 200);
  EXPECT_EQ(count_terms_agree(nodes), 3u);      // 全网收敛同 term
}
TEST(RaftVote, RejectDoesNotResetElectionTimer) {
  auto [a, b] = make_cluster(2);                // b 拒绝 a 的拉票
  deliver(vote_request(a→b)); EXPECT_TRUE(vote_rejected(b→a));
  EXPECT_FALSE(election_timer_reset(a));        // 红线 2.5-1
}
TEST(RaftAppend, WholeSegmentFallbackOnReject) { /* 冲突 term 整段回退，断言 nextIndex 跳变 */ }
TEST(RaftCommit, OnlyCurrentTermEntryCommits) { /* 复制旧 term entry 到多数派 → commit 不动 */ }
```

### M2：快照追赶（simulation 层）

```cpp
TEST(SimCompaction, LaggingPeerCatchesUpViaSnapshot) {
  auto c = sim_cluster(3, seed=42); c.partition({{0},{1,2}}); c.write_n(500);
  c.compact_log_on_leader();            // 旧日志压缩
  c.heal_partition();  c.tick_until_caught_up(peer0);
  EXPECT_EQ(c.get_all_keys(peer0), c.get_all_keys(leader));   // 快照安装后数据一致
}
```

### M3：ReadIndex 一致读

```cpp
// CLI: tinykv-cli --read-mode=read_index get k1    （默认；--read-mode=log 为 PERF-03 对比基线）
// 观测: cli status 显示 read_mode / 每次 read 的 wait_applied_ms 分布（/status 端点）
```

### 150 轮纪律（夜跑）

`ctest -R raft_chaos_n150 --output-junit build/evidence/junit.xml`；
每轮种子与故障序列写入 `build/evidence/nightly/raft_chaos_150.txt`（复现依据）。

### 配置项表（node.yaml，M2 部分）

| 配置项 | 默认值 | 影响 |
|---|---|---|
| `raft.election_timeout_base_ms` | 1000 | 基值 T；随机 [T,2T] |
| `raft.heartbeat_interval_ms` | 250 | 心跳周期；必须 ≤ T×0.5 |
| `raft.inflight_max_bytes / entries` | 64KB / 256 | per-follower 复制窗口 |
| `wal.fsync_on_commit` | true | group commit 窗口 2ms |
| `wal.segment_mb` | 128 | raft log 段大小 |
| `snapshot.compact_threshold_entries` | 100000 | applied 与 first 距离触发 CompactLog |
| `read.mode` | `read_index` | `log`（对比）/`lease`（M3，默认关） |

## 2.7 验收与证据链

### 2.7.1 M2 验收表

| 需求 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| FUNC-03(选举) | 随机超时后唯一 leader；分区恢复后收敛；拒票不重置计时器 | `raft_step_test` `sim_election_n3` `sim_election_n5` | `ctest -L fast` | junit.xml（种子入日志） |
| FUNC-03(复制) | 增量复制+整段回退+误截断防护 | `sim_replication_test` | `ctest -L fast` | junit.xml |
| FUNC-03(压缩/快照) | compact 后落后节点走快照追赶 | `sim_compaction_test` | `ctest -L fast` | junit.xml |
| REL-02 | 随机 kill -9 + 重启：已确认提交零丢失、Raft 状态不错乱、WAL 不被改写 | `wal_segment_crash_test` `sim_restart_test` | `ctest -L slow` | junit.xml + 种子日志 |
| FUNC-04 | 3 节点读写正确；客户端重试幂等（去重表） | `sim_dedup_test` `raft_kv_smoke`（integration） | `ctest -L fast/-L slow` | junit.xml |
| UX-04(M2 端点) | /status 暴露 term/leader/commit/apply lag/QPS/WAL 大小且数值与日志对得上 | `status_endpoint_test` | `ctest -L fast` | junit.xml |
| UX-01/02 | 干净机器 5 分钟上手录得；CLI 集群子命令 --help 齐全 | `quickstart_smoke`（脚本） | CI | 计时日志 |
| UX-03 | 本阶段错误（NotLeader/IoFailed/Corrupted）三要素逐条核对 | —（评审 + `kv_error_test` 扩展） | `ctest -R kv_error_test` | 评审记录 |
| UX-06 | M2 实现笔记一篇 + 架构图一张 | —（人工） | — | `docs/notes-m2.md` |
| ENG-05 | **连续 150 轮随机故障注入无 FAIL**（kill/分区/丢包/重启） | `raft_chaos_n150`（nightly）+ `raft_chaos_smoke`（fast） | `ctest -R raft_chaos_n150` | `evidence/nightly/raft_chaos_150.txt`（含每轮种子） |
| PERF-01/02/05 | S0：≥2k QPS / ≥8× / P99≤10ms | `write_bench` | 夜跑 | `bench/write.json` |
| PERF-06 | kill -9 @1GB log 恢复 ≤30s(S0) | `recovery_bench` | 夜跑 | `bench/recovery.json` |
| PERF-08 | 空载心跳 ≤1MB/s/peer(S0)；心跳消息不含 entries（协议断言） | `heartbeat_bandwidth_bench` + `raft_step_test` 断言 | 夜跑 | `bench/heartbeat.json` |

**M2 完成判定** = 本表全部行证据齐备且口径达 S0 + `notes-m2.md` 就位。

### 2.7.2 M3 验收表（追加）

| 需求 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| REL-01/FUNC-05 | ReadIndex 线性一致历史检查通过 | `lincheck_readindex_test`（integration） | `ctest -L nightly` | 线性化报告 |
| FUNC-06/PERF-04 | LeaseRead（默认关）：开启后读 QPS ≥10× 写共识(S1)；语义仍线性一致 | `sim_leaseread_test` `lincheck_leaseread_test` `read_bench --mode=lease` | `ctest -L slow` / 夜跑 | junit.xml + `bench/read.json` |
| PERF-03 | ReadIndex 读 ≥3×(S0)/≥5×(S1) 写共识路径 | `read_bench --mode=read_index` vs `--mode=log` | 夜跑 | `bench/read.json` |
| UX-06 | M3 实现笔记一篇（读路径选型对比） | —（人工） | — | `docs/notes-m3.md` |

**M3 完成判定** = 本表全部行达标（FUNC-06/PERF-04 为 P2 可选，不做不阻塞 M3 判定但必须留"未做"记录）。

## 2.8 开放决策点

| # | 决策 | 建议默认 |
|---|---|---|
| D2-1 | 选举超时基值 T | 1s（模拟器可缩至 20ms） |
| D2-2 | WAL fsync 策略默认 | `fsync_on_commit=true`（M2）；group commit 窗口 2ms |
| D2-3 | in-flight 复制窗口 | 每 follower 64KB / 256 entry 双阈值 |
| D2-4 | LeaseRead 时钟漂移参数 | 默认关；开启需 election_timeout ≥ 2×心跳 + drift 假设写入配置注释 |
| D2-5 | 快照触发 | CompactLog Admin 命令显式 + apply 距离阈值自动（可配） |
| D2-6 | /status 端点形态 | 复用 RPC 端口 HTTP /status（不引入独立 HTTP 框架） |
