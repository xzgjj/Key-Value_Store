# M4 实现笔记（notes-m4）

> UX-06 交付物，对应 SPEC-3 §3.7 / 分支 `3`（M4 全层：①批量提案 → ②Region 模型 → ③Split →
> ④成员变更（④a 状态机 + ④b 追平门槛）→ ⑤快照分块 → ⑥PD-lite 调度器 → ⑦集成
> （⑦a TransferLeader + ⑦b CLI 观测 + ⑦c mr_chaos_n150））。

## 1. 做了什么（按层，一行一句 + 证据）

| 层 | 实现 | 证据 |
|---|---|---|
| ① | Envelope 批量 propose：攒批窗口内多请求一次共识往返 | PERF-01 管道化 batch=1 28433/s（44×），见 notes.txt 决策链 |
| ② | Region/StoreMeta/Router：按 end_key 区间索引 + epoch 校验 | `region_route_test` 11 例 |
| ③ | Split admin：提案校验与 apply 校验分离，apply 后立即更新路由（红线1/2） | `split_test` 3 例 |
| ④a | 单步成员变更：Entry.type + AddLearner/AddVoter/RemoveNode apply 时生效 | `confchange_test` 5 例 |
| ④b | Learner 追平门槛（差<100 且持续5s，tick 计量）+ 两节点移除 leader 守卫（红线5） | `sim_confchange_test` 3 例 |
| ⑤ | 快照分块：4MB/块 + 流内序号 + 逐块 CRC + 窗口流控（D3-3），保留单块兼容 | `snapshot_chunk_test` 2 例 |
| ⑥ | PD-lite 调度器：心跳视图 + size 均衡建议（限速1/轮）+ AskSplit 预分配 | `sim_scheduler_test` 4 例 |
| ⑦a | TransferLeader：补日志→TimeoutNow→竞选；转移期拒提案；一个 election timeout 放弃（红线6） | `transfer_leader_test` 4 + `sim_transfer_test` 3 |
| ⑦b | CLI 观测 6 命令 + StatusServer AdminHandler + RegionTrace | `cli_region_test` 6 例 |
| ⑦c | mr_chaos_n150：分区+提案+split+confchange 叠加，150 轮收敛 | `mr_chaos_n150`（150/150 种子） |

## 2. 三条贯穿 M4 的设计主线

- **apply 时才生效**：ConfChange 与 Split 都是共识条目，所有副本各自 apply 各自视图——
  这让"三副本 region 指纹一致"成为 split 经共识到达全部副本的直接证据（⑦c 断言）。
- **learner 不计多数派**：④b 补复制链路时把 learner 回执放宽到与 voter 相同，但
  `advance_commit()` 仍只由 voter 回执驱动——这是防少数派提交的关键安全点。
- **控制面只动元数据**：⑥层调度器只产出 MovePeer 建议，迁移执行复用④层
  AddLearner→AddVoter→RemoveNode；快照数据走⑤层独立通道（Mooncake M-1 边界）。

## 3. 踩坑记录（P3）

1. **空 store 被回填成有量**（⑥层）：proto3 下 `total_size=0` 与"未上报"不可区分，
   最初用 region 大小回填 store 汇总做兜底，结果新建空 store 被算成 900 字节、size 差消失、
   均衡永不触发。教训：**心跳是唯一事实来源，0 是合法值**。
2. **AskSplit 从 0 起算撞号**（⑥层 review 发现）：`next_region_id_` 不感知已见 region，
   心跳里见过 region 5 时分配出 id=1。修复：心跳到达即抬升基线；peer id 同法
   （硬编码 `next_peer_id_{1000}` 只是躲撞号不是消除撞号）。
3. **begin_snapshot_send 每轮重置游标**（⑤层）：分块发送状态被 on_ready 重建，
   窗口游标回 0，后续块永不发出。修复：已有任务不重建。
4. **TimeoutNow 已入 outbox 就会送达**（⑦a）：测试假设"不 flush 目标就收不到"是错的；
   模拟到目标的分区必须丢弃 leader 的 ready。
5. **红线 4 的 commit 语义**（⑦a）：新 leader（term+1）必须先提交一条自己任期的条目，
   旧条目才能随之重新提交——"不丢提案"的断言要写在这个语义之后，而不是转移 flush 之后。
6. **三副本共享 StoreMeta 是陷阱**（⑦c）：真实拓扑各 store 各持视图；共享会让后 apply 的
   副本撞"region 不存在"且 apply 错误不重试（take_ready 已推进位点），视图永久滞后。

## 4. 验收对照（SPEC-3 §3.7 逐行）

| 需求 | 测试登记名（实际） | 状态 | 说明 |
|---|---|---|---|
| FUNC-07/REL-05 | `region_route_test`（SPEC 名 sim_region_route_test） | ✅ | 11 例；REL-05 另经 `mr_chaos_n150` 150 轮混沌重验 |
| FUNC-08 | `split_test`（SPEC 名 sim_split_test，多种子） | ⚠️ 部分 | 语义 3 例全过 + ⑦c 150 轮 split 注入；**1M key 规模证据挂 PERF-07 bench（未建）** |
| FUNC-09/REL-04 | `sim_confchange_test` | ✅ | 3 例（未追平拒/两节点移除 leader 拒/三节点允许） |
| FUNC-10 | `sim_transfer_test` | ✅ | 3 例（不丢提案/拒绝可重试/放弃后继续服务）+ 单测 4 例 |
| FUNC-11/REL-03 | `snapshot_test` + `snapshot_chunk_test`（SPEC 名 sim_snapshot_test） | ✅ | 单块兼容 + 分块/CRC/窗口全路径 |
| FUNC-12 | `sim_scheduler_test` | ✅ | 4 例（汇总/AskSplit 不撞号/三轮单调收敛/限速） |
| ENG-05 | `mr_chaos_n150` | ✅ | 150/150 种子；证据 `build/evidence/nightly/mr_chaos_150.txt`（构建产物） |
| PERF-07 | `split_bench` | ✅ 实测 | 1M key + 4 次 split = **6.3s**（159k keys/s），S0 ≤60s 余量 ~10× |
| PERF-08 | `heartbeat_bandwidth_bench` | ✅ 实测 | 心跳体积与日志长度无关（10条 24B vs 1000条 28B，varint 增量）；外推 1000 regions 仅 547 KB/s |
| PERF-09 | `snapshot_bench` | ✅ 实测 | 13.7MB 快照流 = **11 MB/s**（S0 ≥10 达标） |
| UX-04 | `docs/notes-m4-troubleshoot.md` | ✅ | 注入"split 假成功"缺陷，仅凭 CLI 三点交叉定位（见演练记录） |
| UX-06 | 本文件 | ✅ | — |

**结论**：语义/机制各行全部有证据物；三个 PERF bench 未建（S0 规模证据缺失），已如实挂账。
SPEC 建议名与实际登记名的差异均为"sim_ 前缀/scale 后缀"级——登记名是唯一事实来源（testing.md §4），
映射已在本表注明。

## 5. 挂账（需要用户决策）

- **trace 埋点**：开关+落盘通道已交付，Peer 内按 region 粒度的事件埋点（apply/慢日志/
  leader 变更）待 daemon 化时接入（见 docs/M4-CLI-TRACE-DRAFT.md §4）。
- **契约待追认**（4 份草稿）：M4-TRANSFERLEADER-DRAFT（枚举编号 12、放弃是否回执）、
  M4-CLI-TRACE-DRAFT（trace 补 --endpoint、split 回执格式）。

## 6. 数字

- 测试：ctest 45/45（unit 27 / simulation 4 / integration 6 / bench 7 + cli smoke）；
  M4 期间新增登记 9 个测试文件、17 个用例文件级登记。
- review：M4 共 30 轮（每子层 3 轮），全部 PASS；align 3/3 每层复核。
- CI：13 项门禁每层全绿；CI 期间修复的超限：复杂度 3 处（⑥ serve 26、⑦b CLI 23/19、⑦c 26）、
  include 重复窗口 1 处。
- 提交（分支 3，本地，未 push）：966d8ad ④a → a440ec0 ④b → 41d8c5c ⑤ → cf435ae ⑥ →
  98ee0d2 ⑦a → b81ebcf ⑦b → 1a0529a ⑦c →（⑦d 本文档随提交）。
