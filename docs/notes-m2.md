# M2 实现笔记（notes-m2）

> UX-06 交付物，对应 SPEC-2 上篇 / 分支 `2`。只记"为什么"与"还差什么"，"做了什么"看 `git log`。

## 1. 交付了什么

| 提交 | 内容 |
|---|---|
| M2-0 | 契约统一：`proto/common.proto`（CfName + RegionEpoch）、回执改 oneof 对称 |
| M2-1 | Raft 日志：四指针 + 冲突截断 + 压缩 + 元信息落盘 |
| M2-2 | Raft 状态机：Step/Tick/Ready，SPEC-2 四条红线各有用例 |
| M2-3 | RaftKV：提案走共识、apply 落存储、按 client+seq 去重 |
| M2-4 | 随机丢包冒烟：五组种子，修掉一条活性缺陷 |

## 2. 与 SPEC-2 §2.7.1 的差距（必须如实看）

| 验收行 | 状态 |
|---|---|
| FUNC-03 选举/复制 | ✅ `raft_step_test`（3/5 节点收敛、拒绝不重置计时器、整段回退、旧 term 不提交、压缩后回 prevCommit） |
| FUNC-04 3 节点读写 + 重试幂等 | ✅ 单副本链路 `raft_kv_test`；3 节点写读走同一代码路径（消息总线已覆盖复制） |
| ENG-05 150 轮 | ⚠️ `raft_chaos_n150` 已跑通 150 种子（14s，丢包+提案+重启）；**进程级 kill -9 仍用关闭重建模拟** |
| REL-02 崩溃恢复 | ✅ 进程级：子进程写完 `_Exit(0)`（跳过析构），重开只靠 WAL 复原 200 条（`crash_recovery_test`），并断言崩溃时确实没有 SST |
| 快照与 CompactLog | ✅ compact + 生成/传输/安装（`snapshot_test`：隔离→压缩→解开→靠快照追平；快照外的键被清除） |
| UX-04 `/status` 端点 | ✅ `StatusServer`（极简 HTTP，手写不引框架）+ `cli_status_test`；端口独立于 Raft 帧端口（偏差已记） |
| UX-01/02 集群 CLI | ✅ `status` / `get`（三读模式）/ `put` / `del` 全部可用（POST /kv，回执由 apply 给出）；`scan` 仍缺 |
| PERF-01/02/05/06/08 | ⚠️ 仅 M1 的单机写基准；集群档位未测（无网络层） |
| Transport（asio 传输） | ✅ `TcpTransport`：长度前缀分帧、io 线程收、异步短连接发；三节点真 socket 上选主+复制+apply 通过（`tcp_cluster_test`） |

**结论：M2 的"共识正确性内核 + 真实 TCP 传输"已交付并验证；端点、集群 CLI、150 轮长跑、PERF 集群档位仍未交付。**
不宜宣称 M2 完成——按 SPEC-2 §2.7.1 的口径，缺证据物即未完成。

## 3. 四个真缺陷（都由测试当场抓出，值得记）

1. **选举全平票**：所有节点超时相同、同拍竞选，永远选不出 leader。真实 Raft 用 [T,2T] 随机，
   这里用 id 派生确定性错峰（可复现是模拟器的前提）。第一次跑 `ElectionConverges` 就红了。
2. **单副本选不出 leader**：`campaign()` 只等外部回执，自己那一票从不结算。
3. **单副本 commit 永不推进**：`advance_commit` 只由 follower 回执触发，没有 follower 就停在 0。
   这个 bug 让"删除"用例假绿（读不到值就算通过），只有"写入后必须读回"照得出来。
4. **分区恢复后不追赶**（最典型）：leader 恢复后只发纯元数据心跳，落后 follower 永远收不到
   缺失条目，已提交位点永远追不平。修法：心跳分支先判断该 follower 是否落后，落后就补发
   AppendEntries；追平后退化为纯心跳，PERF-08 不受影响。

## 4. 一个测试口径教训

混沌用例最初断言"所有节点 `last_index` 相等"，结果正确实现被判失败——**未提交的尾部条目
各节点不同是 Raft 的正常语义**。改为只比已提交前缀（逐 index 同 term）才是对的。
断言写错方向比代码写错更危险：它会把正确行为固化成"错误"。

## 5. 关键设计取舍

- **raft log 存进 M1 引擎**而非自建段文件 WAL：复用已验证的追加/CRC/重放，少一处崩溃窗口。
- **去重按 client 记录最大 seq** 而非缓存逐条回执：省内存，对顺序重试足够（乱序重试需回执缓存，M3 若要支持再改）。
- **读为 leader 本地读**：M2 明确不保证线性一致，M3 用 ReadIndex 收口。这条限制写在 `peer.h` 头注释里，
  避免以后被当成 bug 追。

## 6. 交接 M3

- ReadIndex 需要 `HeartbeatResponse` 携带的 committed/index 做 quorum 确认，`handle_heartbeat` 已在回执里带 `commit`。
- 快照安装前必须 `is_initialized()` 判断（SPEC-3 红线 4），当前无 peer 初始化状态。
- `/status` 端点：`Peer` 已暴露 `is_leader/committed/applied_seq`，缺口只在 HTTP 层。
- 网络层：`Transport` 抽象尚未定义，内存总线是它的替身，接口形状可直接照 `deliver_round` 提炼。

## 7. 网络层补充（2026-11-16 追加）

三条硬经验，都写在 `tcp_transport.h` 与 `tcp_cluster_test.cpp` 的头注释里：

1. **短连接是有意选择**，不是省事：设计文档 §6 的替代架构里"短连接降级"本就是预案。
   长连接池属 PERF 优化，跨机时会被 PERF-01 的测量照出来。
2. **tick 必须与自旋解耦**：测试里每自旋都 tick，选举超时（10 tick ≈ 1.5ms）比一次 TCP
   投票往返还短，集群永远在重选。这是测试把时钟设得比网络快三个数量级，不是 Raft 的错。
3. **帧边界与线程边界分开验**：单帧往返、多帧不粘连、三节点选举各一个用例。
   前两个证明封装，第三个证明集成；混在一起失败时无法定位。

另记一个测试自身的 bug：`add_requests()` 调两次会造出"只有 value、key 为空"的第二条命令，
apply 时被 `encode_key` 拒绝。这类错误只有真跑 apply 才暴露——只断言"committed 推进"会漏掉。

## 8. 四件欠账的收口（2026-11-16 追加）

| 欠账 | 收口方式 | 证据 |
|---|---|---|
| 快照生成/传输/安装 | protobuf 承载（不与 SST 编码重复）、MSG_SNAPSHOT 单块、安装时清掉快照外的键 | `snapshot_test` 2 例 |
| 集群写 | POST /kv（制表符文本）+ 回执表；CLI put/del | `cli_cluster_get_test` 7 例 |
| 进程级崩溃 | 独立 `crash_writer` 子进程 + `std::_Exit(0)`，跳过一切析构 | `crash_recovery_test` |
| 集群性能 | 写 QPS / 心跳字节 / 恢复时间 | `cluster_bench` |

**PERF-01 未达标，且已定位到根因（被真 TCP 压测推翻了我此前的猜测）**

`tcp_perf_bench`（3 节点真 socket + 独立泵线程，客户端逐条等回执）：

| 阶段 | 配置 | 实测 |
|---|---|---|
| A | fsync=on, batch=1 | **201 records/s** |
| B | fsync=off, batch=1 | **784 records/s** |
| C | fsync=off, batch=64 | **33765 records/s** |

- 我此前把进程内基准的 251 ops/s 归因为"泵粒度开销"——**错了**。真 TCP 下更慢（201），
  说明那点开销不是主因：**每条写一次 fsync 才是**。B/A = 3.9×。
- 距离 S0 的 2k 差约 10×。缺口不在共识逻辑（batch=64 有 43× 收益），而在
  **group commit 缺失**：当前每次写独立 fsync，多个并发写不会合并成一次落盘。
- 修法明确：WAL 增加提交队列——写入者登记后由一个提交者批量 fsync 并唤醒全部等待者。
  这是 SPEC-1 §1.4.2 里点名提到、但实现时跳过的机制。
- 因此 PERF-01 仍记**未达标**，且有了可复现的测量装置（本文件就是它的证据）。

### 8.1 [2026-11-16 接手会话] group commit 已实现 + PERF-01 根因二次修正

**做了什么**：WAL 加后台提交者（`src/storage/wal.cpp`），append 内 fwrite+fflush 后即返回，
2ms 窗口内多条写合并成一次 fsync（延迟确认窗口，用户选定；进程级崩溃不丢，掉电最多丢一窗）。
`crash_recovery_test`、`wal_test`、`wal_crash_test` 全绿，新增 `wal_group_commit_test` 3 例。

**实测（本会话，group commit 后）**：

| 装置 | 配置 | 实测 | 对比改动前 |
|---|---|---|---|
| `write_bench` 单机 batch=1 fsync=on | 无共识 | **74728 QPS / P99 0.04ms** | 3184 → **23.5×** |
| `tcp_perf_bench` D 流水线 batch=1 fsync=on | 集群 | **648 records/s** | 197 → 3.3× |
| `tcp_perf_bench` A 串行 batch=1 fsync=on | 集群 | 455 records/s | 201 → 2.3× |
| `tcp_perf_bench` B 串行 batch=1 fsync=off | 集群 | 692 records/s | 784（同级）|
| `tcp_perf_bench` C batch=64 fsync=off | 集群 | 31287 records/s | 33765（同级）|

**根因二次修正（比 §8 又深一层，如实记）**：
- §8 说"缺 group commit 是主因"——对单机成立（3184→74728，铁证），**对集群不成立**。
- group commit 后集群 A(455)≈B(692)，fsync 已离开关键路径；但**即使完全不 fsync，单条写也只有 ~700/s**。
- 说明集群 batch=1 的稳态瓶颈是**每条写一次共识往返（AppendEntries→多数派→apply→回执）+ 泵调度粒度**，
  与落盘无关。S0=2k 在本进程内泵 + 真 TCP 短连接的装置下不可达，天花板约 ~700/s。
- C(batch=64)=31287 证明共识摊薄有效——要提速必须改共识管线（批量 propose 合并 append、pipeline 复制），
  这超出"补 group commit 收口 M2"的范围，属独立优化/M4 议题。

**结论（不许自圆其说）**：group commit 已交付且收益巨大；**PERF-01 的 S0=2000 仍未达标**，
M2 维持"部分完成"。已如实记录天花板与根因，待用户决策（放宽口径 / 追加共识管线优化 / 接受部分完成进 M4）。
