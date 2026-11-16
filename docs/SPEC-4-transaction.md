# SPEC-4 — M5 Percolator 事务（Snapshot Isolation，白皮书 Project4）

> 上游：SPEC-3（Multi-Raft）；需求：FUNC-13~16（17 P2）、REL-06、UX-02(txn)/06。
> 架构前提：client 驱动 2PC；server 只保证单 key 原子性（Latch）。状态：待实现。

## 4.1 功能范围

- Percolator 三 CF 语义：`Default`（数据）/ `Write`（提交记录）/ `Lock`（锁+primary 指针）；
- Prewrite / Commit / Rollback（SI）；读：遇 Lock 等待（TTL 超时→决议）+ 按 `commitTs ≤ start_ts` 取最新版本；
- 锁解析三件套：CheckTxnStatus / BatchRollback / ResolveLock；
- Latch：server 侧 key 粒度锁（单行原子性前提）；TSO：调度器兼任，批量授时；GC：安全点以下旧版本清理（P2）。

## 4.2 架构

```
TxnClient(Begin→Get/Set…→Commit)
  ├─ TSO: scheduler 兼任 tso::TsProvider.get_timestamps(n)（batch 窗口，严格单调）
  ├─ 2PC: prewrite(并行分发到各 region) → 全部成功 → commit(primary 先, secondary 可异步)
  └─ 冲突/超时 → CheckTxnStatus 决议 → BatchRollback / ResolveLock
Server(per region): Latch(key 粒度) ─► MvccTxn(读写 Lock/Write/Default 三 CF) ─► raft 提案 ─► SPEC-3 管线
key 布局: encode_ts_key(user_key, ts) 大端 → 同 key 多版本字节序有序（Scan 免排序）
```

### 4.2.1 锁状态转换图（单 key 视角，事务生死由 primary 决定）

```
                 prewrite 成功
  [无锁] ───────────────────────► [locked{primary,start_ts,ttl}]
     ▲                                │            │
     │ commit: 写 Write+删 Lock        │ TTL 到期    │ rollback(显式或决议)
     ▼                                ▼            ▼
  [committed: Write{start_ts→commit_ts} 可见]  [rolled_back: Put tombstone 到 Lock CF]
                                        决议入口: CheckTxnStatus(primary)
                                        → primary committed  → 正向推进(补 Write 删 Lock)
                                        → primary 无锁/回滚   → BatchRollback 清锁
```

## 4.3 接口设计

### 4.3.1 proto（txnpb 契约，变更先与用户核对）

```protobuf
// kv.proto Txn 部分（字段语义即契约）
message PrewriteRequest {
  repeated Mutation mutations = 1;      // Put/Delete
  bytes primary_lock = 2;               // 本事务 primary key（决议锚点）
  uint64 start_ts = 3;
  uint64 lock_ttl_ms = 4;
}
message CommitRequest { uint64 start_ts = 1; uint64 commit_ts = 2; repeated bytes keys = 3; }
message RollbackRequest { uint64 start_ts = 1; repeated bytes keys = 2; }
message CheckTxnStatusRequest { bytes primary_key = 1; uint64 start_ts = 2; }
  // → {committed(commit_ts) | rolled_back | lock_ttl_remaining(ms)}
message GCRequest { uint64 safe_point = 1; }   // Admin 走共识（P2）
```

### 4.3.2 C++ 侧

```cpp
namespace tinykv::txn {
struct Mutation { std::variant<Put{key,value}, Delete{key}> op; };

class MvccTxn {          // 单次 apply 内的 MVCC 读写（幂等由上层去重表保证）
public:
    result<std::optional<std::string>> get(std::string_view key, Ts start_ts);
    result<void> prewrite(std::span<Mutation const>, std::string_view primary, Ts start_ts, uint64_t ttl);
    result<void> commit(std::span<std::string const> keys, Ts start_ts, Ts commit_ts);
    result<void> rollback(std::span<std::string const> keys, Ts start_ts);
};

class LatchManager {     // server 侧单行原子性（白皮书明确：测不出但语义必需）
public:
    LatchGuard acquire(std::span<std::string const> keys);   // 阻塞等全部 key（内部按 key 排序加锁防死锁）
};
class LatchGuard {       // RAII；析构释放并唤醒等待者
};

class TsProvider {       // TSO（调度器兼任实现）
public:
    virtual result<std::vector<Ts>> get_timestamps(size_t n) = 0;   // 批量授时，严格单调
};

class TxnClient {        // client 侧编排（驱动 2PC）
public:
    result<Ts> begin();                                   // 取 start_ts
    result<void> set(std::string key, std::string value);
    result<void> del(std::string key);
    result<TxnGetResult> get(std::string const& key);     // 含锁等待/决议重试
    result<Ts> commit();                                  // prewrite→commit 全流程
    result<void> rollback();
};
}
```

### 4.3.3 错误矩阵（本阶段 KvError/结果全集）

| 变体/结果 | 触发条件 | 语义/hint |
|---|---|---|
| WriteConflict | prewrite 时最新 Write.commitTs > start_ts | 立即 abort 自身（无死锁结构保证）；client 换 ts 重试或报用户 |
| LockConflict | prewrite/读遇他事务 Lock 存在 | 等待→TTL 后 CheckTxnStatus 决议 |
| Retryable | 决议后锁已清，可安全重试 | client 自动重试 |
| InvalidArgument | 空 key、mutations 超 batch 上限 | 按字段修正 |
| TsNotMonotonic | TSO 返回非严格递增（不变量破坏） | fatal 路径：拒绝服务并报告（异常分层，CLAUDE.md §7-1） |

## 4.4 核心逻辑骨架

```
Prewrite(keys, start_ts):
  对每个 key: ① 查最新 Write: commitTs > start_ts → 写写冲突 → abort（回滚自己，不等待→无死锁）
              ② 查任意 ts 的 Lock: 存在 → abort（锁冲突交给上层 resolve/TTL）
              ③ 通过 → 写 Default(data) + Lock{primary=keys[0], start_ts, ttl}   [同一 raft 提案]

Commit(start_ts, commit_ts=TSO()):
  primary: 校验 Lock 归属(primary+start_ts 匹配) → 写 Write{start_ts→commit_ts} → 删 Lock  [同一提案]
  secondary: 同检查，可异步并发（乱序安全：client 已持 primary 提交凭证）

读(key, start_ts):
  ① Lock 存在且归属当前事务 → 读自己未提交数据
  ② Lock 存在(他者): TTL 内 → 等待/重试; TTL 过 → CheckTxnStatus(primary):
       primary 已提交 → 正向推进该锁(补 Write 删 Lock)；已回滚/不存在 → BatchRollback 清锁 → 重试读
  ③ 无锁: 找 commitTs ≤ start_ts 的最新 Write → 按其 start_ts 读 Default

ResolveLock(region, start_ts): 扫 Lock CF → 逐 key 决议（查 primary 状态）→ 批量 commit/rollback
GC(safe_point = 全集群最小活跃 start_ts): Admin 命令走共识 → 删 Write/Default 中安全点以下旧版本 [P2]
```

## 4.5 红线

1. 写写冲突**立即 abort 自己**，绝不等待他者——无死锁的结构性保证（REL-06）；
2. primary 决议唯一：一个事务的生死只由 primary 的 Write/Lock 状态决定，secondary 不得自行判定；
3. Lock TTL 过期必须可决议（CheckTxnStatus），不存在永久死锁锁；
4. Latch 是单行原子性的前提（白皮书原文：测试测不出但语义必需）——**每 key 一 latch，事务持有至 apply 完成**；
5. TS 大端编码，`commitTs ≤ start_ts` 的版本查找依赖 Write CF 有序扫描；
6. TSO 时间戳严格单调，绝不回退/重复；
7. GC 只删"安全点以下"版本，安全点取全集群最小活跃 start_ts（P2，实现前置=活跃 ts 上报）；
8. 跨 region 事务：client 驱动 2PC，server 不做跨 region 协调（与 TinyKV 一致，见设计文档 §6 风险 6）。

## 4.6 示例

### SI 写写冲突（`si_semantics_test` 冲突矩阵样例）

```cpp
// t1=Begin, t2=Begin（t1.start_ts < t2.start_ts）
// t1.set(k,"a"); t2.set(k,"b"); t2.commit() 成功;
// t1.commit() → EXPECT 结果为 WriteConflict（t1 prewrite 检出冲突 abort，无死锁、无等待）
// 再读 k：值 = "b"（t1 未留下任何中间态）              // SI：无脏写
```

### 快照读

```cpp
auto t = client.begin();               // start_ts = 100
// 他事务更新 k 并提交（commit_ts=110）
auto v = t.get("k");                    // 仍返回 start_ts=100 时版本（Write CF 按 commitTs≤start_ts 定位）
t.commit();                             // 失败（WriteConflict）——读可重复、写会冲突
```

### 锁等待+决议时序（`txn_resolve_test` 样例）

```
事务 A: prewrite(k) 后进程崩溃（Lock 残留, primary=k, ttl=3s）
事务 B: get(k) → 等待 3s → CheckTxnStatus(k@A.start_ts) → primary 无锁且无 Write → A 判回滚
        → BatchRollback 清 A 锁 → B 继续读到旧值 → 自身 prewrite 成功
断言: A 不存在"半提交"状态；B 全程无死锁、无脏读
```

### Latch / TSO 用法

```cpp
auto guard = latches.acquire(encoded_keys);   // 阻塞至全部 key 可用；RAII 析构释放
auto ts_list = tso.get_timestamps(64);        // store 端批量预取；严格单调断言进 tso_test
```

### CLI（UX-02 M5）

`tinykv-cli txn begin` → `txn set k v` → `txn get k` → `txn commit`；`txn status <start_ts>` 查决议状态。

### 配置项表（增量）

| 配置项 | 默认值 | 影响 |
|---|---|---|
| `txn.lock_ttl_ms` | 3000 | 决议等待上限 |
| `txn.resolve_retry_ms` | 100 | 决议轮询间隔 |
| `txn.max_batch_mutations` | 4096 | 单次 prewrite 上限 |
| `scheduler.tso_batch` | 64 | 每 store 预取 ts 数 |
| `txn.gc_interval_min` | 手动 | P2 自动化才启用 |

## 4.7 验收与证据链

| 需求 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| FUNC-13/REL-06 | 冲突矩阵（同 key/不同 key/读-写/写-读/事务内自读）全分支语义正确 | `txn_prewrite_test` `txn_commit_test` `si_semantics_test` | `ctest -L fast` | junit.xml |
| FUNC-14 | primary 崩溃后 TTL 决议：commit→锁正向推进；rollback→锁清除；无半提交态 | `txn_resolve_test`（模拟器） | `ctest -L slow` | junit.xml + 种子 |
| FUNC-15 | 并发同 key 提案在 server 侧严格串行（latch 观察点） | `latch_concurrency_test` | `ctest -L slow` | junit.xml |
| FUNC-16 | TSO 严格单调 + batch 窗口吞吐（64/次 ≥1e5 ts/s） | `tso_test` | `ctest -L fast` | junit.xml |
| 集成 | 事务与 Multi-Raft/split 叠加随机注入，SI 不变量保持 | `txn_chaos_n150`（nightly） | `ctest -R txn_chaos_n150` | `evidence/nightly/txn_chaos_150.txt` |
| REL-06(端到端) | 外部历史满足 SI：无脏读/无写丢失/首提交不被覆盖（外部审计器逐条判定） | `lincheck_si_test`（integration） | `ctest -L nightly` | SI 审计报告 |
| UX-02(txn) | CLI txn 子命令 --help+示例 | `cli_txn_smoke` | `ctest -L fast` | junit.xml |
| UX-06 | M5 实现笔记一篇 | —（人工） | — | `docs/notes-m5.md` |
| FUNC-17(P2) | GC 后旧版本清除且活跃读不受影响 | `gc_test`（stretch） | `ctest -L slow` | junit.xml |

**M5 完成判定** = 上表 P0 行证据齐备 + `txn_chaos_n150` 报告（150 轮无 FAIL）+ `notes-m5.md`。

## 4.8 开放决策点

| # | 决策 | 建议默认 |
|---|---|---|
| D4-1 | lock TTL | 3s（可配）；决议重试间隔 100ms |
| D4-2 | TSO batch 窗口 | 每 store 预取 64 个 ts |
| D4-3 | secondary commit 异步并发度 | ≤8 |
| D4-4 | GC 触发 | 手动 Admin 命令（P2 自动化，间隔 10min） |
| D4-5 | 冲突重试策略 | client 侧自动重试上限 3 次（退避 10/50/200ms），超限报用户 |
