# M4 第④层 成员变更契约核对（✅ 2026-11-16 核对通过）

> ✅ 用户 2026-11-16 确认：Entry 加 type 字段、ConfChange 载荷放 raft.proto、
> Learner 追平门槛照 SPEC 默认（差<100 且持续 5s）、TransferLeader 拆到第⑦层。
> 第④层是 SPEC-3 最高风险区（白皮书作者耗 3 周），动 raft 核心状态机，逐条 TDD 后推进。

## 现状

- `raft.proto` 的 `Entry{index, term, data}` **没有 type 字段**；
- `Raft::cfg_.voters` 构造时固定，`majority()/is_voter()` 基于它；
- 全项目无任何 ConfChange/learner/TransferLeader 实现（只有 `Region::PeerInfo.is_learner` 字段占位）；
- 第①层的 Envelope 批处理在 `Peer` 层（data 内编码），raft 状态机不感知 Entry 内容。

## 为什么必须改 proto

单步成员变更要求：leader 提交一条 ConfChange 日志，**apply 时**才把它应用到 membership
（保证"先持久化再变更"，否则 leader 崩溃后 membership 无法从日志重建）。
raft 状态机必须能区分"普通日志（Envelope/客户端命令）"与"成员变更日志"——
最干净的方式是 `Entry` 带 type 字段。用 data 内编码（类似 Envelope）会让状态机解析应用层数据，
破坏分层，且快照/重放都要特殊处理。

## 草案 A：`raft.proto` 的 Entry 增加类型字段

```protobuf
enum EntryType {
    EntryNormal = 0;       // 默认值：普通日志（客户端命令 / Envelope 批处理）
    EntryConfChange = 1;   // 单步成员变更
}
message Entry {
    uint64 index = 1;
    uint64 term = 2;
    bytes data = 3;
    EntryType type = 4;    // 新增；默认 EntryNormal 保证向后兼容旧日志/快照
}
```

- 向后兼容：type 默认 0，既有代码/日志/快照无需改即视为 Normal（需你确认快照 proto 是否要带此字段）。

## 草案 B：ConfChange 载荷（新增 confchange.proto 或放 raft.proto）

```protobuf
enum ConfChangeType {
    AddLearner = 0;
    AddVoter = 1;
    RemoveNode = 2;
}
message ConfChange {
    ConfChangeType type = 1;
    uint64 node_id = 2;    // PeerId
}
// Entry.data（当 type=EntryConfChange 时）= ConfChange 序列化字节
```

## 草案 C：raft 状态机行为

- `Raft::propose_conf_change(ConfChange)`：仅 leader 接受，追加一条 `EntryConfChange` 日志并复制；
- apply 阶段（`collect_apply` → Ready.to_apply）暴露 entry.type，由 Peer/Store 层在 apply 时调
  `ApplyConfChange`（更新 `cfg_.voters` + learner 集合 + next/match 索引）；
- Learner：不参与选举、不计入 majority、但接收日志复制（追平后可 AddVoter）；
- TransferLeader：`TimeoutNow` 机制，转移期间拒绝新提案，一个 election timeout 未完成则放弃（红线6）。

## 核对结论（2026-11-16）

1. **C-1**：✅ 批准 Entry 加 `type` 字段（草案 A）。
2. **C-2**：✅ ConfChange 载荷放 `raft.proto`（紧耦合 Entry.type）。
3. **C-3**：✅ Learner 追平门槛照 SPEC 默认（日志差 <100 entry 且持续 5s）。
4. **C-4**：✅ TransferLeader 拆到第⑦层；本层只做 AddLearner/AddVoter/RemoveNode。

**落地**：Entry.type + ConfChange 消息写入 raft.proto → `src/raft/` 状态机 + `src/raftstore/` 按 TDD 实现。
