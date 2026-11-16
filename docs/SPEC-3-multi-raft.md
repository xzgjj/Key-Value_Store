# SPEC-3 — M4 Multi-Raft：Split / 成员变更 / 调度器（白皮书 Project3）

> 上游：SPEC-2（RaftKV）；需求：FUNC-07~12、REL-03/04/05、PERF-07/08/09、UX-04(完整)/06。
> 全项目最大坑（白皮书作者 P3 耗 3 周）——**一切子特性先在确定性模拟器跑通再上真网络**。状态：待实现。

## 3.1 功能范围

- Region 元数据模型：`Region{id, start_key, end_key, peers[], RegionEpoch{conf_ver, version}}`；
- Region Split（含定时 split-check）；单步成员变更（AddLearner→AddVoter→RemoveNode）+ Learner；
- Leader Transfer；快照全链路（region 任务队列 / 分块传输 / 安装 / GC）；
- 调度器 PD-lite（独立进程）：store/region 心跳收集 + size 均衡 MovePeer + AskSplit；
- Client 路由：region 路由缓存 + epoch 校验失败回源刷新 + `NotLeader{leader_hint}` 重定向；
- UX-04 完整版：region 维度 trace 开关 + metrics 汇聚。

## 3.2 架构

```
CLI/Client ─路由缓存─► Router(epoch 校验) ─► RaftStore(每 store 一进程)
   ├ StoreMeta: regions map + regionRanges(btree<end_key,RegionId>) + RWMutex
   ├ Peer(region) × N（一 store 一 region 至多一 peer）
   ├ split-check 周期任务 + snapshot 任务队列 → SnapshotStream(分块+校验，独立通道)
   └ trace 开关（region 粒度，UX-04）
Scheduler(PD-lite, 独立进程, 只动元数据/数据面零流量——Mooncake 控制面分离原则):
   store 心跳(容量/size) + region 心跳(epoch/peers) → size 均衡 MovePeer 三步 / 响应 AskSplit
```

**epoch 比较规则**：`version`（split/merge 更新）与 `conf_ver`（成员变更更新）**逐字段大者新**，
两字段均 ≥ 才算不旧；请求 epoch 过旧 → `StaleEpoch{have, want}` 拒绝 + 客户端回源刷新。

### 3.2.1 Peer 生命周期状态图

```
            create(apply Split / AddLearner / 安装快照新建)
  ─────────► [uninitialized] ──(安装快照完成)──► [initialized] ──► 正常参与共识
                 │                                   │
                 │ destroy(被 RemoveNode)             │ destroy / apply snapshot 重装(先清旧数据!)
                 ▼                                   ▼
              [destroyed: 路由摘除, worker 停止投递, WAL/数据目录待 GC 清理]
```

### 3.2.2 MovePeer 三步时序（调度器驱动）

```
Scheduler: size(store_A) >> size(store_B) → 选 region R → 下发序列
 1) propose AddLearner(R, peer@B)   → apply → B 以 Learner 身份追日志
 2) Learner 追平(add_voter 门槛)    → propose AddVoter(R, peer@B) → apply → B 成 voter
 3) propose RemoveNode(R, peer@A旧) → apply → A 摘除 → destroy → 调度器更新视图
任一步失败/超时 → 整个 MovePeer 中止并回报（序列幂等：重复执行以 epoch 判定去重）
```

## 3.3 接口设计

### 3.3.1 raftstore 侧

```cpp
namespace tinykv::raftstore {
struct RegionEpoch { uint64_t conf_ver; uint64_t version; };
struct Region { RegionId id; std::string start_key, end_key;   // 编码后 key，空串=无限
                std::vector<PeerInfo> peers; RegionEpoch epoch; };

enum class AdminCmd { Split, TransferLeader, AddLearner, AddVoter, RemoveNode, CompactLog };
struct AdminArgs { std::string split_key; PeerId target; PeerInfo new_peer; };

class Peer {
public:
    result<void> propose(NormalRequest req);        // 校验 epoch/leader → 提案 → callback
    result<void> propose_admin(AdminCmd cmd, AdminArgs args);
    result<void> apply_snapshot_chunk(SnapshotChunk c);   // 安装前必须 is_initialized() 判断
    bool is_initialized() const;
};
class StoreMeta {   // RWMutex 保护；regionRanges = btree<end_key, RegionId>
public:
    std::optional<Region> find_region(std::string_view key) const;      // 路由
    result<void> replace_region(Region old, Region new_left, Region new_right); // Split 原子
    result<void> on_conf_change(RegionId, ConfChangeView);              // apply 时同步
};
}
```

### 3.3.2 调度器 proto（PD-lite）

```protobuf
// scheduler.proto —— 契约变更先与用户核对（CLAUDE.md §15）
service Scheduler {
  rpc StoreHeartbeat(StoreHeartbeatReq{store_id, region_count, total_size, capacity}) returns (Ack);
  rpc RegionHeartbeat(RegionHeartbeatReq{Region meta}) returns (Ack);
  rpc AskSplit(AskSplitReq{region_id, split_key}) returns (AskSplitResp{new_region_id, new_peer_ids});
  // MovePeer 不做独立 RPC：调度器经 RaftStore 的 Admin 提案接口下发三步序列
}
```

### 3.3.3 错误矩阵（本阶段新增触发点）

| 变体 | 触发点 | hint 语义 |
|---|---|---|
| StaleEpoch | 请求/提案携带 epoch 落后（split 或 conf change 已发生） | have/want 字段供客户端定向刷新路由 |
| KeyNotInRegion | key 越出所到 region 范围（路由过期或刚 split） | region_id 供回源重查 |
| NotLeader | 路由到非 leader（Transfer 后常见） | leader_hint 重定向 |
| InvalidArgument | split_key ∈ {start,end}（开区间外）、AddVoter 目标已是 voter | 按字段修正 |

## 3.4 核心逻辑骨架

```
split-check(store 周期任务): 对每个 region 按序扫描估计 size/keys（读 SizeDiffHint 加速）
  → 超 threshold → 选中点 split_key → AskSplit(调度器分配新 id) → propose_admin(Split)
apply Split: 校验 epoch 新 + split_key ∈ (start,end) 开区间 → clone 元数据切 [split_key,end) 为新 region
  → 新 region 建 peer 注册路由 → 双写 regionRanges（btree + regions map 同事务）→ 立即 notify 调度器
Put/Delete apply 时: 维护 SizeDiffHint（白皮书 Delete 阶段卡死问题的解法）

单步成员变更: propose(AddLearner) → apply 同步 ApplyConfChange → Learner 追平(voter 门槛)
  → propose(AddVoter) → … → propose(RemoveNode)；被移除者是自己 → destroy peer(通知 store 收尾)
  【两节点集群移除 Leader 边界: 先 TransferLeader 再 RemoveNode + 保底心跳推进】
Leader Transfer: propose_admin(TransferLeader{to}) → 目标日志补齐 → 发 TimeoutNow
  → 转移期间拒绝新提案；一个 election timeout 未完成 → 放弃(提案报错，客户端重试)
快照安装: 收齐分块校验 → 【is_initialized() 判断：未初始化才新建，已初始化走重装清理】→ 原子替换
```

## 3.5 红线（白皮书疑难杂症逐条对应）

1. Split 提案必须校验：epoch 最新 且 split_key ∈ (start, end) 开区间；
2. Split apply 完成后**立即** notify 调度器（防"no region"路由空洞）；
3. Put/Delete apply 维护 SizeDiffHint，防 split-check 对海量 Delete 卡死；
4. 安装快照前必须 `is_initialized()` 判断（白皮书 1/15 概率 BUG 根因）；
5. 两节点集群移除 Leader：先 TransferLeader 再 RemoveNode + 保底心跳推进；
6. Transfer 期间拒绝新提案（客户端收错重试），一个 election timeout 未完成即放弃；
7. 任意时刻每 key 恰好属一个 Region（REL-05 不变量，测试直接断言）；
8. stale epoch 请求拒绝并携带 hint（REL-05 + UX-03）；
9. 调度器只动元数据，数据面（快照传输）走独立通道（控制面/数据面分离）。

## 3.6 示例

### Split 并发读写模拟用例（`sim_split_test` 形态）

```cpp
// 写入 key∈[0,1M) 同时周期注入 split(a..z 前缀段)；读写客户端持续验证:
//   断言 A: 任意时刻 get(k) 满足线性一致（最后确认提交的写可见）
//   断言 B: 结束后 region 集合连续无缝隙: 按 end_key 排序首尾相接且无重叠
//   断言 C: epoch 过旧的请求 100% 被拒且带 StaleEpoch{have,want}
```

### propose_admin 代码示例（store 侧）

```cpp
auto r = peer->propose_admin(AdminCmd::Split, {.split_key = encode_user("m")});
if (!r) {
  if (auto* se = std::get_if<tinykv::StaleEpoch>(&r.error()))
      router.refresh(se->region_id, se->want);        // hint 驱动定向刷新
  else if (std::holds_alternative<tinykv::NotLeader>(r.error()))
      retry_on_hint(...);                              // leader_hint 重定向
}
```

### CLI 观测（UX-04）

`tinykv-cli status` 一屏显示每 region：range / leader / term / commit / apply lag / size hint；
`tinykv-cli trace region 7 on` 开启 region 级 trace 日志（随机 BUG 复现靠日志考古）。

### 配置项表（scheduler.yaml / store 增量项）

| 配置项 | 默认值 | 影响 |
|---|---|---|
| `store.split_check_interval_s` | 30 | split-check 周期 |
| `store.region_size_threshold_mb` | 96 | 触发 AskSplit 的 size 阈值 |
| `scheduler.balance_trigger_diff` | 0.2 | size 差 >20% 触发 MovePeer |
| `scheduler.move_peer_rate_limit` | 1/min | 迁移限速 |
| `scheduler.store_heartbeat_s / region_heartbeat_s` | 10 / 30 | 超时=3× |
| `snapshot.chunk_mb` | 4 | 分块大小，流内序号+CRC |

## 3.7 验收与证据链

| 需求 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| FUNC-07/REL-05 | 路由含 epoch 校验；每 key 恰好一 region（不变量断言） | `sim_region_route_test` | `ctest -L fast` | junit.xml |
| FUNC-08 | 1M key 写入过程中连续 split：断言 A/B/C 全过 | `sim_split_test`（多种子） | `ctest -L fast` | junit.xml + 种子 |
| FUNC-09/REL-04 | 单步变更期间读写不丢已提交数据；learner 追平后才投票；两节点移除 leader 边界成立 | `sim_confchange_test` | `ctest -L slow` | junit.xml |
| FUNC-10 | Transfer 不丢提案；超时放弃路径 | `sim_transfer_test` | `ctest -L fast` | junit.xml |
| FUNC-11/REL-03 | 慢节点经快照追赶成功；安装/清理无窗口错乱（含未初始化 peer 重装） | `sim_snapshot_test` | `ctest -L slow` | junit.xml |
| FUNC-12 | PD-lite 心跳收集 + size 均衡收敛（≤N 轮均衡完成）；AskSplit 契约 | `sim_scheduler_test` | `ctest -L slow` | junit.xml |
| ENG-05 | 150 轮 chaos **含 split+confchange 叠加注入**无 FAIL | `mr_chaos_n150`（nightly） | `ctest -R mr_chaos_n150` | `evidence/nightly/mr_chaos_150.txt` |
| PERF-07 | 1M key 均匀分布 split 完成 ≤60s(S0)/≤30s(S1) | `split_bench` | 夜跑 | `bench/split.json` |
| PERF-08 | Multi-Raft 下心跳带宽仍达标（region×peer 规模化统计） | `heartbeat_bandwidth_bench` | 夜跑 | `bench/heartbeat.json` |
| PERF-09 | 快照传输 ≥10MB/s(S0)/≥50MB/s(S1) | `snapshot_bench` | 夜跑 | `bench/snapshot.json` |
| UX-04 | 排障演练：人为注入一个 BUG，仅凭 CLI status+日志定位 | —（人工演练） | 演练记录 | `docs/notes-m4-troubleshoot.md` |
| UX-06 | M4 实现笔记一篇（P3 踩坑记录） | —（人工） | — | `docs/notes-m4.md` |

**M4 完成判定** = 上表 S0 全达 + 150 轮叠加 chaos 报告 + 排障演练记录 + `notes-m4.md`。

## 3.8 开放决策点

| # | 决策 | 建议默认 |
|---|---|---|
| D3-1 | split-check 周期 / size 阈值 | 30s / 96MB |
| D3-2 | 调度器均衡策略 | size 差 > 20% 触发 MovePeer，限速 1 迁移/分钟 |
| D3-3 | 快照分块大小 | 4MB，流内序号+CRC |
| D3-4 | 心跳间隔 | store 心跳 10s；region 心跳 30s（调度器侧超时 3×） |
| D3-5 | Learner 追平门槛（AddVoter 前置） | 日志差 < 100 entry 且持续 5s |
