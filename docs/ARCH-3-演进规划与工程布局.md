# ARCH-3 演进规划与工程布局

> 性质：规划文档。任务编号沿用 SPEC-6 的 ⑦–⑩ 与 ⑨a/⑨c，新增项用 `T-xx` 避免与既有编号冲突。
> 排序原则：**先堵静默写错，再做能力扩张**。一份计划最大的风险不是漏做，是把顺序做反。

## 1. 判断基线

从 ARCH-1 的结论继承三条事实：

1. 组件层可用（Raft/MVCC/存储各自有测试背书），**装配层有几根线没插**（epoch 未校验、调度器无入口、CLI 三端点悬空）。
2. 两条 P0 是"静默写错"而非"功能缺失"：锁 TTL 时间基准丢失、跨区事务写错分区。
   静默错误的代价随时间指数增长——一旦有真实数据，修复就得配迁移。
3. 并发上限来自一把闸，但**现在拆锁是错的**：协议 bug 与并发 bug 纠缠后无法归因。

由此得出排序：正确性封口 → 真相落盘 → 能力扩张 → 并发与边界。

## 2. 五条波次

```
W1 正确性封口（0.5–1.5 天）
   IF-01 TTL 基准 · IF-04 跨区护栏 · IF-03 去重回执 · IF-06 会话活跃时间 · IF-07 不兜底
        │
        ├──────────────► W2 真相落盘（2–3 天）
        │                  ⑧ region 元数据 · IF-02 去重持久化 · split observer 补偿
        │                       │
        │                       ▼
        │                  W3 跨区 2PC（4–6 天）
        │                  ⑦ 协调器 · RegionGrouper · 恢复路径
        │                       │
        └──────────────► W4 并发与边界（2–3 天，可与 W3 并行）
                           ⑨a 锁分片 · ⑩ Auth/TLS · ⑨c 拆组合根
                                │
                                ▼
                           W5 度量与运维（持续）
```

并行约束只有一条：**⑨a 必须在 W1 之后**（TTL 与护栏没修好就拆锁，等于在流沙上盖楼）。

## 3. 任务卡

### W1 正确性封口

| ID | 目标 | 涉及文件 | 依赖 | 验收 | 风险 / 回退 |
|---|---|---|---|---|---|
| T-00 | `Peer::applied_` 构造时从 `RaftLog::applied()` 播种（P0-4） | `peer.cpp` 构造函数 | — | 重启后不写先读，`GET /kv` 直接返回而非 `IoFailed` | 极低。一行赋值 + 一条用例；回退删该行 |
| T-01 | 锁 TTL 时间基准随请求传递（IF-01） | `proto/kv.proto`、`peer_txn.h`、`peer.cpp`、`mvcc.h` | — | 新增用例：两事务争同一 key，持锁方存活期间被阻塞而非被回滚 | 低。proto 追加字段号，旧值按 start_ts 兜底；回退只需不再填该字段 |
| T-02 | 跨区事务护栏（IF-04） | `peer_txn.h` | — | 多 region 下跨区 prewrite 返回 `InvalidArgument`，不再静默写错分区 | 低。纯拒绝路径，正确路径不变；回退删护栏即可 |
| T-03 | 去重命中登记合成回执（IF-03） | `peer.cpp::propose`、`server_app.h` | — | 重复 seq 的调用在 `wait_applied` 不超时 | 中。触及 `wait_applied_locked` 判定口径；回退保留旧短路 |
| T-04 | 会话按 last_active 回收（IF-06） | `txn_sessions.h` | — | 活跃 31 秒的长事务不被 sweep 误杀 | 低。加一个时间戳字段 |
| T-05 | `require()` 不再兜底错路由（IF-07） | `peer_registry.h` + 4 处调用 | — | 缺失 region 时返回 `KeyNotInRegion`，`/status` 可见 | 中。签名变更波及调用点；回退加回兜底分支 |
| T-06 | 三个悬挂端点收口（IF-05 的前置） | `cli.cpp` 或 `server_app.h` | — | CLI 三命令不再恒 404；或显式移除命令并更新 `docs/cli.md` | 低。二选一，不留"发了但没人接" |

**W1 的退出条件**：`ctest -j1` 全绿 + 新增 4 个用例 + `docs/bugs.md` 登记 TTL 与跨区分区两条缺陷。
W1 不新增任何对外能力，只把"会写错"改成"会报错"。

### W2 真相落盘

| ID | 目标 | 涉及文件 | 依赖 | 验收 | 风险 / 回退 |
|---|---|---|---|---|---|
| T-07 | `meta/regions.pb` 持久化（⑧） | 新增 `raftstore/meta_store.h`、`peer_registry.h`、`server_app.h` | W1 | split 后重启，`/regions` 与重启前一致，右半数据可读 | 中高。需迁移脚本建 `meta/`；回退删文件即回单 region |
| T-08 | 启动按 meta 重建 peer（⑧） | 同上 | T-07 | 重启后 region 数与 meta 一致，各 peer 能当选 | 中。多 region 启动路径首次被真实执行 |
| T-09 | split observer 失败补偿（W6 窗口） | `peer_registry.h::on_split`、`server_app.h` | T-07 | 注入建 Engine 失败，重启后拓扑自愈或明确报错 | 中。需要故障注入测试 |
| T-10 | 去重台账持久化（IF-02） | `peer.cpp`、键空间新增 `\0dd` | W1 | kill -9 后重放旧请求不二次 apply | 中高。日志载荷格式变更，需版本字节 + 迁移 |
| T-11 | 读路径接 epoch 校验（IF-08） | `server_app.h`、`cli.cpp` | T-07 | 携带旧 epoch 的请求收 `StaleEpoch{have,want}`，客户端刷新后成功 | 中。CLI 需学会重取路由 |

**W2 的关键判断**：`T-07` 是 `W3` 的前置真相。路由不持久化就上 2PC，未决事务会被解析到错误分区——
协调器再正确也没用。这条顺序不能倒。

### W3 跨区 2PC

| ID | 目标 | 涉及文件 | 依赖 | 验收 | 风险 / 回退 |
|---|---|---|---|---|---|
| T-12 | `TxnCoordinator` + `RegionGrouper` | 新增 `txn/coordinator.h`、`server/peer_txn.h` | T-02、T-07 | 跨两 region 的事务全可见或全不可见 | 高。协议层新增，需先设计再编码 |
| T-13 | Phase 状态机（PREPARE→COMMIT_PRIMARY→COMMIT_SECONDARY→DONE） | 同上 | T-12 | 每阶段可中断、可恢复；崩溃后无半提交 | 高。状态机要能被日志重放驱动 |
| T-14 | 恢复靠 primary 锁 | `txn/client.h`、`peer_txn.h` | T-12 | 协调者进程死亡后，读者能通过 `check_txn_status` 收敛 | 中。复用既有三态决议，不新增协议 |
| T-15 | 跨区事务 chaos 用例 | `tests/integration/txn_chaos_test.cpp` | T-12 | 150 轮随机注入无 FAIL | 中。需要确定性注入点 |

`T-12`–`T-14` 建议拆三个 PR，每个 PR 自带用例：协调器空转、单区退化、双区正常、双区中断。
**不允许一次性提交"完整 2PC"**——这类改动一旦不收敛，无法定位是分组错还是阶段错。

### W4 并发与边界

| ID | 目标 | 涉及文件 | 依赖 | 验收 | 风险 / 回退 |
|---|---|---|---|---|---|
| T-16 | 锁分片（⑨a，Strangler） | `server_app.h`、新增 `server/txn_gate.h` | W1、W3 | `shard_count=1` 行为与现在逐字节一致；>1 时 bench 吞吐提升且同 region 串行性不破 | 中高。必须先写锁序契约再动手 |
| T-17 | Auth 拦截器 + TLS 装饰器（⑩） | `server/interceptor.h`、`raftstore/tcp_transport.h` | — | 未授权请求 401 且不泄露凭据；未启 TLS 时强制绑回环并告警 | 低（默认关） |
| T-18 | 拆组合根（⑨c） | `server_app.h` → `app / http / view` 三个单元 | T-16 | 单文件 ≤400 行；`intercept_calls` 等对外字段不变 | 低。纯重构，靠现有 65 个测试兜底 |
| T-19 | `/metrics` 直方图 | `interceptor.h`、`status_server.cpp` | T-16 | 延迟 P50/P99 可读，不再只有计数 | 低 |
| T-20 | 崩溃注入测试层 | 新增 `tests/fault/` | W2 | W3/W4/W6 三个窗口各有可复现用例 | 中。注入点需设计，不能靠 kill 随机 |

### W5 度量与运维（持续，不设截止）

- bench 基线固化为 CI 前置：`read/write/cluster/heartbeat_bandwidth` 四条曲线，回归超阈值报警。
- `/status` 增 `pending_reads`、`replies`、`txn_errors` 三个"可能泄漏"的计数（现在只能看到 `apply_errors`）。
- 每 store region 数 vs 句柄/内存的实测曲线——这是 ⑨b（共享引擎）唯一的启动条件。

## 4. 工程布局

### 4.1 现状

```
src/  9 965 行 / 53 个文件（.h + .cpp）
├── common     error/encode/ids/log/crc32c/varint      9
├── config     YAML 最小子集                            2
├── storage    engine/memtable/sst/wal                 8
├── raft       raft/raftlog/entry_batch                5
├── raftstore  peer/region/tcp_transport               6
├── txn        mvcc/latch/executor/client               4
├── scheduler  tso/scheduler                            2
├── server     app/http/session/worker/interceptor     13
└── cli        cli/http_client/main                     4
```

问题只有一个：`server/` 13 个文件里，`server_app.h` 711 行同时承担装配、HTTP 分派、
JSON 序列化、时钟、计数、拦截器持有、split 编排。其余 12 个文件职责都清楚。

### 4.2 目标布局（最小改动版）

不建新子目录，先把文件拆开——符合项目"少于 3 个文件不单独建目录、深度 ≤3"的约束：

| 拆出单元 | 职责 | 预估行数 | 依赖 |
|---|---|---|---|
| `server_app.h/.cpp` | 装配 + Service 生命周期 + 泵 | ≤280 | 全部 |
| `admin_api.h/.cpp` | POST/GET 分派表 + 端点契约 | ≤200 | session/executor/meta |
| `status_view.h/.cpp` | `/status` JSON + 计数聚合 | ≤150 | registry/worker |
| `json.h` | 转义与字段拼接（消灭手拼 JSON） | ≤80 | 无 |

若拆分后 `server/` 仍超 10 个文件，再考虑 `server/http/`（3 文件）与 `server/session/`（2 文件）——
**先拆文件，再谈目录**，否则只是把 God class 换个路径。

### 4.3 依赖规则（写进 review 检查项）

| 规则 | 违反示例 | 检查方式 |
|---|---|---|
| `common` 不 include 任何本项目头（ids.h 除外） | `common/error.h` 引 `storage/` | `grep include` 人工 + CI align |
| `raft` 不感知 `raftstore` / `server` | `raft.cpp` 引 `peer.h` | 编译单元隔离（raft 测试只链 storage） |
| `txn` 不感知 `raft` | `mvcc.h` 引 `raft.h` | 同上 |
| `raftstore` 可用 `raft`/`txn`，但不得引 `server` | `peer.cpp` 引 `server_app.h` | 同上 |
| `server` 是唯一允许"什么都知道"的层 | — | — |
| `cli` 只经 HTTP 或 `Engine` 公开接口 | 直接 include `peer.h` | 同上 |

当前六条全部成立。这条清单的价值在**守住**：`cli.cpp` 里已经出现 `/region/peers` 这种
"以为服务端有"的路径，说明跨层假设没有被显式化。

### 4.4 数据目录布局

```
data_dir/
├── regions/<id>/            # 每 region 一份 Engine（wal/ sst/ manifest）
│   ├── wal/wal-000001.log
│   ├── sst/
│   └── manifest
├── meta/                    # [T-07 后] 路由真相
│   ├── regions.pb
│   └── regions.pb.bak
└── （region 1 兼容路径）
```

**遗留问题**：`bootstrap()` 让 region 1 复用 `data_dir` 根目录（兼容 M1 布局），
split 出的子 region 却在 `regions/<id>/`。两种布局并存会让"删数据目录"的语义变得含混。
建议在 T-07 的迁移脚本里顺手把 region 1 迁到 `regions/1/`，一次性消除双布局。
**这是格式迁移，必须走 `tinykv-cli migrate`，不得手改目录。**

### 4.5 文档体系

现有 `docs/` 混了五种性质的文件，命名已经不足以表达性质：

| 前缀 / 后缀 | 性质 | 是否冻结 | 例子 |
|---|---|---|---|
| `SPEC-n-*` | 分阶段规格（含验收与证据链） | 冻结，修订留痕 | `SPEC-4-transaction.md` |
| `ARCH-n-*` | 分析与设计（本文三册） | 可修订 | `ARCH-1/2/3` |
| `notes-*` | 阶段实现笔记 | 只增 | `notes-m5.md` |
| `*-DRAFT` | 未定稿提案 | 可删 | `M4-CONFIRMCHANGE-DRAFT.md` |
| `Project1-4*` / `README.md` | 上游参考材料 | 冻结（CI 排除） | — |
| `bugs.md` / `testing.md` / `review-log.md` | 台账 | 只增 | — |

建议补两条纪律：

1. **DRAFT 不被任何冻结文档引用**。现在 `M4-*-DRAFT` 与 `SPEC-3` 的内容有重叠，
   读者无法判断哪份是最终口径。
2. **每册 ARCH 在开头写明"覆盖哪几个 SPEC"**。本文三册覆盖 SPEC-0（错误与编码）、
   SPEC-2（Raft）、SPEC-3（Multi-Raft）、SPEC-4（事务）、SPEC-5（运行时）、SPEC-6（M7 规划）。

### 4.6 测试布局

```
tests/
├── unit/         现有 46 个，按模块分目录
├── simulation/   4 个，确定性消息序列
├── integration/  8 个，真进程/真 TCP
├── bench/        7 个，独立于产品代码
└── fault/        [T-20] 崩溃窗口注入（≥3 文件才建目录）
```

四层齐备，缺的是"故障注入"这一层：现在的崩溃用例靠 kill 进程，命中的窗口是随机的。
`tests/fault/` 的价值在于把 W3/W4/W6 三个窗口变成**确定性可复现**的用例，
而不是"跑 150 轮碰运气"。建目录的门槛（≥3 文件）由 T-20 的三个窗口天然满足。

## 5. 门禁与度量

| 门禁 | 现状 | 演进后 |
|---|---|---|
| `ctest -j1` | 65/65 | 65 + W1 4 + W2 6 + W3 6 ≈ 81 |
| CI 十三阶段 | PASS×13 | 保持；新增 `tests/fault` 需先登记 `docs/testing.md` §4 |
| 登记表对账 | 65 ⇄ 65 | 每次新增用例同提交登记 |
| 锁序检查 | 无 | T-16 前先落 `server/txn_gate.h` 的锁序注释与断言 |
| 性能回归 | 手工 bench | W5 固化为阈值 |

**一条容易踩的坑**：CI 的 scope 阶段校验"改动的文件是否在任务声明内"。
W2/W3 会横跨 `proto`、`raftstore`、`server`、`tests`，任务开始前必须整份重写
`ci/task_scope.json`，而不是补一行——M7 六项演进时就因此报过 16 个 `SCOPE-OUT`。

## 6. 非目标与触发条件

| 不做 | 理由 | 重新评估的触发条件 |
|---|---|---|
| Region Merge | 与 split 对称但收益只在"碎片化"场景显形 | region 数 > 500 且碎片率 > 30% |
| Joint Consensus | 单步 + Learner 已能守住安全性 | 出现"变更期间丢已提交数据"的可复现用例 |
| 悲观事务 | SI 已满足课程目标 | 需要"写不冲突也要串行"的业务语义 |
| 共享引擎（⑨b） | 改动横跨日志键布局与数据目录，须配迁移 | 实测"region 数 vs 句柄/内存"曲线拐点出现 |
| 完整 PD 策略 | 单 store 调度无意义 | 多 store 部署成为常态 |
| 在线升级 / 多租户 | 运维层非目标 | 出现第二个使用方 |

## 7. 里程碑检查单

每个波次收口时逐条打勾，缺一条不进入下一波：

**W1**
- [ ] `T-00` 落地：重启后不写先读可用（`build/live/verify.py` 场景由红转绿）
- [ ] TTL 缺陷有回归用例，且用例在修复前是红的
- [ ] 跨区 prewrite 返回结构化错误而非静默成功
- [ ] `docs/bugs.md` 两条新登记，`docs/testing.md` §4 对应登记
- [ ] `ctest -j1` 全绿，CI 十三阶段 PASS

**W2**
- [ ] split → 重启 → `/regions` 与重启前逐字段一致
- [ ] 右半 region 的数据在重启后可读（不是"目录在但没人管"）
- [ ] 迁移脚本可重复执行且幂等，`meta/regions.pb.bak` 在半写时可回退
- [ ] `tests/fault` 覆盖 W6 窗口

**W3**
- [ ] 跨两 region 事务：全提交 / 全回滚 / 中断恢复 三条路径各有用例
- [ ] 协调者死亡后，读者能在有限时间内通过 primary 锁收敛
- [ ] 单 region 事务性能不回退（bench 对比）

**W4**
- [ ] `shard_count=1` 与改造前行为一致（黄金用例逐字节比对）
- [ ] 锁序写进契约并有断言，`gate.reset()` 一类绕道补丁从代码中消失
- [ ] 未启 TLS 时强制绑回环并打印告警
- [ ] `server_app` 拆分后单文件 ≤400 行，对外 JSON 字段不变

---

**一句话总结**：这个项目的架构底子是"把每条不变量都能指到一段代码"，
下一阶段的工作不是加新模块，而是把三条已经写出来但没接上的线（TTL 基准、epoch 校验、跨区归属）
接回去，再把路由真相落到盘上。顺序错了，后面每一步都在还债。
