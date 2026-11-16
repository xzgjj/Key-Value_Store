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

## 4. 验收对照（SPEC-2 §2.7.2）

| 验收行 | 状态 | 证据 |
|---|---|---|
| FUNC-05 ReadIndex | ✅ | `readindex_test` 5 例（quorum 确认 / 分区不可确认 / 非 leader 拒绝 / 默认关 / 过期） |
| REL-01 线性一致 | ✅ 读语义不变量 | `lincheck_readindex`：读己之写 + 单调读，两种模式各一遍。**口径说明**：不是 Wing-Gong 全序搜索，检查的是读被违反时最典型的两条不变量 |
| FUNC-06 LeaseRead | ✅ | 同上 + `lincheck_leaseread`（租约模式跑同一组不变量） |
| PERF-03 读 ≥3×/5× | ✅ **实测** | `read_bench`：read_index 19201 ops/s vs log 327 ops/s = **58.7×**（S0 要求 3×、S1 要求 5×） |
| PERF-04 读 ≥10× | ✅ **实测** | lease 1195219 ops/s vs log 327 = **3652.8×**（要求 10×） |
| CLI `--read-mode` | ✅ | `cli_cluster_get_test`：read_index / lease / log 三模式端到端 + 缺 key 退出码 3 + 非法模式退出码 1 |

**结论：M3 的 SPEC-2 §2.7.2 各行均有证据物。唯一口径差异是 lincheck 用"读语义不变量"
而非全序历史搜索，已在表内注明——不把它说成完整 lincheck。**

## 4.1 基准数字的意义与边界

- 三个模式**都是线性一致读**，差别只在代价：log 走一遍共识（追加+提交+apply 往返），
  read_index 只走一轮心跳 quorum，lease 在窗口内零 RPC。
- 倍率来自**进程内总线**，不含真实网络往返；真机跨网络的 log 模式会更慢，倍率只会更大，
  不会更小——所以这个对比对"ReadIndex 值得做"的结论是稳健的。
- 绝对值不可跨机器比较，`read_bench` 的断言只留"ReadIndex 不慢于走共识"这条底线，
  3×/5×/10× 由人工按本表读数判定。

## 5. 分支说明

M3 全部改动落在分支 `2`（用户指令：M2 缺口与 M3 一并补齐后再交接）。
M4 开分支 `3` 时，集群写提案回调（`Peer::take_reply`）与 `/kv` 端点可直接复用。

## 6. 顺带修掉的一个真 bug

`Peer::take_messages()` 里残留了一次 `take_ready()` 调用，会顺手清掉待 apply 队列与已确认读位点。
当调用方顺序是"先取消息再 on_ready"（泵的常见写法）时，数据直接丢——读基准的写前置就是这么挂的。
现在只取 outbox，注释写明为什么不能再调 take_ready。

## 7. 交接要点

- `Raft::read_index()` 的等待完全由调用方驱动；`Ready.confirmed_reads` 是一次性队列，取走即清。
- `Message.read_ctx` 是新增字段（11），纯增量：0 表示普通心跳，回执必须原样带回。
- 租约的 `now_ticks_` 在 `tick()` 里自增，因此**只有 tick 才会让租约老化**；驱动方漏 tick
  会让租约看起来永不过期——测试里专门用"只 tick 不投递"验证了过期路径。
