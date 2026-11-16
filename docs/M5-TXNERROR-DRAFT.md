# M5 第①层契约核对草稿：事务错误契约（SPEC-4 §4.3.3）

> 状态：按 SPEC-4 错误矩阵落版，留痕以备用户追认。
> kv.proto 的事务消息骨架（TxnGet/Prewrite/Commit/BatchRollback/CheckTxnStatus）
> 为 M0 已核对契约，本层零改动；本层只扩展错误通道。

## 1. KvError 扩展（6 → 9 变体）

| 变体 | 触发 | proto 对应 |
|---|---|---|
| WriteConflict | prewrite 时最新 Write.commitTs > start_ts | RegionError.WriteConflict（oneof=4） |
| LockConflict | prewrite/读遇他事务 Lock | RegionError.LockConflict（oneof=5） |
| Retryable | 决议后锁已清可安全重试 | RegionError.Retryable（oneof=6） |

- 三变体与 proto RegionError 新消息**对称**（不进 contract_exempt 豁免清单）。
- 豁免清单保持 IoFailed/Corrupted/InvalidArgument 三条不变。

## 2. TsNotMonotonic 走 fatal 异常（不进 KvError）

SPEC-4 §4.3.3 明示"fatal 路径：拒绝服务并报告（异常分层，CLAUDE.md §7-1）"。
实现为 `tinykv::TsNotMonotonic : std::runtime_error`（携带 got/last），在 TSO 层检测到
非严格递增时抛出——不变量破坏不进 result 错误通道。

## 3. 待追认项

1. 三变体的 proto 字段集是否沿用（key/start_ts/conflict_commit_ts 等，语义见 SPEC-4 表）。
2. TsNotMonotonic 是否需要同时进入 result 通道供远程回执携带（当前设计：进程内 fatal；
   跨进程场景随 daemon 化评估，RegionError 可后续扩展）。
