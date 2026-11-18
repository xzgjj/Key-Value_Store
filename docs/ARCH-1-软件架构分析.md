# ARCH-1 软件架构分析（基线 be8f54b）

> 性质：分析文档，不是规格。结论以代码为唯一事实来源，文档漂移处逐条标注。
> 度量口径：`src/` 9 965 行（头 5 461 / 实现 4 504），`tests/` 9 289 行，65 个测试可执行。
> 与 README 的「约 9.1k / 8.6k」存在约 9% 偏差——README 停在 M6 收尾时的快照。

## 1. 判定口径

架构好不好不能靠"分层图好看"来判。本文只用四把尺子：

1. **依赖是否单向**：改上层能否不改下层；下层是否反向 include 上层。
2. **不变量是否可指认**：每条"必须成立"的约束，能不能指到一段代码作为守护点。
3. **故障是否可归因**：出错时能否从 `/status`、日志、回执三处定位到"哪一层、哪条不变量"。
4. **扩展是否只动一处**：换传输、换引擎、加并发，改动面是否被抽象点挡住。

第 2 条是本文的主线：一个分布式 KV 的复杂度几乎全部体现为不变量，代码量只是表象。

## 2. 系统全景

### 2.1 分层与依赖方向

```
入口        tinykv-cli（HTTP）           embed_example（C++ API）
                     │                            │
组合根      ┌────────▼────────────────────────────▼────────┐
server      │ ServerApp：Service/Worker/Session/Interceptor │
            │ StatusServer×2（业务口 + 观测口，各一 io 线程）│
            ├───────────────────────────────────────────────┤
控制面      │ RegionPeerRegistry · StoreMeta/Router · TcpTransport │
raftstore   │ Peer（提案/apply/快照收发/去重）               │
            ├──────────────────────┬────────────────────────┤
共识/事务   │ raft（Raft/RaftLog）  │ txn（MvccTxn/Latch/TSO/TxnClient）│
            ├──────────────────────┴────────────────────────┤
存储        │ Engine：WAL → memtable → SST，三列族物理同层   │
            └───────────────────────────────────────────────┘
横切        common：KvError/result · encode · crc32c · ids · log
```

实测依赖矩阵（`grep include` 全量核对）：

| 层 | 允许依赖 | 实际是否越界 |
|---|---|---|
| `common` | 无（仅标准库） | 否 |
| `storage` | common | 否 |
| `txn` | common、storage、scheduler/tso | 否 |
| `raft` | common、storage | 否 |
| `raftstore` | common、storage、raft、txn | 否 |
| `server` | 全部下层 + asio | 否 |
| `cli` | common（HTTP 客户端自持） | 否 |

**结论：单向依赖成立，无倒挂。** 这是本项目最扎实的一条结构性优点：`raft` 不知道 `raftstore` 存在，`txn` 不知道 `raft` 存在，因此两者可以分别用确定性消息序列和内存 Storage 单测。

### 2.2 运行时拓扑（5 条线程，全部 joinable）

| 执行者 | 数量 | 周期 | 持锁 | 职责 |
|---|---|---|---|---|
| main | 1 | 1 s（`cv::wait_for`） | 无 | 推进注入时钟 `now_ms_`、回收空闲会话 |
| Worker | 1 | 10 ms | `peer_mu_` | 全 peer `tick()` + `pump_locked()` |
| StatusServer（业务） | 1 | 事件驱动 | `peer_mu_`（handler 内） | `/kv`、`/txn/*`、`/region/split` |
| StatusServer（观测） | 1 | 事件驱动 | `try_lock` | `/status`、`/regions`、`/metrics` |
| TcpTransport | 1 | 事件驱动 | 内部队列 | Raft 消息收发 |

没有 detached 线程，没有 `sleep_for` 对齐启动——`Worker` 用 `condition_variable::wait_for`，`StatusServer` 用 `executor_work_guard` + `ctx.run()`。异常在泵循环与 HTTP `dispatch()` 两处被截住，不会穿出线程边界。

**架构含义：并发模型是"单写者 + 大闸"。** 所有 Peer 与 Storage 访问由 `peer_mu_` 串行；HTTP handler 在自己的线程里持闸泵共识，直到 apply 到位才返回。这换来的是"正确性可以用一把锁论证"，代价是任何慢请求都会阻塞全站（观测口用独立 io_context + `try_lock` + 缓存快照规避了这一半）。

### 2.3 写路径（raw KV，逐跳）

```
CLI POST /kv "put\tk\tv"
  └─ ServerApp::kv_write                       [持 peer_mu_]
       ├─ registry.require(k)                  StoreMeta 区间索引 → region → Peer
       ├─ peer.propose(req)                    seq = last_index+1；去重查 last_seq_
       │    └─ pending_proposals_.push_back    batching 默认开
       ├─ wait_applied_locked(peer, expect)    自泵 400 轮（多副本改墙钟 5 s + yield）
       │    ├─ pump_locked: transport.poll → step_message
       │    ├─ peer.on_ready: flush_proposals → raft.propose → take_ready
       │    │    ├─ apply_entry → execute → storage.write(批) + replies_ + last_seq_
       │    │    └─ raft.mark_applied(整批末尾)  → RaftLog 写 "\0la" 与 "\0lm" 同批
       │    └─ 释放 / 重试
       └─ peer.drop_reply(1, expect)           组合根不消费回执，必须显式回收
```

三个值得记住的设计点：

- **提案与 apply 同一把闸**，所以"提案后立刻泵到 applied"不会与后台 Worker 的 tick 交错，`last_index+1` 作为 seq 也就不必做 CAS。
- **回执表是显式的**：`take_reply` 取走，`drop_reply` 丢弃。没有隐式 TTL，漏掉任一条就是常驻进程的无界增长——这是 M6 评审抓到的真问题。
- **applied 是整批确认**：一批 100 条里第 100 条失败，前 99 条也不确认位点，重启整批重放。代价是重放要幂等，收益是不存在"半批已提交"。

### 2.4 读路径（线性一致读）

```
kv_read → peer.wait_read_index(pump, rounds, timeout)
   ├─ raft.read_index(): read_idx = committed()，acks=1，广播带 read_ctx 的心跳
   ├─ 每次心跳回执 acks+1，达多数派 → confirmed_reads_.push(read_idx)
   ├─ 调用方泵到 applied >= read_idx
   └─ peer.read_directly(Default, encode_raw_key(key))
```

**ReadIndex 三要素齐备**（发起位点取 committed、多数派确认、等 applied 追平），且 LeaseRead 默认关闭。这是与"leader 本地直读"的本质区别：多副本下即使新 leader 尚未 apply 旧 leader 已提交的条目，也要等 applied 追平才放行。

### 2.5 事务路径（Percolator）

```
CLI txn begin → TxnSessionManager::begin → TxnClient::begin → TSO 取 start_ts
CLI txn set   → 只在服务端会话里缓冲（不落盘、不加锁）
CLI txn commit→ TxnClient::commit:
      primary = min(key) 的字典序
      prewrite(writes, primary, start_ts, ttl=3000, now_ms)  → raft 提案
      commit_ts = TSO
      exec.commit([primary], start_ts, commit_ts)           → raft 提案
      exec.commit(secondaries, ...)                          → raft 提案
apply 侧：execute_txn_payload → Latch(按 key 全序) → MvccTxn → storage.write
```

`txn set` 不落盘这点很关键：**锁的 TTL 从 prewrite 才开始计时，与 commit 的间隔决定锁暴露窗口**；也意味着"set 了很多 key 的事务"在 commit 时需要一次性提案全部 mutations（单条日志条目），条目大小无上限保护。

### 2.6 Split 与快照

Split 走共识：`POST /region/split` → admin 提案 → apply 时 `replace_region` 原子替换区间（epoch.version 双 +1）→ observer 回调 → 新建 `regions/<id>` 引擎 → 三列族按 `[split_key, end)` 迁移（copy + delete）→ 新 Peer 单 voter 立即 tick 成 leader。

快照分块：4 MB/块 + 流内序号 + 逐块 CRC + 窗口流控（`next - ack < kSnapshotWindow`），接收端按 seq 落位、收齐重组、`install_snapshot` 先为"本地有、快照无"的键写墓碑再覆盖——这条"先删后写"是防分叉日志残留的关键。

## 3. 抽象点评估

只有五个点真正"可替换"，其余都是具体类型直连。逐个判分：

| 抽象 | 接口 | 替换成本 | 评价 |
|---|---|---|---|
| `Storage` | get/write/create_reader | 低（RocksDB 适配器只需实现 3 个方法） | **A**：接口最小且完备，Reader 快照语义清晰 |
| `Transport` | poll/send/set_peers | 低 | **B+**：能换 gRPC，但消息里无 region_id，多 region × 多 voter 路由歧义（代码已显式拒绝该组合） |
| `TxnExecutor` | 6 个虚函数 | 低 | **A-**：Local 与 Peer 两实现语义一致；但接口签名缺 region 归属信息，直接导致跨区写错分区（见 §5-P0-1） |
| `Service` | name/start/stop | 极低 | **A**：顺序启动 + 逆序停止 + 失败回滚，组合根的骨架立住了 |
| `TsProvider` | get_timestamps | 极低 | **A**：`CheckedTsProvider` 用 fatal 异常守单调性，取舍正确 |

**缺少的抽象点（按收益排序）**：

1. **Routing 没有抽象**：`StoreMeta` 被 `Peer`、`RegionPeerRegistry`、`ServerApp` 三处直接持有指针，`Router`（带 epoch 校验）只被测试使用。生产路径等于"绕过了自己定义的契约"（详见 §5-P1-6）。
2. **元数据持久化没有抽象**：region 拓扑只活在内存 `StoreMeta` 里，没有 Repository。这是 M7 ⑧ 的动因。
3. **请求分派没有抽象**：`ServerApp::admin_get/admin_post` 是 if-else 链，CLI 与服务端的端点集合已经漂移（§5-P1-1）。

## 4. 结构度量

| 指标 | 实测 | 判读 |
|---|---|---|
| 头文件占比 | 55%（5 461/9 965） | 偏高。`server_app.h` 711 行全是内联实现，改一行触发全量重编 |
| 最大文件 | `peer.cpp` 829、`raft.cpp` 787、`server_app.h` 711 | 前两个是"一个类一件事"的合理体量，第三个是 God class |
| 单函数最长 | `ServerApp::admin_get` / `status_json_locked` 手拼 JSON | 手拼 JSON 不转义 `\u0000`–`\u001f`，键值含控制字符即产出非法 JSON |
| 错误通道 | 全接口 `result<T>`，仅 `TsNotMonotonic` 一处 throw | **A**：异常只用于"不变量破坏"，与项目约定一致 |
| 强类型 ID | `Term/LogIndex/RegionId/PeerId/Ts` 均为 struct | **A**：跨层不混用裸 `uint64_t` |
| 测试密度 | 9 289 行测试 / 9 965 行源码 ≈ 0.93 | 课程级项目里属高密度；四层（unit/simulation/integration/bench）齐备 |

`server_app.h` 的 711 行里装了：装配、HTTP 分派、JSON 序列化、时钟、计数器、拦截器持有、split 编排。它同时是组合根、控制器和视图——这是 **M7 ⑨c 拆分**的唯一理由，也是最容易改出回归的地方。

## 5. 架构级问题清单

按"会不会写错数据"排序，不按"改起来难不难"。

### P0-1 跨区事务静默写错分区

`PeerTxnExecutor::prewrite` 把整批 mutations 经 **primary 所在 peer** 一次提案；`peer_txn.h:50` 签名里根本没有"key 属于哪个 region"的信息。`Peer::propose_txn_prewrite` 只封包，不校验 key 归属；而读路径按 key 路由。

后果：primary 在 region 1、secondary 在 region 2 时，两把锁和两份数据全落 region 1 的引擎；读 region 2 得到空。对外表现是"commit 返回 OK，数据不见了"。**这是正确性洞，不是能力缺口**——护栏必须先于协调器落地。

### P0-2 锁 TTL 的时间基准在传输中丢失，锁"出生即过期"

链路：`TxnClient::commit` 把 `now_ms` 传给 `exec_.prewrite(...)` → `PeerTxnExecutor::prewrite` 的形参写作 `std::uint64_t /*now_ms*/`（**丢弃**）→ `kvpb::TxnPrewriteRequest` 只有 `lock_ttl`、没有 `created_ms` 字段 → apply 侧 `run_txn_prewrite` 调 `mvcc.prewrite(..., 0)` → `LockValue.created_ms = 0`。

于是 `check_txn_status` 计算 `elapsed = now_ms - 0`。主循环每秒 `advance_clock(1000)`，**进程跑过 3 秒后，任何新写的锁都会被判为"已过期"**，冲突方随即走 `resolve_locks_of(..., nullopt)` 把活事务的锁回滚成墓碑。单线程冒烟测试看不出（事务活不过 1 秒），并发下就是隔离性破裂。

根因是接口而非实现：时间这种跨层语义没有随请求传递，只能靠服务端本地时钟猜。

### P0-3 去重只覆盖一半路径，且不持久

- raw 路径：`Peer::propose` 查 `last_seq_[client_id]`，`seq <= applied_seq` 直接返回成功。
- txn 路径：载荷 `[0x2B][type][kvpb]` 里**没有 client_id/seq**，完全不去重，只靠 MVCC 自身的语义幂等兜底。
- 两张表都在内存：`last_seq_`、`replies_` 重启清空。

三个后果：重试语义按接口不一致；重启后同一请求可二次 apply（对 put 无害、对 del+put 组合有害）；去重命中时**只返回成功、不登记回执**，调用方若按"applied 到位点"等待会空转到超时。

### P0-4 重启后"只读不写"不可用：applied 双游标只播种了一个

三个位点，两处恢复，一处漏掉：

| 位点 | 类型 | 重启后取值 | 依据 |
|---|---|---|---|
| `RaftLog::applied` | 持久化（`\0la`） | N（正确） | `RaftLog::open` 读 meta |
| `Raft::applied_` | 内存 dispatch 游标 | `min(applied, committed)` = N（正确） | `Raft::Raft` 构造函数 |
| `Peer::applied_` | 内存 apply 游标 | **0（漏）** | 成员默认初始化，全文件只有 `peer.cpp:351` 一处赋值 |

后果链：重启后 `Raft` 构造时按 `applied_ = min(...) = committed` 计算，`collect_apply()`
无条目可灌 → 没有 apply 批次 → `Peer::applied_` 永远停在 0 → `wait_read_index` 的
`peer.applied_at_least(read_idx)` 恒假 → 每次读都要自旋到超时，返回
`IoFailed: applied 未追上确认位点`。写一笔之后 `applied_ = last` 才被修正，读立刻恢复。

这条最能说明"同一语义两个副本"的危险：M7 ① 把 applied 持久化做对了（`\0la` + 整批确认），
但只在"崩溃重放有缺口"的路径上验证过。当 `applied == committed`（无缺口）时，
重放队列是空的，第二条游标没有任何机会被赋值——测试覆盖的是"有缺口"的分支。

最小修法：`Peer` 构造时 `applied_ = log_->applied().value`，并补一条"重启后不写先读"的回归。
复用 `build/live/verify.py` 的场景即可复现，不需要新工具。

### P1-1 CLI 端点与服务端已漂移

服务端 `admin_get` 只认 `/txn/get`、`/txn/status`、`/regions`、`/region`、`/trace`、`/metrics`；`admin_post` 只认 `/txn/{begin,set,del,commit,rollback,gc}`、`/region/split`。

CLI 却发了三个无人接的请求：`/region/peers`（`cli.cpp:333`）、`/scheduler/status`（`cli.cpp:358`）、`POST /trace` 开关（`cli.cpp:373`）。它们会命中兜底分支返回空串 → 404。`scheduler.h` 里的 PD-lite 调度器因此**在守护进程里没有任何入口**，只有库形态。

### P1-2 契约声明了 epoch/peer 校验，服务端不校验

`RaftRequestHeader` 注释写"epoch 必填（落后则回 StaleEpoch）""peer 必填（错则回 NotLeader+hint）"，但 `kv_write` 只填 `client_id`/`sequence_id`，`region_id`/`epoch`/`peer` 全空；路由走 `registry_->require(key)`，不做世代比较。带 `epoch` 校验的 `Router` 类只在 `tests/unit/raftstore/region_route_test.cpp` 里被实例化。

结论：`StaleEpoch` 这个错误变体在生产路径上永远不会产生。契约与实现之间存在一条"写了但没接"的缝——这比没写更危险，因为评审会以为它生效了。

### P1-3 split 元数据不持久，重启即回单 region

`StoreMeta` 全在内存，`regions/<id>` 目录下的数据在重启后无人纳管：`bootstrap()` 固定建 region 1 并复用 `data_dir` 根，`regions/*` 成为孤儿目录。同时 `apply_split_admin` 的 observer 若在建右半 peer 前失败，路由已指向一个没有 peer 的 region，`require()` 会兜底返回首个 peer（`peer_registry.h:109`）——**静默落到错误的 peer**，而不是报错。

### P1-4 会话回收用"创建时刻"而非"最后活跃"

`sweep` 比较 `now_ms - created_ms >= 30 s`，而 `set/get/commit` 都不刷新 `created_ms`。语义上这是"会话寿命上限"，被写成了"空闲回收"。一个持续 31 秒的长事务会被判空闲、走 rollback 清锁。

### P1-5 全表扫描型操作没有索引

- `MvccTxn::locks_of` 扫全 Lock CF（上限 4 096）找"同 primary 的锁"，`resolve_locks_of` 每次决议调用一次。
- `RegionPeerRegistry::peer_by_peer_id` 线性遍历所有 slot，**每条 Raft 入站消息调用一次**。
- `GC::prune_user` 依赖 Write CF 全表扫（上限 1 MiB 条），且把 `by_user` 全量物化在内存。

Region 数到几十、锁数到几千时，这三处会成为第一瓶颈。

### P1-6 应用层错误可观测但不完整

`Peer::apply_error_count/last_apply_error` 已进 `/status`（M7 ④），但 `txn_errors_` 以日志位点为键、只有被 `take_txn_error` 取走才 erase。调用方超时放弃或换路径取走时，残留项无人清理；`replies_` 同理（`drop_reply` 需要调用方自觉）。

### P2 级（记录，不改）

- `Raft::pending_reads_` 只在达多数派时 erase，心跳丢包则该轮次常驻（无 deadline）。
- 每 region 一份 Engine → 文件句柄与 memtable 内存 ×N，无共享引擎（SPEC-6 已裁定移出 M7）。
- 成员变更只靠日志重放重建 `voters_`，`cfg_.voters` 不持久化；日志压缩后无法重建，重启必须由启动参数给出同一 voters 列表。
- `Raft::apply_conf_change` 只改内存成员表，不校验"移除自身后如何收尾"。
- 手拼 JSON 不转义控制字符（`status_json_locked`、`kv_read` 的值字段已用 `escape`，`region_json` 未全覆盖）。

## 6. 不变量清单

架构分析的核心交付物是这张表：每条不变量都能指到守护点，破坏后有可观察后果。

| ID | 不变量 | 守护点 | 破坏后果 | 现状 |
|---|---|---|---|---|
| INV-01 | 任意 key 恰属一个 region | `StoreMeta::add_region/replace_region` 重叠校验 | 双写/读空 | ✔ 内存内成立 |
| INV-02 | 写必须经共识，apply 只由 committed 驱动 | `Peer::propose` → `Raft::propose`；`on_ready` 只 apply `Ready.to_apply` | 单机正确、多副本丢写 | ✔ |
| INV-03 | `applied` 独立持久化，整批写完后前移 | `Raft::mark_applied` + `\0la` | 崩溃窗口内已提交条目永不重放 | ✔（M7 ①） |
| INV-04 | term/vote 先落盘再对外发消息 | `campaign` / 投票分支内联 | 一任期双 leader | ✔ |
| INV-05 | 同 `(client_id, seq)` 不二次 apply | `Peer::propose` + `execute` 尾部 `last_seq_` | 重试变重复写 | ◐ 仅 raw、仅内存 |
| INV-06 | 事务生死只由 primary 状态决定 | `MvccTxn::check_txn_status` | 双决议、半提交 | ✔ 但受 P0-2 影响 |
| INV-07 | 同 key 并发提案在 server 侧串行 | `LatchManager::acquire` 全序加锁 | 单行原子性破裂 | ✔ |
| INV-08 | TSO 严格单调不回退 | `CheckedTsProvider`（fatal） | 快照读看到未来数据 | ✔ |
| INV-09 | 凡进引擎的 key 都经 encode 函数 | `encode_key/encode_ts_key/encode_raw_key` | CF 串写、split 误判 | ✔ |
| INV-10 | Reader 快照不可变 | `Engine::mutable_mem_locked` CoW | 读到半批数据 | ✔ |
| INV-11 | 读取 `commit_ts ≤ start_ts` 的最大版本 | `MvccTxn::find_write` 升序覆盖 | 读到未提交/旧值 | ✔ |
| INV-12 | Entry 首字节 `0x2B` 即 txn 载荷 | `Peer::apply_entry` | 分派错、反序列化失败 | ✔ |
| INV-13 | 一次提案内多条命令的原子性 = 一个 Storage 批 | `Peer::execute` 单批 write | 半批可见 | ✔ |
| INV-14 | 快照安装先删"快照无而本地有"的键 | `install_snapshot` 墓碑先行 | 分叉脏键永久残留 | ✔ |
| INV-15 | split 后的左/右区间首尾相接且覆盖原区间 | `StoreMeta::replace_region` 三键校验 | 区间空洞/重叠 | ✔ |
| INV-16 | `Peer::applied_` 与 `RaftLog::applied` 语义一致 | `Peer` 构造 / `on_ready` 尾部 | 重启后读路径永不放行（P0-4） | ✘ 构造未播种 |

**INV-05、INV-06、INV-16 是当前三个"半成立"的不变量**，分别对应 P0-3、P0-2、P0-4。

## 7. 与同类实现的取舍对照

| 决策点 | 本项目 | TiKV | etcd | 判断 |
|---|---|---|---|---|
| raft log 存放 | 复用自研 KV 引擎（`\0re` 前缀） | RocksDB 单实例多 CF | 独立 WAL 文件 | 合理：少一套段管理，代价是 flush 策略耦合 |
| 每 region 引擎 | 一 region 一 Engine | 共享单实例 | 无 region 概念 | 课程级可接受；规模上不成立（⑨b 待测） |
| 读一致性 | ReadIndex 必做，Lease 关 | ReadIndex + LeaseRead | ReadIndex + Lease | 保守但正确，符合"默认不建立在未验证假设上" |
| 成员变更 | 单步 + Learner | 单步 + Learner | Joint Consensus | 与 TiKV 同路；Joint 的复杂度在本项目 ROI 为负 |
| 事务协调 | client 驱动单 region 2PC | TiKV 内部 2PC + 外置 GC | 无 | 缺跨区协调（P0-1） |
| 并发 | 一把大闸 | per-region latch/raftstore 线程 | 单节点串行 apply | 自知取舍，但已到收益拐点 |

## 8. 结论

**组件层 A-，装配层 C+，运维层 D。**

- 组件层：Raft 状态机纯净（零锁零 IO）、MVCC 三列族完整、存储崩溃恢复有 1 000 条随机写回归、错误通道统一 —— 这些是"内核骨架造得对"的证据。
- 装配层：多副本能力已进组合根（M7 ⑤），但 epoch 校验未接、调度器无入口、CLI 三个端点悬空、跨区事务写错分区 —— 属于"零件齐了但有几根线没插"。
- 运维层：无鉴权、无 TLS、无在线配置、无 region 元数据持久化、观测口只有计数没有直方图。

**下一步不该是加功能，而是把 P0-4（重启读不可用）、P0-2（TTL 基准）、P0-1（跨区护栏）、P0-3（去重契约）
四条"静默出错/静默不可用"堵住。** 具体任务卡与依赖顺序见 `docs/ARCH-3-演进规划与工程布局.md`；
数据布局与接口契约的逐条定义见 `docs/ARCH-2-接口与数据结构设计.md`。

## 9. 修订记录

| 日期 | 变更 | 状态 |
|---|---|---|
| 2026-11-18 | 基线建立（HEAD `be8f54b`），度量与问题清单按代码实测 | — |
| 2026-11-18 | §5 P0-1/P0-2/P0-3/P0-4 四条全部处理：跨区护栏（`PeerTxnExecutor::guard_single_region`）、锁 TTL 基准随 `created_ms` 传递、去重命中登记合成回执（`propose` 返回 `result<bool>`）、`Peer` 构造从 `RaftLog::applied()` 播种 | 已修复，见 `docs/bugs.md` 0006–0009 与 `diff.md` 条目 037 |

**修复后仍需跟踪的项**：§5 的 P1-1（CLI 三个悬空端点）、P1-3（split 元数据不持久）、
P1-5（全表扫描型操作无索引）、P1-6（应用层错误回收不完整）仍成立，按
`docs/ARCH-3-演进规划与工程布局.md` 的 W2–W4 波次推进。P0-3 的"txn 路径不去重"部分
（载荷无 client_id/seq）未在本次处理——它属于 INV-05 的完全形态，依赖日志载荷版本化，
排在去重台账持久化一起做。
