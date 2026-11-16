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
| REL-02 崩溃恢复 | ✅ 落盘可读回 + `raft_chaos_n150` 的 CommittedDataSurvivesRestart；**进程级 kill -9 未做** |
| 快照与 CompactLog | ⚠️ `RaftLog::compact` 已实现并有测试；**快照生成/分块传输/安装未做** |
| UX-04 `/status` 端点 | ✅ `StatusServer`（极简 HTTP，手写不引框架）+ `cli_status_test`；端口独立于 Raft 帧端口（偏差已记） |
| UX-01/02 集群 CLI | ⚠️ 集群模式**只读**：`--endpoint ... status` 可用；`put/del/scan` 明确拒绝（需提案回调，与 M3 ReadIndex 同链路一起做） |
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
