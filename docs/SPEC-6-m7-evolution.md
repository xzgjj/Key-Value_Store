# SPEC-6 — M7 演进：多 Region 可用性、元数据真相、并发与安全边界

> 上游：SPEC-5（运行时）；需求：FUNC-07/13 的扩展 + 本册新增 M7-REL-01、M7-PERF-01、M7-SEC-01/02。
> 状态：**规划中**（范围与决策已定，未开工）。编号 ⑦–⑩ 承接在飞任务已占用的 ①–⑥，不重号。
> 登记表联动：§6.7 的计划测试名实现时必须先登记 [testing.md](testing.md) §4，未登记 = CI 红。

## 6.0 与在飞任务的关系

M7 不是从零开始：另一执行线已在工作区落地六项演进（未提交）。本册只规划**没做过的部分**，
先给对账结论，避免同一件事被规划两次。

### 6.0.1 六项在飞清单的实测状态

| # | 条目 | 证据 | 状态 |
|---|---|---|---|
| ① | applied 位点持久化 + 崩溃重放 | `\0la` 独立键、`Raft::mark_applied`、Peer 整批确认 | 已落地 |
| ② | 日志压缩阈值 + SST 合并 | `can_compact`、压缩阈值 512、SST 阈值 8 | 已落地 |
| ③ | 读路径统一 ReadIndex | `Peer::wait_read_index` | 已落地 |
| ④ | apply 错误上 /status + 观测独立 io_context | `observe_port`、`status_snapshot()` 快照缓存 | 已落地 |
| ⑤ | Transport 与多 voter 装进组合根 | `TcpTransport` 进 `ServerApp`、`voters`/`replica_id` | 已落地 |
| ⑥ | 拆 God class / 流式 scan / 共享 memtable 快照 | 后两项已做；拆分未开始 | 部分 |

### 6.0.2 两处收口：一处已关闭，一处仍在

1. **登记缺口已关闭**（2026-11-18 同时段由在飞任务补齐）：`multi_replica_test` 已进 `testing.md` §4，
   登记 65 ⇄ 注册 65，docs 阶段转绿。本册不重复记录该动作。
2. **范围声明缺口仍在**：`ci/task_scope.json` 的任务声明只覆盖 ① 的路径，②–⑤ 的源码改动多在声明外，
   scope 阶段报 SCOPE-OUT。提交前必须重写整份声明，不是补一行。

⑥ 的"拆 God class"并入本册 ⑨c，不另编号。



## 6.1 功能范围

| 条目 | 交付一句话 | 类型 |
|---|---|---|
| ⑦ 跨 Region 事务 | 协调器 + 按区分组 2PC；跨区写入不再落错分区 | FUNC-13/14 扩展 |
| ⑧ Region 元数据持久化 | 路由真相落盘，重启按 meta 重建 peer | FUNC-07 扩展 |
| ⑨a 锁分片 | 按 region 分桶并发，甩开全局串行闸 | M7-PERF-01 |
| ⑨c 拆 ServerApp | 组合根只做装配，职责拆三个服务 | ENG-04 扩展 |
| ⑩ 安全边界 | 鉴权拦截器 + TLS 传输装饰器，默认关 + 告警 | M7-SEC-01/02 |
| ⑨b 共享引擎 | **移出 M7**，见 §6.1.3 | — |

### 6.1.1 本册新增需求

原设计文档 44 项需求之外的部分单独编号，不并入 §0.9 覆盖矩阵：

| 编号 | 需求 | 优先级 | 验收 |
|---|---|---|---|
| M7-REL-01 | 跨区事务要么全可见要么全不可见；崩溃后由锁决议收敛，不产生半提交 | P0 | §6.7 |
| M7-REL-02 | split 后重启，路由与数据一致；apply 与 meta 之间崩溃不丢已提交切分 | P0 | §6.7 |
| M7-PERF-01 | 多 region 并发吞吐不低于改造前；同 region 串行性不破 | P1 | §6.7 |
| M7-SEC-01 | 未授权请求被拒绝且不泄露凭据 | P1 | §6.7 |
| M7-SEC-02 | 可选 TLS 传输；未启用时必须绑定回环地址 | P2 | §6.7 |

### 6.1.2 投入产出排序

| 序 | 条目 | 投入 | 收益 | 风险 | 依赖 | ROI |
|---|---|---|---|---|---|---|
| 0 | ⑦-0 跨区护栏 + 在飞收口 | 0.5 天 | 消除静默写错分区；CI 回绿 | 低 | — | 最高 |
| 1 | ⑧ 元数据持久化 | 2–3 天 | 重启不丢拓扑，是 ⑦ 的前置真相 | 中 | — | 高 |
| 2 | ⑦ 2PC 协调器 | 4–6 天 | 对齐 TiKV 的核心能力 | 高 | ⑧ | 中高 |
| 3 | ⑩ 安全边界 | 1–2 天 | 评审必问项、部署前提 | 低 | — | 中高（可并行） |
| 4 | ⑨a 锁分片 | 1–2 天 | 抬并发上限，可用现有 bench 证明 | 中 | ⑦ 完成 | 中 |
| — | ⑨c 拆分 | 持续 | 可读性 | 低 | — | 低 |

**建议序列**：`0 → 1 → 2 →（3 并行）→ 4`，⑨c 随改动顺带。

排序依据四条：

1. **先真相后协调**：路由不持久化就上 2PC，未决事务会被解析到错误分区——⑧ 必须早于 ⑦。
2. **一次只放开一个自由度**：先在单闸下把 2PC 语义跑对，再拆锁。否则并发 bug 与协议 bug 纠缠，无法归因。
3. **能关掉才敢上**：新能力都要有开关与保守默认（LeaseRead 同理）。
4. **没有度量不做优化**：⑨b 这类结构性改造必须先有 bench 数据。

### 6.1.3 非目标

Region Merge、Joint Consensus、悲观事务、完整 PD 策略、跨机房复制、RDMA、**共享引擎单例化**（⑨b）。

⑨b 移出理由：改动面横跨日志键布局与数据目录，必须配迁移脚本；收益要 region 数到规模才显形。
在拿到"每 store region 数 vs 句柄/内存"的实测曲线前，投入产出不成立。触发条件与设计草案留在
`implementation-notes.md`，够条件时另立一册。

## 6.2 架构

### 6.2.1 现状事实：跨区事务会写错分区

`PeerTxnExecutor::prewrite` 把整批 mutations 经 **primary 所在 peer** 一次提案；`Peer::propose_txn_prewrite`
只封包进日志，不校验 key 归属；而 `get` 按 key 路由。于是 primary 在 region 1、secondary 在 region 2 的
事务，写入全落 region 1 的引擎，读 region 2 得空——对外表现为"提交成功但数据不见了"。

结论：这不是能力缺口，是正确性洞。⑦ 的第 0 步是**护栏**，不是协调器。

### 6.2.2 目标结构

```
CLI / TxnSession → TxnClient → TxnCoordinator（新增，server 层）
                                ├─ RegionGrouper：mutations 按 region 分桶
                                ├─ Phase 状态机：PREPARE → COMMIT_PRIMARY → COMMIT_SECONDARY → DONE
                                └─ TxnExecutor（既有 Strategy 抽象：单机 / 经 raft 两实现）

RegionPeerRegistry ──► StoreMetaStore（新增，Repository）
                         ├─ 写：split apply 成功后同批落 meta/regions.pb
                         └─ 读：启动 load() → 有 meta 逐 region 建 peer；无 meta 回退单 region
```

分层不变式：**协调在 client 侧，apply 只做本地语义**（沿用 SPEC-4 红线 8）。Peer 之间不互发 RPC，
避免 apply 路径里出现分布式调用成环。

### 6.2.3 元数据存储形态（D6-1 已定：独立文件）

```
data_dir/
├── regions/<id>/           # 每 region 一份 Engine（不变）
└── meta/
    ├── regions.pb          # 全量快照：regions[] + epoch + version + CRC32C
    └── regions.pb.bak      # 上一次成功版本（半写时回退用）
```

选择独立文件而非塞进 Engine 键空间：元数据必须在"引擎可能打不开"的情况下仍可读——
这正是恢复期最需要它的时刻。代价是多一个文件与一次 fsync，可接受。

## 6.3 接口设计

### 6.3.1 proto

⑦ 与 ⑧ **proto 零改动**：prewrite/commit 请求本就按 region 分批发送，meta 走独立文件。
若 ⑦-2 需要给 `TxnExecutor` 增加按区分批入口，属 C++ 接口变更，先与用户核对再动（CLAUDE.md §15）。

### 6.3.2 C++ 侧

```cpp
namespace tinykv::txn {

// 跨区协调器：只编排，不碰存储。恢复真相在 primary 的锁，不在本对象内存。
class TxnCoordinator {
public:
    TxnCoordinator(TxnExecutor& exec, tso::TsProvider& tso);
    [[nodiscard]] result<Ts> begin();
    result<void> set(std::string key, std::string value);
    [[nodiscard]] result<Ts> commit(std::uint64_t now_ms);   // 分组 → PREPARE → COMMIT_PRIMARY → SECONDARY
    result<void> rollback();
    [[nodiscard]] std::size_t region_count() const;          // 观测：本次事务跨了几个 region
private:
    [[nodiscard]] result<RegionGroups> group_by_region() const;
};
}

namespace tinykv::server {

// meta 仓储：写全量快照 + 保留上一版；读到半写回退，绝不猜。
class StoreMetaStore {
public:
    StoreMetaStore(std::filesystem::path data_dir, raftstore::StoreMeta& meta);
    [[nodiscard]] result<void> load();                        // 无文件 = 单 region 兼容路径
    [[nodiscard]] result<void> persist();                     // apply 路径内调用，幂等
};

// 锁注册表：按 RegionId 固定分桶；shard_count=1 等价改造前行为。
class LockRegistry {
public:
    explicit LockRegistry(std::size_t shards = 16);
    [[nodiscard]] std::unique_lock<std::mutex> lock(RegionId id);
};
}
```

### 6.3.3 错误矩阵（新增部分）

| 变体 | 触发 | 语义 |
|---|---|---|
| `InvalidArgument{"跨区事务未启用"}` | ⑦-0 护栏命中：mutations 跨 region 且协调器未开 | 明确拒绝，禁止落错分区 |
| `RegionError` 包装 | 某 region prewrite 失败 | 触发全量回滚，回滚本身幂等 |
| `IoFailed{"meta"}` | meta 落盘失败 | 停止推进并告警；下次 apply 重试 |
| `Unauthorized`（新增变体，需 proto 对齐声明） | 鉴权失败 | 401，日志不含凭据明文 |

## 6.4 核心逻辑骨架

```
⑦ commit（跨区）:
  groups = group_by_region(writes)                  # key → region，读路由表，不持锁
  if groups.size() == 1: 走原单区路径（零额外开销）
  for g in groups: prewrite(g)                      # 任一组失败 → rollback(已成功的组)
  commit_ts = TSO()
  commit(primary_group, commit_ts)                  # 原子锚点落定
  for g in secondary_groups: commit(g, commit_ts)   # 可异步；失败由锁决议补
  恢复：任何一侧崩溃后，读遇 Lock → check_txn_status(primary) → 正向推进或回滚

⑧ persist（apply 内）:
  apply(SplitAdmin) → StoreMeta::replace_region → 迁数据 → meta_store.persist()
  重放：命中已切分区间 → 幂等跳过（既有守卫）→ persist 覆盖为同一内容

⑨a 加锁:
  auto g = locks.lock(region_id)                    # 桶内互斥，桶间并行
  不变量：同一 region 的 apply 仍严格串行；跨 region 无全局顺序要求

⑩ 鉴权:
  interceptor.before: 取 Authorization → 常时比较 → 失败即 401（after 不写敏感字段）
  未配置 token → 启动即 WARN："鉴权未启用，端口仅绑定回环"
```

## 6.5 红线

1. **不许静默写错分区**：跨区请求在协调器不可用时必须报错，禁止退回"全写 primary"的旧行为。
2. **协调不跨 apply**：apply 路径只执行本地语义，不得因协调需求发起对等 RPC。
3. **原子锚点是 primary 的 Write 记录**：secondary 的存在与否由锁决议决定，不由协调器内存决定。
4. **meta 不猜**：读到 CRC 不符或半写，回退上一版并告警，绝不"修补后继续"。
5. **同 region 串行不可破**：分片只改变不同 region 之间的并行度，不改单 region 的 apply 顺序。
6. **开关优先**：⑦/⑩ 必须有可关闭的开关，默认保守；开关状态要能在 `/status` 看到。
7. **回滚点写在 `diff.md`**：每一步独立可回退，不允许"整坨 revert"。
8. **测试先行登记**：§6.7 的用例名先登记再写代码，未登记视为没做。

## 6.6 示例

### 跨区事务（两 region）

```cpp
auto t = coord.begin();          // start_ts
t.set("a", "1");                 // 落 region 1
t.set("z", "2");                 // 落 region 2
t.commit(now_ms);                // 两个 region 各一次 prewrite，primary 提交后 secondary 推进
// 断言：任一步崩溃后重启，要么两个 key 都可见，要么都不可见
```

### 护栏（⑦-0 生效后）

```bash
$ tinykv-cli --endpoint 127.0.0.1:3380 txn set "$SID" z 2
error: 跨区事务未启用（key=z 属 region 2，primary 属 region 1）
hint:  同 region 内写入，或等待 M7 ⑦ 完成
```

### split 后重启

```bash
tinykv-cli ... region split 1 m        # 切成 [.., m) 与 [m, ..)
# 重启服务端
tinykv-cli ... regions                 # 仍是两个 region（M6 行为是回退成一个）
```

### 配置项（增量）

| 配置项 | 默认 | 影响 |
|---|---|---|
| `txn.cross_region_enabled` | true | ⑦ 协调器开关（2026-11-18 随机 kill chaos 通过后转正）；关时只允许单区事务 |
| `raftstore.meta_dir` | `meta` | ⑧ 元数据目录（相对 data_dir） |
| `server.lock_shards` | 16 | ⑨a 分片数；1 = 等价改造前 |
| `server.auth_token` | 空 | ⑩ 为空即关闭鉴权，并在启动时告警 |
| `server.tls_cert/key` | 空 | ⑩ 为空即明文；非空必须同时给两者 |

## 6.7 验收与证据链

| 需求 | 验收口径 | 计划测试名 | 证据物 |
|---|---|---|---|
| ⑦-0 | 跨区写入返回结构化错误，错误 region 无数据落地 | `txn_cross_region_guard_test` | junit.xml |
| M7-REL-01 | 跨两 region 事务原子可见；中途杀 leader 后可收敛 | `txn_cross_region_test`、`txn_cross_region_chaos` | `build/evidence/nightly/` |
| M7-REL-02 | split → 重启 → 路由/数据/再 split 一致；apply 与 meta 间崩溃不丢 | `split_meta_persistence_test` | junit.xml |
| M7-PERF-01 | 同 region 串行性不破；多 region 吞吐不低于改前 | `lock_registry_test`、`tcp_perf_bench` 对比 | `build/evidence/bench/` |
| M7-SEC-01 | 无 token/错 token 拒绝，日志与响应不含凭据 | `auth_test` | junit.xml |
| M7-SEC-02 | TLS 往返成功；未启用时绑定回环 | `tls_transport_test` | junit.xml |
| ENG-04 | 组合根只剩装配；三个服务各自可单测 | `server_app_test`（回归）、`lifecycle_test` | junit.xml |

**M7 完成判定**：⑦⑧ 的 P0 行证据齐备 + ⑦ 的 chaos 夜跑 150 轮无 FAIL + `docs/notes-m7.md`。
⑨c 不设完成判定（持续项，随改动评估）。

**当前进度（2026-11-18）**：⑧ 完成；⑦ 完成（护栏 → 分组 → 状态机 → 恢复四步齐备，
`txn_cross_region_test` 3 例 + `txn_cross_region_chaos` 150 轮）且**已转正**——
`txn_cross_region_chaos_random` 补上随机 kill（子进程 `_Exit` 真崩溃语义 + 死亡点/主组侧/半途决议
三个随机维度，150 轮），通过后经用户批准 `txn.cross_region_enabled` 默认 true，
开关状态进 `/status`（红线 6 补齐）；⑨a 原语完成、接线进行中；⑩ 鉴权完成、TLS 经用户裁决
**移出 M7**（D-2 收口：TLS 依赖与证书生命周期另立项）。
`docs/notes-m7.md` 随 ⑨a 接线批次一起写（避免半成品笔记变成第二份事实源）。

## 6.8 开放决策点

| # | 决策 | 结论 |
|---|---|---|
| D6-1 | meta 存独立文件还是 Engine 键空间 | **已定**：独立文件 `meta/regions.pb` + `.bak`（2026-11-18 用户选择） |
| D6-2 | ⑨b 共享引擎是否进 M7 | **已定**：移出 M7，等 bench 数据另立项（2026-11-18 用户决定） |
| D6-3 | 鉴权默认开还是关 | **已定**：默认关 + 启动告警 + 强制绑回环（2026-11-18 用户选择） |
| D6-4 | ⑦-0 护栏何时生效 | 按建议默认：动工即做（行为从"静默成功"变"明确报错"）；若否决需在开工前说明 |
| D6-5 | ⑦ 是否覆盖跨 store | 建议 M7 内只做**单 store 跨 region**；跨 store 需要 leader 定位与真 RPC，另立项 |
| D6-6 | ⑨a 分片数 | 16（固定桶，按 RegionId 取模）；`1` 为回退档 |
| D6-7 | `Unauthorized` 是否进 KvError | 需要与 proto 消息对称（align 检查）；倾向新增变体并在 `contract_exempt` 之外正式对齐 |

## 6.9 进展

| 日期 | 事件 | 影响 |
|---|---|---|
| 2026-11-18 | 本册建立（由 `docs/M7-PLAN.md` 升格，原文件作废） | D6-1~D6-3 落定；⑦-0 待动工 |
| 2026-11-18 | ⑦-0 护栏落地：`PeerTxnExecutor::guard_single_region` 在提案前按 key 归属拒绝跨区 prewrite（BUG-0008） | ⑦ 的第一步完成；跨区从"静默写错分区"变成明确报错 |
| 2026-11-18 | ⑧ 落地：`StoreMetaStore`（`meta/regions.pb` + `.bak` + CRC）、启动按 meta 重建 peer、split 后落盘；`split_meta_persistence_test` 三断言通过 | M7-REL-02 的证据链齐备；`next_region_id` 取盘上最大值 +1 |
| 2026-11-18 | ⑨a 原语落地：`LockRegistry`（分桶 + 同区串行/跨区并行用例），**默认未接线** | 按 ARCH-3 §2 的并行约束，等 ⑦ 收口后再改组合根 |
| 2026-11-18 | ⑩ 部分落地：Bearer token 鉴权（常量时间比较、401 不回显）+ 未鉴权强制回环 + `/status` 可见；**TLS 未做** | M7-SEC-01 达成；M7-SEC-02 只剩 TLS 一项，见 D-2 |
| 2026-11-18 | ⑦ 第二步（分组）落地：`txn/region_grouper.h` 纯函数 + 6 例 | 分组正确性可与协议正确性分开验证 |
| 2026-11-18 | ⑦ 第三/四步落地：`TxnClient` 分组提交（PREPARE → TSO → COMMIT_PRIMARY → COMMIT_SECONDARY）+ 恢复路径（`resolve_locks_of` 广播到各 region 引擎）；`txn.cross_region_enabled` 开关 + `txn_cross_region_test` 3 例 | M7-REL-01 的三条路径（全可见/全不可见/中断收敛）各有一条用例；默认仍关 |
| 2026-11-18 | ⑦ chaos 证据：`txn_cross_region_chaos` 150 轮枚举三个中断窗口（只 PREPARE 主组 / 全 PREPARE 未取 ts / 主组已提交），断言半提交不出现 | §6.7 的 chaos 行达成；默认开关是否打开仍待你拍板 |
| 2026-11-18 | ARCH-1 P1-1 收口：`/region/peers`、`/scheduler/status`、`POST /trace` 服务端实现 + `admin_endpoints_test` | CLI 三个悬空命令不再 404 |
| 2026-11-18 | ⑦ chaos 升级为随机中断：`txn_cross_region_chaos_random`（子进程 `_Exit` 真崩溃 + 死亡点/主组侧/半途决议随机，150 轮）通过 | §6.7 的"夜跑级随机中断"缺口补齐；修复前 kill≥3 轮必失败（目录与主组侧错位），红→绿有据 |
| 2026-11-18 | ⑦ 开关转正：`txn.cross_region_enabled` 默认 true（用户批准），开关状态进 `/status`（红线 6） | 跨区事务正式可用；M7-REL-01 证据链闭合 |
