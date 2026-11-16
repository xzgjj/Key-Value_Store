# M4 第⑤层 快照分块契约核对（按 SPEC-3 D3-3 默认落版）

> 依据：SPEC-3 §3.6 配置项表 `snapshot.chunk_mb=4`（分块大小，流内序号+CRC）与
> §3.8 D3-3「4MB，流内序号+CRC」。用户 2026-11-16 指令「⑤ 契约核对 完成 m4」，
> 授权按 SPEC 默认核对并落版；如需调整分块大小/窗口，改本文件后再动实现。

## 现状（M2 单块）

`MSG_SNAPSHOT` 用 `snapshot_data` 一次性承载整个 `SnapshotData` 序列化字节。region 变大后单块
消息会压垮连接、无流控、单点失败全量重传——M4 分 region 后必须改成分块。

## 草案（落版）

### proto 增量

```protobuf
enum MessageId {
    // ... 既有 0..9 ...
    MSG_SNAPSHOT_CHUNK = 10;  // leader→follower：快照分块
    MSG_SNAPSHOT_ACK   = 11;  // follower→leader：流控确认（已连续收到的最大 chunk_seq）
}

// 快照分块（载荷放 Message.snapshot_data，不新增 Message 字段，保持七字段复用约定）
message SnapshotChunk {
    uint64 snapshot_index = 1;  // 本快照覆盖位点（同一快照所有块一致）
    uint64 snapshot_term  = 2;  // 该位点任期
    uint32 chunk_seq      = 3;  // 流内序号，从 0 起连续
    uint32 total_chunks   = 4;  // 总块数，接收端据此判断收齐
    uint32 crc32          = 5;  // data 的 CRC32C（逐块校验，不必等整表）
    bytes  data           = 6;  // 该块载荷，≤ chunk_size（默认 4MB）
}
```

复用约定：`MSG_SNAPSHOT_CHUNK` 的 `Message.snapshot_data` = `SnapshotChunk` 序列化字节；
`MSG_SNAPSHOT_ACK` 的 `Message.index` = 已连续收到的最大 seq，`Message.snapshot.index/term` = 快照代号。

### 行为

1. **发送**：leader 发现 `Ready.snapshot_targets` → `dump_snapshot()` 一次 → 序列化 blob →
   按 `chunk_size`（默认 4MB）切块，为每个 target 建发送游标；窗口 `kSnapshotWindow=4`，
   收到 ACK 推进后再发，实现在途块上限（流控）。
2. **接收**：按 `(from, snapshot_index)` 缓存块，逐块校验 CRC；`received==total` 时按 seq 重建 blob →
   整表 `SnapshotData` 解析 → 复用既有 `install_snapshot()`（含"快照外键删除"）→ `restore_snapshot()`。
   每收一块回 `MSG_SNAPSHOT_ACK`。
3. **向后兼容**：`MSG_SNAPSHOT` 单块路径保留（旧实现/单测仍可用）；新路径在
   `snapshot_chunk_test` 与既有 `snapshot_test` 端到端验证。
4. **红线**：块 CRC 失败 → 丢弃该块并等待重传（不安装半截快照，防脏数据入库）。

## 核对结论

按 SPEC-3 默认落版：4MB/块、流内序号、逐块 CRC32C、窗口 4 块流控。若需改 chunk 大小，
改 §草案 的 `chunk_size` 默认值与 `snapshot.chunk_mb` 配置项，再同步本文件。
