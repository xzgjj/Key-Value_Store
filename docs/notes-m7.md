# M7 最终交付说明（多 Region 可用性、元数据真相、并发与安全边界）

> 对应 SPEC-6（`docs/SPEC-6-m7-evolution.md`）。本文是里程碑收尾笔记：交付了什么、为什么这么做、
> 踩了什么坑。过程细节见 `docs/review-log.md` 与 `notes.txt` 断点。

## 1. 交付内容

| 条目 | 实现 | 证据 |
|---|---|---|
| ⑦ 跨区事务 | ⑦-0 护栏（`guard_single_region`）→ `region_grouper`（纯函数分组）→ `TxnClient` 分组 2PC（PREPARE → TSO → COMMIT_PRIMARY → COMMIT_SECONDARY）→ 恢复（`resolve_locks_of` 广播到各 region）；`txn.cross_region_enabled` **默认 true（已转正）** | `txn_cross_region_test` 3 例、`txn_cross_region_chaos` 150 轮（枚举三窗口）、`txn_cross_region_chaos_random` 150 轮（随机 kill，真崩溃语义） |
| ⑧ 元数据持久化 | `StoreMetaStore`：`meta/regions.pb` + `.bak` + CRC32C；启动按 meta 重建 peer；split 后同批落盘 | `split_meta_persistence_test`（split → 重启 → 拓扑/右半数据/再 split 三断言） |
| ⑨a 锁分片 | `LockRegistry` 接入组合根：PeerGate 分区闸（单 region 操作只锁所属桶）/ 全局闸（跨 region 操作锁全部桶，升序防死锁）；`server.lock_shards` 默认 16；多副本强制退化一把大锁 | `lock_registry_test` 4 例 + `lock_shards_wiring_test` 3 例 + `region_concurrency_bench`（比值 1.04，见 §2 决策 3） |
| ⑩ 安全边界 | Bearer token 鉴权（常量时间比较、401 不回显）+ 未鉴权强制绑回环 + `/status` 暴露 `auth`/`txn_cross_region`（红线 6）；**TLS 移出 M7**（D-2 裁决） | `auth_test`、`admin_endpoints_test` |
| ⑨c 拆 God class | 随改动顺带：`start_transport_locked`/`start_http_locked`/`compact_log_if_needed`/Cursor 归并等抽取 | CI static（2158 个函数全在行数/复杂度上限内） |

M7 完成判定（SPEC-6 §6.7）：⑦⑧ 的 P0 证据齐备 + chaos 150 轮无 FAIL（枚举 + 随机两套）+ 本文。
**达成**（⑨b 共享引擎按 D6-2 移出，触发条件见 `implementation-notes.md`）。

## 2. 关键决策

**1）随机 chaos 用真崩溃语义，不满足于进程内枚举。** 枚举版 chaos 命中率 100% 但只覆盖
"已知的三个窗口"；`txn_cross_region_chaos_random` 用独立子进程执行 2PC 前 N 步后 `_Exit(0)`
（等价 kill -9，同 crash_writer 的论证：进程内关开走析构，验证不到 WAL 恢复），死亡点 0..4
均匀采样 × 主组侧随机 × 1/3 轮注入"半途决议"，种子=基数+轮次可精确复现。这是开关转正的前提。

**2）"转正"由证据驱动，不由时间驱动。** `txn.cross_region_enabled` 默认 false 保持到随机
chaos 通过，经用户批准后翻 true；显式配 false 即完全回退。开关状态进 `/status`
（红线 6：`txn_cross_region` 字段，此前缺失本批补齐）。

**3）分片闸"一次只放开一个自由度"。** 先把 ⑦ 协议语义在全局闸下跑对，再换闸；换闸时
多副本模式强制退化（泵触碰全部 peer + transport，分片零收益），`shards=1` 保留回退档。
诚实结论：`region_concurrency_bench` 实测跨区吞吐与改前持平（比值 1.04/0.98 两轮）——
**下一个瓶颈是 StatusServer 的单 io_context 线程**（所有 HTTP 处理在一个线程串行执行），
分片的吞吐收益要等 HTTP 层多线程化才能显形。M7-PERF-01 的验收口径是"不低于改前"，达成。

**4）锁序全链固定**：region 桶（升序）→ registry mu_ → Engine mu_ / latch / session mu_ / tso mu_。
registry 的 `slots_` 表加内部锁（split 动态建 slot 与路由读并发）；StoreMeta 自带 RWMutex，
无需第二把共享闸。多副本泵路径仍走全局闸，永不与分区操作交错。

## 3. 踩坑（后来者先读这里）

1. **随机 chaos 的"目录与主组侧错位"**：子进程签名是 `<主组目录> <副组目录>`，首版测试把
   主组 key 永远传给 dir1、断言却按随机布局期望 primary 在 dir2——kill≥3（主组已提交）的轮次
   全部失败。这正是回归测试的价值：修复前红、修复后绿，写进了提交说明。
2. **HTTP 单线程是吞吐天花板**：StatusServer 单 io_context 线程串行执行全部 handler，
   客户端多少线程都没用。诊断方法：换闸前后吞吐不动（≈1.0），即可断定瓶颈在闸之外。
3. **gtest 宏里的花括号陷阱**：`ASSERT_EQ(d.cli({"a", &got}, 0), 0)` 这类"花括号初始化 +
   输出指针混在宏参数里"的写法会踩 gtest 空指针字面量检测的展开问题；`run_cli` 收
   `span<const string>` 时花括号初始化不推导。两条都绕开：先取返回码再断言 / 显式建 vector。
4. **include 顺序即宏顺序**：`raft_test_base.h` 经 asio 引入 Windows `DELETE` 宏，
   展开状态下破坏 kvpb 枚举解析——共享头 `server_daemon.h` 里 `server_app.h` 必须先于它。
5. **工作区外部干扰**：本批期间另一会话把根目录 `SPEC.md`、`TinyKV-CPP-设计思路与需求.md`
   移入 `docs/`（相对链接全断）并落入 `.slidep/`、`年终总结/`、`.cache/` 等未跟踪目录。
   处置：文件移回原位（consistency 42 链接恢复全绿）；外来目录加 `.gitignore` 不动内容。

## 4. 未决与移交

| 项 | 状态 | 去向 |
|---|---|---|
| TLS 传输装饰器 | D-2 裁决移出 M7 | 另立项（新依赖 + 证书生命周期） |
| HTTP 层多线程化 | 分片吞吐显形的前提 | 另立波次（handler 已具备并发安全的闸基础） |
| ⑨b 共享引擎单例化 | D6-2 移出 | 等"每 store region 数 vs 句柄/内存"bench 曲线 |
| 多 region × 多 voter | split 显式拒绝（路由歧义） | 等 raftpb::Message 携带 region_id |
| 随机 chaos 夜跑常态化 | 已登记 nightly;serial | 夜跑 CI 自动执行 |
