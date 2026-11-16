# M4 接口契约核对（✅ 已于 2026-11-16 核对通过）

> ✅ **契约已由用户逐条确认**（2026-11-16）：调度器仅消息+进程内对象、scan 用 next-key 游标、
> CLI 新增 6 条观测命令。按 CLAUDE.md §15，本节内容为准，可落入正式 `proto/scheduler.proto`
> 与 `docs/cli.md`；改动正式契约文件须同步 diff.md 留痕。本文件保留作为核对证据（不删除）。

## 背景

M4 第①层（Envelope 批量 propose）已完成，不改任何契约、已提交（95e90dd）。
第②层起要建 Region 模型 + Router + 调度器 PD-lite，引入**新 proto 契约**（调度器）与**CLI 观测命令**。
现有契约已就位、本草案**复用而不改动**：
- `common.proto` 的 `RegionEpoch{version, conf_ver}` —— 直接复用
- `raft_cmdpb.proto` 的 `RegionError{KeyNotInRegion|StaleEpoch|NotLeader}` —— 直接复用
- `ids.h` 的 `RegionId` / `PeerId` 强类型 —— 直接复用

---

## 草案 A：`scheduler.proto`（新增文件，PD-lite 调度器契约）

SPEC-3 §3.3.2 给出的骨架。设计原则（Mooncake 控制面/数据面分离）：调度器**只动元数据**，
快照等数据走独立通道，不经调度器转发。

```protobuf
syntax = "proto3";
package tinykv.schedulerpb;

import "common.proto";
import "raft.proto";

// 存储节点心跳：容量与负载
message StoreHeartbeatReq {
    uint64 store_id = 1;
    uint32 region_count = 2;
    uint64 total_size = 3;   // 字节
    uint64 capacity = 4;     // 字节
}
message RegionHeartbeatReq {
    uint64 region_id = 1;
    tinykv.commonpb.RegionEpoch epoch = 2;
    repeated tinykv.raftpb.Peer peers = 3;
    bytes start_key = 4;
    bytes end_key = 5;
    uint64 approximate_size = 6;
}
message Ack {
    bool accepted = 1;
    string reason = 2;       // 拒绝原因（可空）
}

// AskSplit：store 请求调度器为新 region 预分配 id
message AskSplitReq {
    uint64 region_id = 1;
    bytes split_key = 2;
}
message AskSplitResp {
    uint64 new_region_id = 1;
    repeated uint64 new_peer_ids = 2;   // 与新 region 副本数等长
}

// ✅ 用户确认 A1：proto 只定义消息，不生成 service stub；
//    调度器先做进程内 C++ 对象（scheduler.h），不引 gRPC、不开新 RPC 通道。
//    SPEC-3 的 service Scheduler 定义暂不落地为 RPC，留待后续阶段如确需跨进程再评估。
```

**核对结论（2026-11-16）**：
- A-1：✅ 仅消息 + 进程内对象（不生成 service stub，不开新 RPC 通道）。
- A-2：✅ `AskSplitResp` 先按一次分配一个 region（merge 需要时再扩展，留版本号兼容）。
- A-3：✅ 字段名/编号按本草案，无调整。

---

## 草案 B：CLI 新增观测命令（`docs/cli.md` 追加，不改现有语义）

UX-04 要求 region 维度可观测。现有 CLI 命令（put/get/del/scan/status）保持不变。

| 命令 | 作用 | 输出 |
|---|---|---|
| `tinykv-cli --endpoint <host:port> regions` | 列出该 store 的全部 region | 表格：region_id / range / leader / term / commit / apply_lag / size_hint |
| `tinykv-cli --endpoint <host:port> region <id>` | 单个 region 详情 | 同上 + peers 列表 + epoch |
| `tinykv-cli --endpoint <host:port> region peers <id>` | 副本分布 | 每 peer：store_id / 角色 voter\|learner / 是否 leader |
| `tinykv-cli --endpoint <host:port> region split <id> <key>` | 手动触发 split（调试用，走 admin 提案） | 成功/StaleEpoch/NotLeader |
| `tinykv-cli --endpoint <host:port> scheduler status` | 调度器状态 | 节点数 / region 数 / 待迁移队列 / 最近调度动作 |
| `tinykv-cli trace region <id> on\|off` | region 级 trace 开关 | 成功/失败 |

**新增退出码**：无（沿用现有 0/1/2/3）。

**核对结论（2026-11-16）**：
- B-1：✅ 用户确认增加 `region peers <id>`、`region split <id> <key>`、`scheduler status` 三条；
  `region leader <id>` 用户**不要**，不实现。
- B-2：✅ trace 日志落 `log/` 目录的 region 粒度子文件（CLAUDE.md §4 硬约束）。

---

## 草案 C：cluster `scan` 开放（C 阶段遗留，与 M4 region 边界一起做）

现有 `docs/cli.md` §7 明确"集群 scan 未开放，留到 M4 多 region 一起做"。开放需要：
- 请求带 `start_key` + `limit` + 可选 region 边界；
- 跨 region 时客户端按路由逐段拉取（分页游标语义）。

**核对结论（2026-11-16）**：
- C-1：✅ 用户确认 next-key 游标（对 split 友好）。
- C-2：✅ 不保证跨 region 原子快照，逐 region 各自一致（与 TiKV 一致）。

---

## 核对状态（全部完成 2026-11-16）

- [x] 草案 A（scheduler.proto）：A-1 仅消息+进程内对象 / A-2 单 region 分配 / A-3 字段名编号照草案
- [x] 草案 B（CLI 观测命令）：B-1 增 3 条命令（不要 region leader）/ B-2 trace 落 log/
- [x] 草案 C（cluster scan）：C-1 next-key 游标 / C-2 逐 region 各自一致

**落地清单**：把 A 落入 `proto/scheduler.proto`（仅消息），B 落入 `docs/cli.md`（新增 6 条命令），
C 的 scan 语义在 M4 第③/⑦层实现；本文件保留为核对证据。
