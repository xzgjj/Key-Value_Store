# M4 第⑦层契约核对草稿：TransferLeader / TimeoutNow

> 状态：按 SPEC-3 红线 6 与既有提交路径落版，留痕以备用户追认。
> 用户 2026-11-16 已定「TransferLeader 拆到第⑦层」，本文件记录该层实际落地的契约面。

## 1. 已核对部分（不新增契约）

`TransferLeaderRequest` 在 `proto/raft_cmdpb.proto` 早已定版（用户 2026-11-16 核对通过）：

```proto
message TransferLeaderRequest {
    tinykv.raftpb.Peer peer = 1;      // 目标新 leader
}
message AdminRequest {
    oneof cmd {
        SplitRequest split = 1;
        CompactLogRequest compact_log = 2;
        TransferLeaderRequest transfer_leader = 3;
    }
}
```

本层**不修改**该消息：`peer.id` 即目标 peer，语义"把 leadership 交给该 peer"。

## 2. 本层新增：一个枚举值（追加式，不改既有编号）

```proto
enum MessageId {
    ...
    MSG_SNAPSHOT_ACK = 11;
    MSG_TIMEOUT_NOW = 12;            // leader→target：立即竞选
}
```

- **为什么不复用 MSG_HUP**：`MSG_HUP` 的语义是"本地选举超时"，属节点内部驱动，不携带发送方；
  `MSG_TIMEOUT_NOW` 是**跨节点显式指令**。复用会让"谁让我竞选"这一信息丢失，也让
  日志/追踪里无法区分自发选举与转移触发选举。
- **为什么只加枚举不加字段**：TimeoutNow 只需 `term/from/to`，三者都是 `Message` 既有字段，
  符合 SPEC-0 §0.3.2「七字段复用，禁止为单类消息私加持久字段」。
- **兼容性**：只追加编号 12，不改既有编号与字段 → 旧节点收到未知枚举值会落 `default` 分支
  返回 `InvalidArgument`（已存在的兜底），不会静默误处理。

## 3. 语义契约（红线 6 的三条，逐条对应实现）

| 红线 6 要求 | 实现 | 证据 |
|---|---|---|
| 转移期拒绝新提案 | `propose` / `propose_conf_change` 在 `transfer_target_` 有值时返回 `InvalidArgument` | `transfer_leader_test` 第 2 例 |
| 一个 election timeout 未完成即放弃 | `transfer_deadline_ = now_ticks_ + election_timeout`，`tick()` leader 分支到点清除 | `transfer_leader_test` 第 3 例 |
| 目标须先追平才发 TimeoutNow | `match_index_ < last_index` 时不发，等 `handle_append_response` 追平后再发 | 第 1 例（先写多条再转移） |

## 4. 不落日志的理由

TransferLeader **不写日志**：它是可放弃的临时意图（一个 election timeout 内没成即作废），
落日志会让"放弃"变成一条需要复制的记录，且崩溃后重放会重放一个已经过期的意图。
这与 etcd 的处理一致（`leadTransferee` 是易失字段）。

## 5. 待用户追认项

1. `MSG_TIMEOUT_NOW = 12` 这一枚举值编号是否沿用（若后续要与其他实现对齐可换号，语义不变）。
2. 转移失败后是否需要在回执里显式告知客户端"已放弃"——当前设计是**静默放弃 + 日志**，
   CLI 侧通过 `region <id>` 的 leader 字段观察，不新增回执字段。
