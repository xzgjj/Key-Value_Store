# M3 实现笔记（notes-m3）

> UX-06 交付物，对应 SPEC-2 §2.4.3 / §2.7.2 / 分支 `3`（本轮在分支 `2` 上追加，见 §5）。

## 1. 做了什么

| 项 | 实现 | 证据 |
|---|---|---|
| ReadIndex（必做） | leader 记 `read_index = committed` → 广播带 `read_ctx` 的心跳 → 收 quorum 回执 → 位点进 `Ready.confirmed_reads` → 等 `applied ≥ 位点` 再读 | `readindex_test` 5 例 |
| LeaseRead（默认关） | quorum 心跳回执刷新 `lease_deadline = now + 0.7×election_timeout`；窗口内可零 RPC 直读 | 同上（`LeaseDisabledByDefault` / `LeaseValidAfterQuorumHeartbeatThenExpires`） |
| Peer 两阶段读 | `begin_read_index()` → 调用方泵消息 → `take_confirmed_reads()` → `applied_at_least()` → `read_directly()` | `readindex_test` |

## 2. 为什么是两阶段而不是阻塞读

`Peer` 不持有传输层，自己无法泵消息。若把"等 quorum"写成阻塞，就必须在 Peer 内部起线程或
回调，那就把状态机的单线程前提破坏了。两阶段把"等待"留在驱动方（Worker / 测试泵），
Peer 只提供判据（`applied_at_least`）——这也是 SPEC-2 里 Worker drain 模型的原意。

## 3. 三条安全断言的取舍

- **确认位点必须 ≥ 发起时 commit**：`read_index` 取的是发起瞬间的 committed，不是 last_index。
  取 last_index 会让未提交的条目参与读语义，等于把脏数据当成已确认。
- **分区中的 leader 必须确认失败**：这条用 100% 丢包直接测。若这里能"确认"，线性一致就破了，
  而且症状极隐蔽（只在网络抖动时读到旧值）。
- **租约默认关**：D2-4 要求开启需声明时钟漂移假设。实现里租约用**单调 tick 计数**而不是墙钟，
  所以模拟可复现；真实部署接入墙钟时必须重算 drift 余量（`election_timeout ≥ 2×心跳 + drift`）。

## 4. 与 SPEC-2 §2.7.2 的差距（如实）

| 验收行 | 状态 |
|---|---|
| FUNC-05 ReadIndex | ✅ 协议级断言（quorum 确认 / 分区不可确认 / 非 leader 拒绝） |
| REL-01 线性一致历史检查 | ⚠️ **未做 `lincheck`**：需要历史记录器 + Wing-Gong 检查器。当前用协议不变量替代，覆盖弱于历史检查 |
| FUNC-06 / PERF-04 LeaseRead | ⚠️ 语义有用例（有效/过期/默认关），**`read_bench --mode=lease` 未做**，QPS ≥10× 未测 |
| PERF-03 读 ≥3×/5× | ⚠️ **未测**：`read_bench` 的 read_index/log 两模式对比未实现 |
| CLI `--read-mode` | ❌ 未接（集群模式仍只读 status） |

**结论：M3 的一致性协议已实现并验证，但"线性一致历史检查"与"读性能达标"两项证据物缺失，
按 SPEC-2 §2.7.2 口径记为部分完成。**

## 5. 分支说明

本轮改动落在分支 `2`（用户指令：M3 与 M2 缺口一并补齐后再交接）。下一阶段开分支 `3` 时，
M3 的剩余项（lincheck、read_bench、CLI read-mode、集群写提案回调）应在分支 `3` 内收口，
或与 M4 的多 region 一起做——集群写回调与 ReadIndex 共用"apply 到位点"的同一条等待链路。

## 6. 交接要点

- `Raft::read_index()` 的等待完全由调用方驱动；`Ready.confirmed_reads` 是一次性队列，取走即清。
- `Message.read_ctx` 是新增字段（11），纯增量：0 表示普通心跳，回执必须原样带回。
- 租约的 `now_ticks_` 在 `tick()` 里自增，因此**只有 tick 才会让租约老化**；驱动方漏 tick
  会让租约看起来永不过期——测试里专门用"只 tick 不投递"验证了过期路径。
