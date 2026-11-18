# 代码质量审查报告（2026-11-18）

> 范围：`src/`（5934 行）+ `proto/` 全量精读，`tests/`（9289 行）抽样核对。
> 方法：逐模块静态审查 + 实际构建 + 全量测试执行 + 文档声明与实现对照。
> 结论按"是否会导致错误结果 / 数据丢失 / 崩溃"分级，不含纯风格偏好。

---

## 0. 实测基线（先给事实，再给判断）

| 项 | 结果 |
|---|---|
| 增量构建 | 通过，124 个目标，`-Wall -Wextra -Wpedantic -Werror` 零告警 |
| 单元测试 | 45/45 通过（13.4s） |
| 集成 + 模拟测试 | 13/13 通过（28.2s，含 raft/mr/txn chaos 各 150 轮） |
| 测试规模 | 255 个用例、1370 条断言、测试:源码 ≈ 1.6:1 |
| 登记表一致性 | `check_test_registry.py` PASS（65 登记 = 65 add_test = 65 文件） |
| 文档纪律 | bugs.md 5 条缺陷全部带可执行复现命令 + 回归用例编号 |

**总体评价**：工程规范属于同类课程项目里的上游水平——文档即契约、缺陷登记可执行、门禁真实生效、
注释解释"为什么"而不是"做了什么"。但**正确性与持久化语义上存在若干会把承诺打穿的缺陷**：
README/SPEC 声称的 REL-02（崩溃安全）与事务隔离，在几条具体路径上并不成立。
建议定位为"架构与工艺 A-，可靠性 B-"。

---

## 1. 严重问题（P0：会导致错误结果、数据丢失或崩溃）

### P0-1 生产数据路径硬编码关闭 fsync，与 REL-02 承诺相反

`src/server/peer_registry.h:63`、`:142`

```cpp
auto eng = Engine::open({.dir = data_dir_, .fsync_on_commit = false});
auto eng = Engine::open({.dir = data_dir_ / "regions" / ..., .fsync_on_commit = false});
```

`Engine` 侧注释明确写着"`fsync_on_commit=false` 关掉仅供基准对比，掉电会丢窗口内数据"，
`config.h` 默认值也是 `true`，但**常驻服务的唯一装配点写死了 false**，且 `main.cpp` 根本不读
`demo.yaml`（配置模块只被 CLI 使用）。后果：

- 机器掉电/断电时，已向客户端返回成功的写入会丢失（页缓存未落盘）；
- 更严重的是 Raft 语义：`raft.h:10` 注释称"先持久化 term/vote 再对外发消息"，但底层 WAL 在
  `fsync_on_commit=false` 下只做 `fflush`，多个节点同时掉电可能丢失已提交日志，
  从而破坏 State Machine Safety（已提交条目丢失），这不只是可用性问题。

**建议**：装配点改为从配置读取，默认 true；把 `fsync_on_commit=false` 限定在 bench/测试装置内
（`tests/bench/cluster_bench_test.cpp:114` 那种用法是合理的）。

---

### P0-2 TSO 每次进程启动从 0 重新计数，重启后事务读失效

`src/scheduler/tso.h:53`（`std::uint64_t counter_{0}`）+ `src/server/server_app.h:687`（成员，无恢复逻辑）

无持久化、无物理时钟基线、无"从数据最大 commit_ts 恢复"的任何路径。设重启前最大 commit_ts = 1000，
重启后新事务 start_ts = 1：

- `MvccTxn::find_write` 只认 `commit_ts <= start_ts`，于是**所有历史版本都读不到**，返回"不存在"
  （不是报错，是静默空值）；
- `latest_commit_bound` 返回的 max commit_ts 远大于 start_ts，于是**任何写都命中 WriteConflict**
  → 事务面在重启后实际不可写；
- 违反"ts 严格单调"这条被列为红线 6 的不变量（虽是被测的 `CheckedTsProvider` 定义，却未接入服务：
  该装饰器目前**只在 `tests/unit/txn/tso_test.cpp:74` 使用**，生产装配用的是裸 `MonotonicTsProvider`）。

同时存在"多时钟"问题：TSO 计数器、`ServerApp::now_ms_`（每秒 +1000 的注入时钟）、
`steady_clock` 超时三者互不相关，语义容易互相污染。

**建议**：TSO 采用物理时间基线（`max(last+1, physical_ms<<18)`）并在启动时用
`max(已持久化 commit_ts, 物理基线)` 恢复；把 `CheckedTsProvider` 接进装配。

---

### P0-3 锁 TTL 链路断裂：`created_ms` 永远为 0，长跑后活跃锁被判过期

链路三处断点：

1. `proto/kv.proto` 的 `TxnPrewriteRequest` **没有 `created_ms`/`now_ms` 字段**（只有 `lock_ttl`）；
2. `src/server/peer_txn.h:53` 把注入参数直接丢掉：`std::uint64_t /*now_ms*/`；
3. `src/raftstore/peer.cpp:484` 写死 `mvcc.prewrite(muts, req.primary_lock(), Ts{...}, req.lock_ttl(), 0)`。

于是 `LockValue.created_ms` 恒为 0，而 `check_txn_status` 用 `now_ms - created_ms` 判过期
（`src/txn/mvcc.h:372`）。常驻服务的 `now_ms_` 每秒 +1000：

- **进程运行超过 TTL（默认 3s）之后，任何锁都被立即判为"已过期"**；
- 另一事务读到 LockConflict → `resolve_blocking` → 判定过期 → `resolve_locks_of(rollback)`
  → **把一个仍在 prewrite 与 commit 之间的活事务的锁强制回滚**，制造大量伪冲突/伪回滚；
- 现有测试之所以全绿，是因为测试里 `now_ms` 也常从 0 开始、`elapsed` 恒为 0 → TTL 分支永不触发，
  **这条路径实际上没有被测试覆盖**。

**建议**：proto 增加 `created_ms`（属接口契约变更，按项目规则需先与用户核对）；服务端在 apply
时用确定性时钟写入；补一个"锁在 TTL 内不被他人回滚"的回归用例。

---

### P0-4 MvccTxn 把 IO 错误折叠成"不存在"——读错误当空值、写绕过锁

`src/txn/mvcc.h` 多处：`load_lock:392`、`find_write:476`、`find_write_exact:498`、
`find_commit_ts_of:507`、`latest_commit_bound:529`、`for_each_write_version:449`、`locks_of:279`

```cpp
auto r = storage_.get(Cf::Lock, as_bytes(key));
if (!r.has_value() || !r->value.has_value()) {
    return std::nullopt;      // ← IoFailed（磁盘错误/引擎已关闭）被当成"没有锁"
}
```

后果是语义级的：一次 `IoFailed` 会被读路径解释为"该 key 无值"（返回不存在的值），
被写路径解释为"该 key 无锁"（于是 prewrite 直接覆盖别人持有的锁）。
**在"错误即返回空"的语义下，一次瞬时 IO 错误就能造成幻读与写冲突漏检**，而不是返回一个可重试的错误。
这与项目"全接口 `result<T>` + 结构化错误"的自身约定相悖。

同类问题还出现在：
- `Peer::fill_get_reply:403` / `fill_scan_reply:413`（存储错误 → 回执 found=false / 空列表）；
- `PeerTxnExecutor::gc` 里的 `(void)p->on_ready()`；
- `ServerApp::pump_locked:281,286` 的 `(void)step_message` / `(void)on_ready`；
- 全项目 35 处 `(void)` 丢弃 `result`。

**建议**：区分"缺失"（`std::optional` 空）与"失败"（`result` 错误）两层类型——
即 `result<std::optional<T>>`，不要用单层 optional 同时表达两者。

---

### P0-5 `Engine::flush_locked` 中途失败后留下空 WAL 指针，下一次写入空指针解引用

`src/storage/engine.cpp:455-467`

```cpp
wal_.reset();                                    // ① 先置空
if (auto r = Wal::clear_all(wal_dir_); !r) {
    return r;                                    // ② 失败直接返回，wal_ 仍是 nullptr
}
auto wal = Wal::open(...);
if (!wal) { return std::unexpected(wal.error()); }  // ③ 同样留下 nullptr
```

而 `Engine::write:406` 无任何判空：`if (auto r = wal_->append(batch); !r)`。
一旦 `clear_all` 或 `Wal::open` 失败（磁盘满、文件被占用、Windows 删不掉被打开的文件），
引擎进入"每次写都解引用空指针"的状态——**进程直接崩溃**，且 `~Engine()` 里 `flush_locked`
重入同一条路径。

**建议**：先构造新 WAL 成功后再替换（`wal_ = std::move(*wal)`），删除旧段放在最后；
或在 `write` 前显式判 `wal_ != nullptr` 并返回 `IoFailed`。

---

### P0-6 RaftLog 的"删条目 + 改 meta"分两次写，崩溃窗口内元信息与数据不一致

`src/raft/raftlog.cpp:210-224`（`truncate_from`）、`:226-244`（`compact`）

```cpp
if (auto r = remove_range(idx, LogIndex{meta_.last_index + 1}); !r) return r;
meta_.last_index = idx.value - 1;
return persist_meta();     // ← 第二次独立写
```

`persist_meta` 内部把 meta+applied 合成一批（这点做对了），但**与条目删除不是同一批**。
崩溃落在两次写之间：

- `truncate_from`：条目已删、`last_index` 仍指向旧值 → 重启后 `at(last_index)` 返回
  `InvalidArgument{"entry 不存在"}` → 恢复路径直接失败；
- `compact`：条目已删、`first_index` 仍是旧值 → `term(idx)` 走 `at()` 失败 → 返回 `Corrupted`；
  更糟的是这个 `Corrupted` 在 `raft.cpp:107/399/530` 处被 `t ? ... : 0` 吞成"term = 0"，
  于是 prevLogTerm 判定静默失真。

同类"非原子多步"还有 `append`（entries 批 + meta 批，`raftlog.cpp:191-195`）。

**建议**：把"删条目 + 新 meta"合进同一个 `ModifyVec` 一次 `storage_.write`（引擎的批次是原子的）；
顺带把 `(void)log_.set_committed/append/truncate_from` 这些丢弃错误的位置改为向上传播。

---

### P0-7 `Peer::applied_` 未从 `RaftLog::applied()` 播种，重启后读路径超时

`src/raftstore/peer.h:230`（`std::uint64_t applied_{0}`）+ `src/raftstore/peer.cpp:53-56`（构造未赋值）
+ `peer.cpp:351`（全文件唯一赋值点，只在 `applied != 0` 时执行）

机器重启后 `log.applied == log.committed == N`（上次已正常 apply 完），于是：

1. `Raft` 构造里 `applied_ = min(applied, committed) = N` → `collect_apply` 无事可做 →
   `Ready.to_apply` 为空；
2. `Peer::applied_` 因此**永远停在 0**，而 `Raft::applied_` 已是 N；
3. `wait_read_index` 的 `pump_until_applied` 判 `applied_at_least(N)` 恒为假 → 400 轮后报
   `"applied 未追上确认位点"`；`wait_applied_locked` 同理。

净效果：**重启后第一次读请求必然失败**（直到有一次写把 `applied_` 推上去），
即"重启到可服务"这条 UX 承诺在数据非空的库上不成立。
`applied_recovery_test` 六例全部覆盖的是"applied < committed 的缺口重放"分支，
恰好绕开了 `applied == committed` 这条最常见路径 —— 这是一个典型的"测试补在了分支旁边"。

**建议**：构造函数末尾 `applied_ = log_->applied().value`；补一个
`RestartWithNoPendingEntriesStillReadable` 用例。

---

## 2. 中等问题（P1：可靠性/健壮性/性能上的实质缺陷）

| # | 位置 | 问题 |
|---|---|---|
| P1-1 | `storage/wal.cpp:197-207` | 后台提交者 `fflush`/`fsync` 失败只 `continue`：**已向调用方确认的写永远丢了**，且循环每 2ms 空转重试，错误无人可见。至少要置错误状态让后续 `append` 返回 `IoFailed`。 |
| P1-2 | `storage/sst.cpp:149-158` | SST 写盘只 `fflush` 不 `fsync`；`fwrite` 短写时**半截文件留在 sst/ 目录**。而 `Engine::load_manifest` 清单缺失时回落到目录扫描（`engine.cpp:261`），会把半截表当合法表加载 → `Corrupted` → 启动失败。写临时名再改名 + fsync + 失败即删。 |
| P1-3 | `storage/engine.cpp:285-290` | MANIFEST 用 `remove(dst)` + `rename(tmp,dst)`：中间窗口清单不存在。POSIX 上 `rename` 本就原子覆盖，先删反而制造窗口。 |
| P1-4 | `raftstore/peer.cpp:759` | `recv.chunks.assign(recv.total, ...)`，`total` 来自网络且为 u32，未按 blob 上限校验 → 恶意/损坏的 leader 可令接收端一次分配数 GB（`bad_alloc` 直接终止进程）。必须校验 `total <= kMaxSnapshotBytes / chunk`。 |
| P1-5 | `raftstore/peer.cpp:536-537` | `payload[1]` + `payload.substr(2)`：载荷长度 <2 时 `substr` 抛 `std::out_of_range`，异常从 apply 路径穿出。第 377 行只判了 `data()[0]`。 |
| P1-6 | `txn/mvcc.h:279-314` | `locks_of` 扫描上限 `kScanLimit = 4096`，`resolve_locks_of` 据此清锁：大事务（>4096 个 key）会**静默遗留锁**，锁永不清除；返回值还是"成功"。 |
| P1-7 | `server/peer_registry.h:293` + `:228` | split 迁移上限 `1<<20` 条：超出部分不迁移也不报错（右 region 数据静默丢失）；且 `copies`/`removes` 全量驻留内存，大 region 时内存翻倍。 |
| P1-8 | `raftstore/peer.cpp:76`、`:649` | 去重表 `last_seq_` 只在内存：重启后客户端重试会被**二次 apply**（正是注释里担心的"删了又写回"）。TiKV 的 apply state 是持久化的。 |
| P1-9 | `raftstore/peer.cpp:436-462` | `propose_txn_*` 返回 `bool`，把 `NotLeader`/`InvalidArgument` 的区别全丢了，上层只能伪造一个 `NotLeader`（`peer_txn.h:196`）。违反自家"错误码不进 bool"的清单。 |
| P1-10 | `raftlog.cpp:191-195` | 每次 `append` 两次 `storage_.write`（条目批 + meta 批），每次 `mark_applied` 一次 `persist_meta`：写放大 ~2-3 倍，且多一个崩溃窗口（见 P0-6）。 |
| P1-11 | `raftstore/tcp_transport.cpp:148-167` | 每条消息新建一条 TCP 连接（connect → write → close），`host_` 恒为本机监听地址 → 无法跨机部署 + TIME_WAIT 压力；`inbox_` 无上限、accept 后无读超时 → 半开连接可无限堆积。 |
| P1-12 | `server/http_helpers.h:65-74` | `escape()` 只转义 `"` 和 `\`，值里的 `\n`/`\t`/控制字符会产出**非法 JSON**；`region_json` 的 start/end key 完全不转义；`interceptor.h:142,176` 的 `/metrics`、`/trace` 拼接同样不转义 path。 |
| P1-13 | `server/peer_registry.h:109-116` | `require()` 找不到 key 时**回落到第一个 peer**：把"路由失败"变成"打到错误 region 上读写"，是数据错乱级别的静默兜底；`slots_` 为空时还解引用 `begin()`。 |
| P1-14 | `txn/client.h:58-60` | 空 `if` 块（只有注释），死代码；`commit` 在"锁被占"分支伪造 `LockConflict{.primary="", .lock_start_ts=0}`（`client.h:120-122`），与 `error.h:69`"未知时必须为 nullopt，禁止伪造"的自我要求冲突。 |
| P1-15 | `raft/raft.cpp:775` | `matches[majority()-1]`：`voters` 为空时越界（`propose_conf_change` 只挡了 leader 自己那条路，apply 路径无守卫）。 |

---

## 3. 轻微问题（P2：可读性/一致性/工程卫生）

**代码卫生**

- `src/raftstore/peer.cpp:8` 与 `:16` 重复 `#include "common/error_format.h"`，`<utility>` 重复两次，
  include 分组顺序也乱了（第 10-17 行夹在两组之间）；`as_string(const std::string&)` 是纯拷贝的空壳函数。
- `src/txn/mvcc.h:215` 残留 `// DEBUG` 注释；`:536` 有 `std::fprintf(stderr, "[dbg] ...")` 调试输出
  —— 项目规定日志统一走 spdlog，这条是漏网的 `stderr` 打印。
- `src/server/server_app.h:71-74` 用裸 `new MetricsInterceptor()` 再交给 `unique_ptr` 接管：
  违反 `CLAUDE.md §4-4`"无裸 new/delete"的自身红线，且 `add()` 抛异常时泄漏。
  应 `auto m = std::make_unique<...>(); metrics_ = m.get(); add(std::move(m));`
- `server_app.h` 711 行、`mvcc.h` 569 行（400+ 行实现写在头文件里）：`http_helpers.h` 已经做过一次
  "拆 God class"（注释自称），但业务处理器（kv/txn/split/观测）仍全部内联在组合根头文件里，
  每次改业务逻辑都触发全量重编。

**语义/一致性**

- 两套时钟口径并存（注入 `now_ms_` / `steady_clock` 超时 / TSO 计数器），`~Worker` 的
  `cv_.notify_all()` 未持 `cv_mu_`（最多 10ms 延迟唤醒，无实质危害但属 CV 用法瑕疵，`worker.h:58,91`）。
- `Raft::step` 收到 `MSG_HUP` 时无条件 `campaign()`（`raft.cpp:228`）——leader 收到会被自己抬 term 掉线。
- `Raft::handle_append` 中 `prev+1 < first` 回执用 `committed`、追平路径用 `last_index`
  （`raft.cpp:617` vs `:570`）：两处口径不同，容易在压缩边界来回拉锯。
- `Peer::execute` 在执行完整个 `batch` 的 `storage_.write` **之前**就 `fill_get_reply`
  （`peer.cpp:630`）：同一 `RaftCmdRequest` 里 `[put k, get k]` 读到的是旧值。
- `MemTable` 用 `std::map` + 每次 `Engine::get` 都要拿全局 `mu_` 并线性回扫全部 SST
  （`engine.cpp:386-393`）：无 bloom filter / 无 block 索引，scan 与 get 都是 O(#SST)；
  `compact_ssts_locked` 把整库读进 `std::map<string,optional<string>>`
  （`engine.cpp:483-488`）——内存峰值 = 全库数据，这是明确的规模化天花板。
- `TxnClient` 头注释承诺"冲突重试上限 3 次、退避 10/50/200ms（D4-5）"，代码里没有任何退避实现
  （注释推给"调用方"，但调用方 = 会话管理器也没做）。

**未接线/死配置**

- `src/config/` 完整实现了解析 + `demo.yaml`，但 `src/server/main.cpp` 只读两个 argv，
  配置模块事实上只服务 CLI。
- `CheckedTsProvider`（红线 6 的"致命校验"）仅存在于单测。
- `raftstore::Router`（含 epoch/StaleEpoch 校验，`region.h:185-206`）**生产路径零调用**：
  组合根直接走 `StoreMeta::find_region` + `RegionPeerRegistry::require`，
  客户端的 epoch 既不校验也不回传 `StaleEpoch` —— REL-05 的"路由过期"语义实际未生效。
- `examples/`、`tools/` 下 `__pycache__` 已入库（建议确认 `.gitignore` 覆盖）。

---

## 4. 值得肯定的部分

1. **错误模型**：`KvError` 九变体 + `result<T>` + `RegionEpoch` 刻意不提供 `<=>` 只给
   `is_newer_than/is_comparable_with`——后者是有洞察力的设计决策（避免混序被词典序误判）。
2. **Raft 分层**：状态机零锁零 IO、`Ready` 单端输出、确定性 tick 代替墙钟、
   红线（只统计当前 term 推进 commit、learner 不计多数派、压缩边界回执）在代码里逐条有注释对应实现。
3. **WAL**：段文件 + 逐条 CRC + group commit，`take_bytes`/`decode_batch` 全部有界检查，
   `kMaxRecordBytes` 防坏长度爆内存；flush 顺序（先 SST→清单→清 WAL）本身是对的，缺的是 fsync 与失败回滚。
4. **测试纪律**：bugs.md 每条缺陷都有可执行复现命令 + 回归用例名 + 日期，且 CI 强制校验；
   255 用例 / 1370 断言 / 150 轮 chaos 三层齐备——这不是装饰。
5. **文档即契约**：`CLAUDE.md` / `AGENT_EXECUTION_PROTOCOL.md` / `docs/SPEC-*` / `diff.md` /
   `implementation-notes.md` 的分工清晰，且代码注释普遍解释"为什么"（如 `memtable.h:36` 关于
   `try_emplace` 会提前移走 value 的坑），这种密度在同类项目里少见。

---

## 5. 建议的处理顺序

| 优先级 | 事项 | 理由 |
|---|---|---|
| 1 | P0-7 `applied_` 播种 + P0-5 空 WAL 指针 | 各 1-3 行改动，分别修复"重启不可读"与"必崩" |
| 2 | P0-1 fsync 装配 | 一行改动 + 一处配置接线，直接决定 REL-02 是否成立 |
| 3 | P0-6 RaftLog 批原子性 + P1-1 fsync 失败可见 | 崩溃安全的核心；改动集中在 `persist_meta` 的调用方式 |
| 4 | P0-2 TSO 恢复 + P0-3 锁 TTL 链路 | 事务面在生产语义下不可用；P0-3 需先动 proto（按项目规则要用户批准） |
| 5 | P0-4 错误分层（`result<optional<T>>`）| 侵入面最大，建议单独立项、分批改，先改读路径 |
| 6 | P1-4/5/6/7 边界校验 + 上限显式报错 | 都是"静默截断/静默越界"，属于会藏 bug 的类型 |
| 7 | P1-11/12 + P2 卫生项 | 可在下一次提交里顺手清掉（重复 include、裸 new、`fprintf`、死代码、Router 接线） |

> 说明：本报告未改动任何源码。P0-3 涉及 `proto/kv.proto` 接口契约变更，
> 按 `CLAUDE.md §15` 需先与你核对后再实施。

---

## 6. 与既有记录的对照

`.workbuddy/memory/2026-11-18.md`（本日早间会话）已登记过两项与本文重合的缺陷：
锁 TTL 断链（本文 P0-3）与 `Peer::applied_` 未播种（本文 P0-7）。
本报告独立复核后**确认两项均成立**，并补充了先前未记录的 5 个 P0
（fsync 装配、TSO 重启、错误折叠、空 WAL 指针、RaftLog 非原子）与 15 个 P1。
其余早间结论（跨区写路由、CLI 悬空端点、会话 TTL 用 created 而非 last_active、split 元数据不持久）
本文未逐条复核，不在此重复判定 —— 建议以 `docs/bugs.md` 为唯一收口处，
按本文 §5 顺序逐条转成 BUG-NNNN 与回归用例。
