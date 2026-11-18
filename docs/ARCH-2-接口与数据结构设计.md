# ARCH-2 接口与数据结构设计

> 性质：设计文档。前半（§1–§4）描述**已实现的字节布局**，任何改动都是格式迁移，须走迁移脚本；
> 后半（§5–§10）描述**契约与协议**，含已识别缺陷与建议签名。
> 与 SPEC 的关系：SPEC 按里程碑记账，本文按"表 / 状态机 / 接口 / 并发 / 幂等"五个正交切面重组，
> 两者冲突时以代码与本文的字节布局为准，并回头修 SPEC。

## 1. 四条设计原则

1. **编码唯一实现点**：凡进引擎的 key 必须经 `encode_key` / `encode_ts_key` / `encode_raw_key` 之一。
   调用方再拼一次前缀就会双重前缀——这条规则的代价是"哪个函数带前缀"必须一眼可判，收益是 CF 隔离与 Scan 有序由同一处保证。
2. **错误进 `result<T>`，不变量破坏进异常**：`KvError` 九变体（含 M5 三个事务变体）是唯一错误通道；
   唯一允许 throw 的是 `TsNotMonotonic`——TSO 回退意味着已写出的数据不可信，继续服务比拒绝服务更贵。
3. **结构化而非元组**：`Put`/`Delete` 独立类型，`Modify` 用 `variant`；不返回 `pair`，不传 `ptr+len`。
4. **幂等优先于去重**：能用数据语义做到幂等的（commit / rollback / GC / split 重放），不依赖请求序号；
   序号去重只兜"语义不幂等"的写（raw put/del 组合）。

## 2. 键空间全景

存储是"一个 Engine = 一个 region"，key 的一级区分是 CF 前缀字节，二级是保留段 `\0`。

```
Engine（data_dir 或 data_dir/regions/<id>）
│
├─ CF Default  [0x00]
│   ├─ 0x00 "re" + be64(index)          Raft 日志条目（raftpb.Entry）
│   ├─ 0x00 "lm"                        Raft 元信息（6×u64，小端，48B）
│   ├─ 0x00 "la" + 8B?                  —— 实际是 "la" 三字节键 + u64 值
│   ├─ 0x00 "rk" + user_key             raw KV（无 ts 后缀的独立命名空间）
│   └─ 0x00 user_key + be64(start_ts)   MVCC 数据版本
├─ CF Write    [0x01]  user_key + be64(commit_ts) → be64(start_ts)
└─ CF Lock     [0x02]  user_key → LockValue（每 key 至多一把）
```

**为什么 raw 要独立前缀**：Default CF 里 MVCC 键 = `user_key + 8B ts`，raw 键没有后缀。
split 迁移要靠"剥 8 字节取 user_key"判归属，8 字节长的 raw 键会被剥成空串而误判给左半，
路由却按真 key 判给右半——读回 `found:false`，split 还回 `OK`。BUG-0004 记录了这个静默丢数据，
修法是给 raw 加保留前缀 `\0rk`（第三字节与 `\0re`/`\0lm` 不同，共处 `\0` 段而不冲突），
而不是"按 Write/Lock 反查布局"这类启发式。

## 3. 数据表草案

每张表按"主键 / 排序 / 值布局 / 写入者 / 读取者 / 生命周期 / 版本"七栏描述。
标记 `[已实现]` 的可以直接对照源码；`[规划]` 的只有设计，不随当前代码交付。

### 3.1 T-01 `kv_data` — MVCC 数据表 `[已实现]`

| 栏 | 内容 |
|---|---|
| 物理位置 | CF `Default`（cf_id = 0x00） |
| 主键 | `(user_key, start_ts)`，字节序 = `user_key` 字典序 → `be64(start_ts)` |
| 值 | 原始字节串；删除事务写**空值**作为墓碑占位 |
| 写入者 | `MvccTxn::prewrite`（同一 Storage 批内与 Lock 同行写） |
| 读取者 | `MvccTxn::read_data`（按精确 `(key, version_ts)`）、`Peer` 快照 |
| 多版本 | 每次 prewrite 新建一行，start_ts 不同即不同行 |
| 回收 | `MvccTxn::prune_user`：只保留 `commit_ts ≤ safe_point` 的最新版本所引用的数据行 |
| 不变式 | 同一 `(user_key, start_ts)` 至多一行；空值 ≠ 不存在（需由 Write 记录区分） |

### 3.2 T-02 `kv_write` — 提交记录表 `[已实现]`

| 栏 | 内容 |
|---|---|
| 物理位置 | CF `Write`（0x01） |
| 主键 | `(user_key, commit_ts)` |
| 值 | `be64(start_ts)`，8 字节——指向 T-01 的数据版本 |
| 写入者 | `MvccTxn::commit`（写 Write + 删 Lock，同批） |
| 读取者 | `find_write`（≤ start_ts 的最大 commit_ts）、`find_commit_ts_of`、`latest_commit_bound`（写写冲突） |
| 语义 | 这张表的存在即"已提交"。没有对应 Write 的 Default 行是未提交残留 |
| 索引 | 主键前缀区间扫描 `[user_key]`—`[user_key + 0xFF…]` 即得该 key 全部版本 |
| 回收 | 同上，只保留 `≤ safe_point` 的最新一条 |
| 坑 | 前缀扫描必须**剥 ts 后精确等值比较**：`starts_with` 会把 `k1` 的版本误配给 `k` |

### 3.3 T-03 `kv_lock` — 事务锁表 `[已实现]`

| 栏 | 内容 |
|---|---|
| 物理位置 | CF `Lock`（0x02） |
| 主键 | `user_key`（无 ts 后缀） |
| 值布局 | `[flag:1B][start_ts:8B BE][ttl_ms:4B BE][created_ms:8B BE][primary_len:4B BE][primary]`，最小 25B |
| flag | `0x01` 活跃锁；`0x02` rollback 墓碑 |
| 写入者 | `prewrite`（active）、`rollback`（墓碑）、`commit`（删除） |
| 读取者 | `load_lock`、`locks_of`（全表扫）、`check_txn_status` |
| 唯一约束 | 每 user_key 至多一把活跃锁 |
| 回收 | 墓碑不主动删；`prewrite` 遇到墓碑可覆盖。GC 不清理 Lock（依赖 TTL 决议） |
| 已知缺陷 | `created_ms` 在 daemon 路径恒为 0（ARCH-1 §5-P0-2），使 TTL 判定失真 |

**墓碑为什么必要**：`rollback` 后再 `prewrite` 同一 key，若锁被直接删除，"有没有锁"
无法区分"从未 prewrite"与"已回滚"。墓碑把三态（活跃 / 已回滚 / 从未存在）压进一个 flag 字节。

### 3.4 T-04 `kv_raw` — raw KV 名空间 `[已实现]`

| 栏 | 内容 |
|---|---|
| 物理位置 | CF `Default`，user_key 前置保留段 `\0rk` |
| 主键 | `\0rk + user_key`（存储层再补 `0x00` CF 前缀） |
| 值 | 原始字节串，`GetResult.value` 用 `optional` 区分"空值"与"无值" |
| 写入者 | `Peer::execute`（Put/Delete 走共识） |
| 读取者 | `ServerApp::kv_read` → `read_directly(Default, encode_raw_key(key))` |
| 与 T-01 隔离 | 靠 `\0` 段 + 第三字节区分；split 迁移按前缀分派后才判归属 |
| 版本 | 单版本，后写覆盖；无 MVCC、无 TTL（proto 的 `ttl` 字段保留但服务端忽略） |

### 3.5 T-05 `raft_entry` — 日志表 `[已实现]`

| 栏 | 内容 |
|---|---|
| 物理位置 | CF `Default`，`\0re` + `be64(index)` |
| 主键 | `index`（大端保证 Scan 序 = index 序） |
| 值 | `raftpb.Entry` 序列化（index / term / data / type） |
| data 载荷 | 三种：`RaftCmdRequest`（单条，向后兼容）、Envelope 批（`pack_entries`）、txn 载荷（首字节 `0x2B`） |
| 写入者 | `RaftLog::append` / `truncate_from` / `compact` / `reset_snapshot` |
| 读取者 | `RaftLog::at/slice/term`、`collect_apply` |
| 回收 | `compact(upto)` 批量删 `[first, upto]`；`upto ≤ applied` 且 `can_compact`（全 voter 已 match） |

### 3.6 T-06 `raft_meta` / T-07 `raft_applied` — 双指针表 `[已实现]`

| 栏 | `raft_meta`（`\0lm`） | `raft_applied`（`\0la`） |
|---|---|---|
| 主键 | 固定三字节键，单行 | 固定三字节键，单行 |
| 值 | `first_index / last_index / committed / term / vote / dummy_term`，6×u64 小端，48B | 1×u64 |
| 为什么分开 | 改 6×u64 的 blob 形状会触发数据迁移（红线）；独立键不动原有格式 | — |
| 写入时机 | append / set_committed / set_hard_state / compact | `mark_applied`，与 meta 同批 |
| 崩溃语义 | 缺 `\0la` 时读 0 = 保守重放（宁可重复不可丢） | 缺 = 从未 apply |
| 不变式 | `1 ≤ first ≤ last+1`；`applied ≤ committed ≤ last`；`first ≤ applied+1` | 同左 |

### 3.8 内存表

这些不是持久表，但它们是"系统状态"的一部分，重启即失——列出来是为了明确哪些语义不跨重启。

| ID | 表 | 主键 | 值 | 归属 | 重启后果 |
|---|---|---|---|---|---|
| M-01 | `s_session` | session_id (`t<n>`) | `TxnClient`（start_ts + 写缓冲） | `TxnSessionManager` | 全部活跃事务丢失，锁靠 TTL 决议 |
| M-02 | `s_dedup` | `client_id` | 已 apply 的最大 seq | `Peer::last_seq_` | 去重基线归零 → 旧请求可二次 apply |
| M-03 | `s_reply` | `(client_id, seq)` | `RaftCmdResponse` | `Peer::replies_` | 未取走的回执丢失（调用方本就该超时） |
| M-04 | `s_txn_err` | log index | `KvError` | `Peer::txn_errors_` | 丢弃（apply 已成功落盘，错误是给发起方的诊断） |
| M-05 | `s_region` | `region_id` | `Region`（含 epoch、peers、size_hint） | `StoreMeta` | **拓扑归零，只剩 region 1**（ARCH-1 P1-3） |
| M-06 | `s_latch` | user_key | `shared_ptr<Slot{mutex}>` | `LatchManager` | 无意义（锁是瞬态的） |
| M-07 | `s_snapshot_send/recv` | peer_id | 游标 / 块缓存 | `Peer` | 传输中断，重传即可（幂等） |

### 3.9 规划表 `[规划]`（M7 ⑧，不随当前代码交付）

元数据文件形态已定：`data_dir/meta/regions.pb` + `.bak`，全量快照 + CRC32C。

```text
region_meta  （protobuf，regions[] 全量）
  region_id      uint64   主键
  start_key      bytes    闭端（"" = -∞）
  end_key        bytes    开端（"" = +∞）
  epoch.version  uint64   split/merge 递增
  epoch.conf_ver uint64   成员变更递增
  peers[]        {peer_id, store_id, role}
  checksum       uint32   覆盖上述字段的 CRC32C
```

写纪律：`split apply 成功` → 同批写新文件 → fsync → rename 覆盖 → 旧版本转 `.bak`。
读纪律：启动先读 `regions.pb`，CRC 不过读 `.bak`，两者皆废回退"单 region + 告警"。

### 3.10 关系型等价视图（仅供理解，不是实现计划）

若要换成 SQL 承载同样的语义，最小表集如下——它把 KV 的"隐式结构"显式化，
便于评审时对照"我们到底存了哪些实体"：

```sql
-- 数据版本：等价 T-01 + T-02 合并视图
CREATE TABLE kv_version (
  user_key   BYTEA  NOT NULL,
  start_ts   BIGINT NOT NULL,           -- 数据版本号
  commit_ts  BIGINT,                    -- NULL = 未提交（有锁）
  value      BYTEA,                     -- NULL/空串 = 删除墓碑
  PRIMARY KEY (user_key, start_ts)
);
CREATE INDEX kv_version_commit ON kv_version (user_key, commit_ts DESC);

-- 活跃锁：等价 T-03
CREATE TABLE txn_lock (
  user_key   BYTEA  PRIMARY KEY,
  start_ts   BIGINT NOT NULL,
  ttl_ms     INTEGER NOT NULL,
  created_ms BIGINT NOT NULL,           -- 当前实现恒 0（缺陷）
  primary_key BYTEA NOT NULL,
  state      SMALLINT NOT NULL          -- 1=active 2=rollback
);

-- 幂等台账：当前只有内存版（M-02），持久化后等价此表
CREATE TABLE idempotency (
  client_id  BIGINT NOT NULL,
  region_id  BIGINT NOT NULL,
  seq        BIGINT NOT NULL,
  result     BYTEA,
  PRIMARY KEY (client_id, region_id, seq)
);

-- region 拓扑：当前纯内存（M-05），⑧ 后落 regions.pb
CREATE TABLE region (
  region_id  BIGINT PRIMARY KEY,
  start_key  BYTEA NOT NULL,
  end_key    BYTEA NOT NULL,
  version    BIGINT NOT NULL,
  conf_ver   BIGINT NOT NULL
);

-- 会话：当前内存（M-01）
CREATE TABLE session (
  session_id    TEXT PRIMARY KEY,
  start_ts      BIGINT NOT NULL,
  last_active_ms BIGINT NOT NULL        -- 当前用 created_ms，语义错（P1-4）
);
```

## 4. 编码规则与兼容

| 规则 | 内容 | 违反后果 |
|---|---|---|
| 字节序 | 一律大端（raft meta 的 6×u64 例外，仅本模块自读自写） | Scan 序 ≠ 数值序，版本查找错 |
| CF 前缀 | 单字节，Default=0 < Write=1 < Lock=2 | 同名 key 跨 CF 串写 |
| 数值宽度 | ts 8B、ttl 4B、primary_len 4B | 截断后解析出错误 primary |
| 长度下界 | `decode_lock_value` 要求 ≥25B，`plen` 越界返回 `nullopt` | 损坏数据被当成合法锁 |
| 格式迁移 | 只能经 `tools/migrate` 或 `tinykv-cli migrate`，迁移前自动备份 | 手改数据文件即不可回滚 |

**兼容策略**：新增字段只能追加（protobuf 字段号递增），不得复用编号；`\0lm` 的 6×u64
形状视为冻结，新增元信息一律独立键（`\0la` 就是这么加进去的，代价是每 region 多一个键）。

## 5. 状态机

七台状态机，按"谁驱动 / 状态集 / 转移 / 不变量"描述。所有 tick 驱动都用**逻辑拍**
而非墙钟，保证模拟器可复现。

### SM-01 Raft 角色机（`raft/raft.cpp`）

```
                tick 超时 & 非 leader
   ┌────────────┐ ─────────────────────► ┌───────────┐
   │  Follower  │                        │ Candidate │
   └────────────┘ ◄───────────────────── └───────────┘
      ▲   ▲         更高 term / 同 term 已有 leader     │
      │   │                                            │ 票数 ≥ majority
      │   │  AppendEntries(term ≥ 当前)                 ▼
      │   └────────────────────────────────────── ┌───────────┐
      └────────────────────────────────────────── │  Leader   │
              AppendResp 显示更高 term            └───────────┘
```

| 转移 | 触发 | 副作用（必须原子完成） |
|---|---|---|
| F→C | `election_elapsed_ ≥ election_timeout_` | term+1、vote 投自己、**先持久化 term/vote**、广播 RequestVote |
| C→L | 赞成票 ≥ majority | 初始化 `next_index_[p]=last+1`、`match_index_[p]=0`、投空条目占位 |
| C→F | 收到 term ≥ 当前的消息 | 清票、重置计时 |
| L→F | 收到更高 term 的 Append/Heartbeat/Vote | 降级并修正 next/match |
| L→L' | `transfer_leader(target)`：目标已追平 | 发一次 TimeoutNow、转移期拒新提案、一个超时内静默放弃 |
| 任意→F | 安装快照 | `restore_snapshot(index,term)` 重置日志指针 |

**选举超时错峰**：`election_timeout_ = base + (id*3) % base`——节点专属偏移，避免同拍全部竞选。
这不是随机化，是可复现的确定性错峰，模拟器里同一 seed 结果一致。

**成员变更不吃这台机**：`EntryConfChange` 在 apply 时才改 `voters_/learners_`，
角色机只看到"多数派规模变了"。好处是崩溃后可从日志重建成员表，坏处是
`cfg_.voters` 不持久化、日志压缩后无法重建（ARCH-1 §5-P2）。

### SM-02 日志四指针机（`raft/raftlog.h`）

状态 = `(first, last, committed, applied, dummy_term)`，事件是六个操作：

| 操作 | 前置 | 后置 | 越界返回 |
|---|---|---|---|
| `append(entries)` | 条目 index 连续且 > last | `last += n` | `InvalidArgument` |
| `truncate_from(i)` | `i > committed` | `last = i-1` | `InvalidArgument` |
| `set_committed(i)` | `i ≤ last` 且单调 | `committed = i` | `InvalidArgument` |
| `mark_applied(i)` | `i ≤ committed` 且单调 | `applied = i`（持久化） | `InvalidArgument` |
| `compact(upto)` | `upto ≤ applied`（上层 `can_compact` 再收紧） | `first = upto+1` | `InvalidArgument` |
| `reset_snapshot(i,t)` | — | `first=i+1, last=committed=applied=i, dummy_term=t` | — |

**核心不变量**：`first ≤ applied+1 ≤ committed+1 ≤ last+1`。
`committed` 与 `applied` 之间那段就是"已提交未 apply"的崩溃重放窗口——M7 ① 之前这段会永久丢失。

### SM-03 Peer apply 分派机（`raftstore/peer.cpp`）

```
entry.data()
 ├─ 空                     → 跳过（leader 占位条目）
 ├─ [0]==0x2B              → txn 载荷：[type:1B][kvpb] → Latch → MvccTxn
 └─ 其他                   → try_unpack_entries?
        ├─ 是              → 逐条 RaftCmdRequest → execute（各自 client/seq 登记回执）
        └─ 否              → 单条 RaftCmdRequest（向后兼容旧日志）
                                ├─ has_admin → split / compact / transfer
                                └─ requests[] → Get / Put / Delete / Scan → 单批 write
```

| 状态 | 判据 | 说明 |
|---|---|---|
| 成功 | 无异常且无 `txn_errors_` 项 | 整批 `mark_applied` |
| 失败 | 返回 `KvError` | 记录 `apply_errors_` + 不推进 applied，整批重放 |
| 半成功 | txn 载荷内 MVCC 返回错误 | **不**让 on_ready 失败；错误进 `txn_errors_[index]`，位点照常推进 |

"半成功"这条分叉是刻意的：事务冲突（WriteConflict/LockConflict）是业务结果，
不是 apply 故障。若让 apply 失败，冲突会把整批回滚重放，形成活锁。

### SM-04 Percolator 事务机（`txn/client.h` + `txn/mvcc.h`）

```
Idle ──set/del──► Buffered ──commit──► Prewriting ──ok──► Locked(primary)
                                          │                    │
                                          │ 冲突                │ commit(primary) ok
                                          ▼                    ▼
                                     Resolving ──► Buffered  Committing ──ok──► Committed
                                          │                    │
                                          │ 判定回滚            │ 失败
                                          ▼                    ▼
                                     RolledBack          （保持 Locked，等决议）
```

决议三态（`check_txn_status`，唯一判据是 **primary**）：

| 观测 | 结论 | 动作 | 判据 |
|---|---|---|---|
| primary 有 `Write(commit_ts) 且 value=start_ts` | committed | 用对方 commit_ts 补 Write、删 Lock | `find_commit_ts_of` 命中 |
| primary 有活跃锁且 `now - created < ttl` | 等待 | 返回剩余 TTL，调用方退避重试 | `elapsed < ttl_ms` |
| primary 有活跃锁且已过期 | 过期 | 墓碑清锁（BatchRollback 全部同 primary 锁） | `elapsed ≥ ttl_ms` |
| primary 无锁且无 Write | 已回滚 | 清残留锁 | 落到 `rolled_back=true` |

**红线级约束**：写写冲突立即 abort 自己、绝不等待（无死锁的结构性保证）；锁的生死只由
primary 判定（避免双决议）；commit 必须 `commit_ts > start_ts`。

### SM-05 会话机（`server/txn_sessions.h`）

```
Created ─► Active ──commit ok──► Committed(Closed)
              │  └──rollback──► RolledBack(Closed)
              └──idle ≥ 30s──► Expiring ──rollback ok──► Closed
                                     └──fail──► Active（保留重试，不丢未清的锁）
```

`take/put_back` 的语义是"把 client 从表里摘出来用，用完放回"——同一 session 的并发请求
会看到"会话不存在"（`InvalidArgument`）而不是排队。这是当前实现的隐式串行化：
不是锁，是"表项暂时消失"。后果是并发同 session 请求报错而非阻塞，且 `created_ms_`
不随访问刷新（P1-4）。

### SM-06 Region 机（`raftstore/region.h`）

```
Normal(id, [s,e), epoch{v,c})
   │ admin split（走共识）
   ▼
Splitting ──apply 成功──► 左半(id, [s,k), epoch{v+1,c})  +  右半(new, [k,e), epoch{v+1,c})
                                  │                              │
                                  │ observer 建 Engine+Peer       │ 单 voter 立即 campaign
                                  ▼                              ▼
                               Normal                         Normal
```

| 约束 | 守护 |
|---|---|
| `split_key ∈ (start, end)` 开区间 | `split_region` |
| 左右首尾相接且覆盖原区间 | `replace_region` 三键校验 |
| 不与其他 region 重叠 | `overlaps_locked` 全扫描 |
| 每个 key 恰属一个 region | 上两条合并即得 |
| epoch 只单调推进 | `is_newer_than` / `is_comparable_with`（混序按 stale 拒绝） |

重放幂等（BUG-0005）：`apply_split_admin` 先查"左半 end == split_key 且该 key 归新 region"，
命中即视为上次已切过，直接跳过 `replace_region`。

### SM-07 快照传动机（`raftstore/peer.cpp`）

| 端 | 状态 | 转移 | 判据 |
|---|---|---|---|
| 发送 | `Idle → Sending(index,term,blob,total,next,ack)` | `on_ready` 发现 `snapshot_targets` | follower 的 `next_index < first_index` |
| 发送 | `Sending → Sending` | `next - ack < window` 时继续发 | 窗口流控 |
| 发送 | `Sending → Done` | `ack == total-1` | 收到最后一个 ACK |
| 接收 | `Idle → Receiving(chunks[], have[], received)` | 首块到达 | `snapshot_index/term` 与本地不冲突 |
| 接收 | `Receiving → Receiving` | 每块 CRC 通过 → 落位 → 回 ACK | CRC 不符则丢弃该块，不回 ACK |
| 接收 | `Receiving → Installing` | `received == total` | 序号连续 |
| 接收 | `Installing → Idle` | 整表安装 + `restore_snapshot` | 安装失败则整体作废重传 |

## 6. 核心接口契约

签名只写关键部分（完整见头文件）。"幂等"栏是本文最实用的一列：调用方能否安全重试。

### 6.1 存储层

| 接口 | 签名要点 | 前置 | 后置 / 错误 | 幂等 | 线程安全 |
|---|---|---|---|---|---|
| `Engine::open` | `result<unique_ptr<Engine>>(EngineOptions)` | dir 可写 | 载入 manifest → 重放 WAL | 是 | — |
| `Storage::write` | `result<void>(span<const Modify>)` | key 非空（编码层校验） | 先 WAL 后 memtable，一次 fsync | **是**（同批重放结果相同） | 内部 `mu_` |
| `Storage::get` | `result<GetResult>(Cf, span<const byte>)` | — | `optional` 区分空值与无值 | 是 | 内部 `mu_` |
| `Storage::create_reader` | `result<unique_ptr<Reader>>()` | — | 快照隔离（共享 memtable + CoW） | 是 | 内部 `mu_` |
| `Reader::scan` | `result<unique_ptr<Iterator>>(Cf, start, limit)` | limit > 0 | 升序、`limit` 截断**无游标** | 是 | 快照只读 |
| `Iterator::key/value` | `string_view` | `valid()` | 失效即悬垂 | — | 否 |

### 6.2 共识层

| 接口 | 签名要点 | 语义 | 幂等 / 副作用 |
|---|---|---|---|
| `Raft::step` | `result<void>(const Message&)` | 纯状态机入口，零 IO | 重复消息按 term/index 去重 |
| `Raft::tick` | `void()` | 推进逻辑拍，到点竞选/心跳 | 幂等（同拍重复调用多走一拍，破坏可复现性） |
| `Raft::propose` | `result<void>(string data)` | 追加 + 广播，非 leader 返回 `NotLeader` | **不幂等**（每调一次多一条日志） |
| `Raft::take_ready` | `Ready()` | 取走消息 + 待 apply + 已确认读位点 | 取走即清空，**不可重入** |
| `Raft::mark_applied` | `result<void>(LogIndex)` | 校验 ≤ committed 后持久化 | 是（重复确认同值无害） |
| `Raft::read_index` | `result<uint64_t>()` | 记轮次、广播心跳 | 每次调用新增一轮（`pending_reads_` 无超时清理） |
| `RaftLog::append/compact` | `result<void>` | 见 SM-02 | 幂等性由前置条件保证 |

### 6.3 raftstore 层

| 接口 | 签名要点 | 关键约定 |
|---|---|---|
| `Peer::open` | `result<unique_ptr<Peer>>(Config, Storage&)` | 一个 Engine 只挂一个 Peer（日志键固定前缀，共享必串 log） |
| `Peer::propose` | `result<void>(const RaftCmdRequest&)` | 去重：`seq ≤ applied_seq` 静默成功；batching 下非 leader 在此拦截 |
| `Peer::on_ready` | `result<size_t>()` | 攒批 → take_ready → apply 全部 → 整批 `mark_applied` → 可能压缩 |
| `Peer::wait_read_index` | `result<uint64_t>(pump, rounds, timeout)` | 非阻塞两阶段；pump 由调用方注入（Peer 不持传输层） |
| `Peer::take_messages` | `vector<Message>()` | **绝不能**顺带调 `take_ready`（会清掉待 apply 队列） |
| `Peer::take_reply` / `drop_reply` | `optional<RaftCmdResponse>` / `void` | 取走或丢弃，二选一，漏了就是无界增长 |
| `Peer::compact_log` | `result<void>(LogIndex)` | 超过 applied 被拒 |
| `StoreMeta::find_region` | `optional<Region>(string_view)` | `+∞` 区间（end 空）不进 upper_bound 索引，单独兜底 |
| `Router::route` | `result<Region>(key, epoch)` | **生产路径未使用**（ARCH-1 P1-2） |
| `Peer::applied_index` | `uint64_t()` | 读放行判据；**构造期未从 `RaftLog::applied` 播种**，重启后恒 0（ARCH-1 P0-4） |

`Peer::applied_` 是一个"内存副本"，它的真值来自 `RaftLog::applied`（持久）与
`Raft::applied_`（dispatch 游标）。三者的关系应当在 `Peer::open` 里显式建立，
而不是靠"下一次 apply 顺手覆盖"。这类"同一语义多处副本"的字段，播种点必须和持久化点写在同一处注释里，
否则恢复路径一换就漏。

### 6.4 事务层

| 接口 | 签名要点 | 幂等 | 备注 |
|---|---|---|---|
| `MvccTxn::get` | `result<optional<string>>(key, start_ts)` | 是 | 先看 Lock：自己的锁读自己的数据，他者锁返回 `LockConflict` |
| `MvccTxn::prewrite` | `result<void>(mutations, primary, start_ts, ttl, now_ms)` | **否**（重放会报 WriteConflict） | 校验 primary 必须在 mutations 内 |
| `MvccTxn::commit` | `result<void>(keys, start_ts, commit_ts)` | **是**（`find_write_exact` 命中即跳过） | 校验 `commit_ts > start_ts` |
| `MvccTxn::rollback` | `result<void>(keys, start_ts)` | 是（无锁 / 他者锁都跳过） | 只写墓碑，不删锁 |
| `MvccTxn::check_txn_status` | `result<TxnStatus>(primary, start_ts, now_ms)` | 是（只读） | 唯一决议入口 |
| `MvccTxn::resolve_locks_of` | `result<size_t>(primary, start_ts, commit_to?)` | 是 | 全表扫 Lock CF 找同 primary |
| `TxnExecutor` | 6 虚函数，语义同 MvccTxn | 同上 | 两个实现：Local（直连）/ Peer（经 raft） |
| `TsProvider::get_timestamps` | `result<vector<Ts>>(n)` | **否**（每次新号） | 严格单调，破坏即 fatal |

**接口缺口的根源**：`TxnExecutor::prewrite` 的参数里没有"这些 key 属于哪些 region"，
也没有"现在几点"。前者导致跨区写错分区（P0-1），后者导致锁 TTL 失真（P0-2）。
两个缺陷都在同一处签名上，说明这个接口是按"单机语义"设计的，装进分布式壳时没重新过一遍。

### 6.5 HTTP 端点契约

| 方法 | 路径 | 请求体（`\t` 分隔） | 返回 | 一致性 |
|---|---|---|---|---|
| GET | `/kv?key=K` | — | `{"found":bool,"value":"..."}` | ReadIndex |
| POST | `/kv` | `put\tK\tV` / `del\tK` | `OK` / `ERR <reason>` | 共识 |
| GET | `/status` | — | 计数器 JSON | 快照（try_lock） |
| GET | `/regions` | — | Region 数组 | 内存读 |
| GET | `/region?id=N` | — | Region + term + commit | 内存读 |
| GET | `/metrics` | — | 按 path 计数 | 内存读 |
| GET | `/trace` | — | 环形缓冲事件 | 内存读 |
| POST | `/txn/begin` | — | `OK session=tN` | TSO |
| POST | `/txn/set` | `set\tSID\tK\tV` | `OK` | 仅本地缓冲 |
| POST | `/txn/del` | `del\tSID\tK` | `OK` | 仅本地缓冲 |
| GET | `/txn/get?session=SID&key=K` | — | `{"value":"..."}` / `BLOCKED` | ReadIndex |
| GET | `/txn/status?session=SID` | — | `{"session","start_ts"}` | 内存读 |
| POST | `/txn/commit` | `commit\tSID` | `OK commit_ts=N` | 2PC |
| POST | `/txn/rollback` | `rollback\tSID` | `OK` | 2PC |
| POST | `/txn/gc` | `gc\t<safe_point>` | `OK gc` | 各 region 共识 |
| POST | `/region/split` | `split\tRID\tKEY` | `OK split` | 共识 |

**观测口白名单**：`GET /regions`、`GET /region`、`GET /metrics`、`GET /trace`；其余 404。
这是"业务泵持闸时仍能观测"的结构保证，不是权限控制。

**漂移**：CLI 还在发 `/region/peers`、`/scheduler/status`、`POST /trace`，三者服务端无实现（ARCH-1 P1-1）。

## 7. 并发设计

### 7.1 锁清单与锁序

| 锁 | 类型 | 保护对象 | 持有者 | 最坏持有时间 |
|---|---|---|---|---|
| `ServerApp::peer_mu_` | `mutex` | 全部 Peer + 全部 Engine + StoreMeta 写 | HTTP handler、Worker、Executor | 一次网络往返（多副本 ≤5 s） |
| `StoreMeta::mu_` | `shared_mutex` | regions / ranges 索引 | 所有路由读 | 微秒级 |
| `Engine::mu_` | `mutex` | WAL / memtable / sst 列表 | 单 region 内读写 | 一次 fsync |
| `LatchManager::slots_[k]` | `mutex` | 单 key 的 MVCC 写 | apply 路径 | 一次三列族写 |
| `LatchManager::mu_` | `mutex` | 槽表本身 | `acquire` 取槽 | 微秒级 |
| `TxnSessionManager::mu_` | `mutex` | clients_ / created_ms_ | 会话操作 | 微秒级 |
| `MonotonicTsProvider::mu_` | `mutex` | counter 取段 | TSO 调用 | 纳秒级 |
| `Worker::mu_` / `cv_mu_` | `mutex` | 线程启停 / 等待 | start/stop | 一次循环 |

**实际锁序**：`peer_mu_ → {StoreMeta::mu_, Engine::mu_, latch, session::mu_, tso::mu_}`。
反向路径不存在——但这是**观察结果，不是声明**。代码里已有一处绕道证据：
`PeerTxnExecutor::resolve_locks_of` 先 `hold_gate()` 读锁、再 `gate.reset()` 释放、
再走写路径重新加闸（`peer_txn.h:117` 注释"同一线程不可对同一 mutex 重复 lock"）。
这说明"大闸 + 内部加锁"的组合天然会产生"持闸调用又加闸"的自死锁风险，
每加一处都要靠人工判断是否要 reset。M7 ⑨a 拆锁时必须先把锁序写进契约。

### 7.2 无锁共享状态

| 变量 | 类型 | 读方 | 写方 | 保证 |
|---|---|---|---|---|
| `now_ms_` | `atomic<uint64>` | HTTP 线程（TTL/GC） | 主循环 | relaxed 足够（单调推进） |
| `kv_writes_/kv_reads_/txn_ops_/split_events_` | `atomic<uint64>` | 观测口 | 各线程 | relaxed 计数 |
| `next_region_id_` | `atomic<uint64>` | split 路径 | split 路径 | 前置 CAS（实际由 peer_mu_ 串行） |
| `observe_cache_` | `string` | 观测口 | 业务泵（try_lock 后写） | `observe_mu_` |

`split_error_` 是普通 `string`，写在 apply 路径（持 peer_mu_）、读在 `admin_split`（持 peer_mu_）
——**当前安全，但依赖"两者都在闸内"这一隐式前提**，拆锁时是首要排查点。

### 7.3 并发上限的结构性来源

1. 一把 `peer_mu_`：region 数增加不提升并行度。
2. HTTP handler 在自己的线程里泵共识并等待 applied：`/kv` 写请求的延迟 = 共识延迟，
   期间整站阻塞。
3. 单 io 线程 + 每连接一次 `async_accept`：连接建立串行。
4. `kScanLimit = 4096` 的版本扫描 + 全表 Lock 扫描：单请求 CPU 随数据量线性。

这四条合起来解释了为什么 bench 里吞吐随并发线程数迅速饱和——不是 CPU 不够，是闸只有一把。

## 8. 幂等与崩溃窗口

### 8.1 四层幂等

| 层 | 机制 | 覆盖范围 | 判据 | 缺口 |
|---|---|---|---|---|
| L1 协议去重 | `(client_id, seq)` ≤ `applied_seq` 即短路 | raw put/del | `last_seq_` | 内存态、txn 载荷不覆盖、命中不登记回执 |
| L2 语义幂等 | MVCC 原语自带 | commit / rollback / GC | `find_write_exact`、无锁跳过、幂等删 | prewrite 重放报伪 WriteConflict（不改数据） |
| L3 结构幂等 | apply 侧守卫 | split 重放、快照安装、slot 重复建 | 区间判据、先墓碑后覆盖、`slots_.find` | observer 失败后无补偿 |
| L4 恢复幂等 | applied 位点整批确认 | 崩溃重放 | `\0la` | 已压缩日志的窗口不可恢复 |

L2 是当前最可靠的一层：即使 L1/L4 全失效，重复 commit 也只是幂等跳过。
这也解释了为什么 txn 路径敢完全不做 seq 去重。

### 8.2 崩溃窗口矩阵

| 窗口 | 崩溃点 | 后果 | 是否安全 | 依据 |
|---|---|---|---|---|
| W1 | WAL append 之前 | 提案丢失 | 安全 | 客户端超时重试 |
| W2 | WAL append 后、memtable 之前 | 内存无、盘上有 | 安全 | 重放 WAL |
| W3 | 共识已提交、apply 之前 | 已提交未生效 | 安全 | `applied < committed` → 重放 |
| W4 | apply 写完存储、`mark_applied` 之前 | 整批重放 | 条件安全 | 重放需幂等：put 幂等，txn prewrite 报伪冲突 |
| W5 | `mark_applied` 后、回执前 | 客户端重试 → 二次 apply | **不安全** | L1 内存态，重启失效 |
| W6 | split 改元数据后、observer 建右半前 | 路由指向无 peer 的 region | **不安全** | `require()` 兜底返回首个 peer |
| W7 | SST flush 中（SST→manifest→清 WAL→换 memtable） | 新旧状态混合 | 安全 | manifest 未写则旧 WAL 重放 |
| W8 | 快照收到一半 | 部分块 | 安全 | 缓存按 from 丢弃重传 |

W5 与 W6 是两个"已知不安全"的窗口，也是 M7 ⑧ 与去重持久化的直接动因。

### 8.3 幂等实现要点

- **去重短路必须"登记回执"**：当前 `propose` 命中去重直接 `return {}`，
  调用方等 `applied >= expect` 会空转到超时。正确做法是登记一个合成回执，
  或让调用方按 `applied_seq` 判定而非位点判定。
- **墓碑是幂等的载体**：rollback 写墓碑而非删锁，使"回滚过"与"从未写"可区分，
  重复回滚不改变状态。
- **GC 的幂等来自"删不存在的键 = 成功"**，但删的是 Write 与 Default 两类，
  若中途崩溃，重跑会重算保留集——因为判据是"当前 Write CF 的内容"，不是"上次删到哪"。
- **快照安装必须"先删后写"**：只覆盖不删除会让分叉日志留下的脏键永久残留，
  且这个残留不会被任何后续操作纠正。

## 9. 接口问题与修改建议

编号供提交信息与 review 引用。每条给"现状 → 建议 → 影响面 → 兼容策略"。

**IF-01 TTL 时间基准随请求传递（对应 P0-2）**
现状：`PeerTxnExecutor::prewrite` 丢弃 `now_ms`，proto 无 `created_ms` 字段，apply 侧传 0。
建议：`TxnPrewriteRequest` 增 `uint64 created_ms = 5;`；`run_txn_prewrite` 用它填 `LockValue.created_ms`；
`TxnClient` 用同一个 `now_ms` 保证 start_ts 与 created_ms 同源。
影响面：`proto/kv.proto`、`peer_txn.h`、`peer.cpp`、`mvcc.h` 注释。
兼容：新增字段号，旧数据 `created_ms=0` 视为"立即过期"需一次性迁移或按 `start_ts` 兜底。

**IF-02 事务载荷带去重键（对应 P0-3）**
现状：`[0x2B][type][kvpb]` 无 client/seq。
建议：头部扩为 `[0x2B][type][be64 client][be64 seq][kvpb]`，或把去重上移到 `Peer::propose_txn_*`。
影响面：`peer.cpp` 四个 `propose_txn_*` 与 `execute_txn_payload`；日志格式**变更**，需版本字节。
兼容：type 字节留高位做版本位，旧载荷按 v0 解析。

**IF-03 去重命中登记回执（对应 W5）**
现状：`propose` 去重短路返回空。
建议：命中时向 `replies_` 写入一个 `Retryable` 语义的合成回执，或让 `wait_applied` 先查 `applied_seq`。
影响面：`peer.cpp::propose`、`ServerApp::wait_applied_locked`。
兼容：纯行为变更，无格式影响。

**IF-04 跨区护栏（对应 P0-1，M7 ⑦-0）**
现状：`prewrite` 整批经 primary 所在 peer。
建议：`PeerTxnExecutor::prewrite` 先按 `peer_for(key).id` 分组，若存在 multi-region 直接返回
`InvalidArgument{"跨 region 事务未支持"}`；`commit/rollback` 同理按 key 分组路由。
影响面：`peer_txn.h` 一个文件。
兼容：护栏先行不改变正确路径行为，是后续协调器的地基。

**IF-05 端点表化（对应 P1-1）**
现状：`admin_get/admin_post` if-else 链。
建议：`std::map<std::string, Handler>` 注册表，注册即契约；CLI 侧用同一份路径常量。
影响面：`server_app.h`、`cli.cpp`、`docs/cli.md`。
兼容：纯重构，外部行为不变（但会**暴露**已有 404）。

**IF-06 会话提供 last_active（对应 P1-4）**
建议：`created_ms_` → `last_active_ms_`，`set/get/commit` 刷新；`sweep` 改判 last_active。
影响面：`txn_sessions.h` 一处。
兼容：无格式影响。

**IF-07 `require()` 不再兜底**
现状：找不到 peer 时返回 `slots_.begin()` 的第一个 peer（静默错路由）。
建议：返回 `result<Peer&>` 或 `Peer*`，由调用方转 `KeyNotInRegion`。
影响面：`peer_registry.h` + 所有调用点（4 处）。
兼容：行为变更，需要测试覆盖"region 缺失"路径。

**IF-08 读路径接入 epoch 校验（对应 P1-2）**
建议：`ServerApp` 用 `Router::route(key, epoch)` 替代 `registry_->require(key)`；
CLI 携带路由快照的 epoch；`StaleEpoch` 回执驱动客户端刷新。
影响面：`server_app.h`、`cli.cpp`、`proto`（已有字段，无需改）。
兼容：纯行为启用，需先让 CLI 学会"收到 StaleEpoch 就重取 `/regions`"。

**IF-09 锁索引**
建议：`prewrite` 时同批写 `\0li + primary + start_ts + key → 空`，`resolve_locks_of` 改按索引扫。
影响面：`mvcc.h`、GC（需同步清索引）。
兼容：新增键空间，需迁移脚本回填。

**IF-10 读轮次超时清理**
建议：`PendingRead` 记 `deadline_tick`，`tick()` 里清理超时项并计入 `/status`。
影响面：`raft.h/.cpp`（核心模块，须逐条批准）。
兼容：内部状态，无格式影响。
