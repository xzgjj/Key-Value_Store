# M5 实现笔记（notes-m5）

> UX-06 交付物，对应 SPEC-4 §3.7 / 分支 `4`（M5 Percolator 事务：①错误契约 → ②TSO+Latch →
> ③MvccTxn → ④SI语义+决议 → ⑤经raft集成 → ⑥CLI+审计+chaos → ⑦本笔记）。

## 1. 做了什么（按层，一行一句 + 证据）

| 层 | 实现 | 证据 |
|---|---|---|
| ① | 错误契约扩展：KvError 6→9 变体（WriteConflict/LockConflict/Retryable）+ RegionError 对称 oneof；TsNotMonotonic 走 fatal 异常 | `M5-TXNERROR-DRAFT.md`；align 对账对称（不进豁免） |
| ② | TSO 三件套（Provider/Monotonic/Checked fatal 装饰器）+ 调度器兼任适配；Latch key 全序加锁 | `tso_test` 6 例 + `latch_concurrency_test` 4 例 |
| ③ | MvccTxn：三列族布局（Default@start_ts / Lock@key / Write@commit_ts），prewrite/commit/rollback/快照读/决议三态 | `mvcc_test` 9 例 |
| ④ | TxnClient（client 驱动 2PC）+ 决议编排三态 + resolve_locks_of | `si_semantics_test` 6 例 + `txn_resolve_test` 2 例 |
| ⑤ | TxnExecutor 双端（Local/Peer）+ Peer txn 路径（[0x2B] 载荷 → Latch → MvccTxn → 共识） | `txn_peer_test` 3 例 |
| ⑥ | CLI txn（§11 契约）+ lincheck_si 审计 + txn_chaos_n150 | `cli_txn_smoke` 4 例 + 审计 5 种子 + chaos 150/150 |
| ⑦ | 本笔记 + 证据表核对 | 本文件 |

## 2. 三条贯穿 M5 的设计主线

- **生死只由 primary 决定（红线2）**：MvccTxn 是纯机械件；check_txn_status(primary) 是唯一
  决议入口；resolve_locks_of 只处理 primary+start_ts 双匹配的锁——secondary 不得自行判定。
- **决议编排全在 client（红线8）**：server 只保证单 key 原子性（Latch）+ MVCC 机械；
  跨 region 协调、锁等待重试、冲突上报全部在 TxnClient。
- **确定性时钟**：TTL 判定显式注入 now_ms（不读墙钟），chaos/决议测试零真实等待——
  与 ④b 追平门槛的 tick 取向同源，模拟可复现。

## 3. 踩坑记录（P3）

1. **Engine 自带 CF 前缀**：事务键 = user_key + 大端 ts 后缀（CF 前缀归引擎）——
   再拼一次 encode_ts_key 就是双重前缀，什么也查不到（③层首跑全挂）。
2. **be64_str 字节序写反**：读出 110×2^56——测试输出出现规整 2^n 倍数时优先怀疑字节序。
3. **版本归属判定必须等值**：剥 8B ts 尾后按用户 key 精确比较——starts_with 会把 "k1"
   的版本误配给 "k"。
4. **TSO 占号不消费**：`current()+1` 会被后续 begin() 撞出同 start_ts——一律真实分配。
5. **begin() 清空 writes_**：先 set 后 begin = 空事务。
6. **client 持执行端引用**：Fixture 每次重建执行端 → 先前 client 悬垂（win-debug 不炸、
   CI 构建必炸）——持引用对象只建一次。
7. **读己之写末写胜出**：get 取首写而 prewrite 落盘末写，两者不一致——lincheck 审计器捕获。
8. **崩溃级联决议**：崩溃事务的 prewrite 可能撞上此前崩溃事务的未过期锁——按编排先决议
   再重试（chaos 首轮 57/150 失败全是这个未覆盖场景）。

## 4. 验收对照（SPEC-4 §3.7 逐行）

| 需求 | 测试登记名（实际） | 状态 | 说明 |
|---|---|---|---|
| FUNC-13/REL-06 | `mvcc_test` + `si_semantics_test`（SPEC 名 txn_prewrite_test/txn_commit_test） | ✅ | 冲突矩阵全分支；机制与语义分两层证明 |
| FUNC-14 | `txn_resolve_test`（模拟器） | ✅ | 崩溃 TTL 决议 + 正向推进；注入时钟零真实等待 |
| FUNC-15 | `latch_concurrency_test`（SPEC 标 slow，实际 fast） | ✅ | 同 key 串行观察点 + 交叉 key 无死锁；注入时钟后 fast 可判定 |
| FUNC-16 | `tso_test` | ✅ | 严格单调 + fatal 路径 + 1e5 ts/s 吞吐 |
| 集成 | `txn_chaos_n150` | ✅ | 150/150 种子；崩溃级联决议；三副本一致 |
| REL-06(端到端) | `lincheck_si_test` | ✅ | 外部审计器三条不变量，5 种子随机历史 |
| UX-02(txn) | `cli_txn_smoke` | ✅ | --help + demo（交互式随 daemon 化，docs/cli.md §11） |
| UX-06 | 本文件 | ✅ | — |
| FUNC-17(P2) | `gc_test` | ❌ 未建 | P2 stretch，挂账 |

**结论**：P0 行证据齐备（FUNC-17 为 P2 挂账）；SPEC 建议名与实际登记名的差异均为
"拆分/合并"级映射（登记名以 testing.md §4 为唯一事实来源）。

## 5. 挂账与已知限制（需要用户决策/后续迭代）

- **GC（FUNC-17，P2）**：安全点以下旧版本清理未实现（含活跃 ts 上报前置）。
- **CLI 交互式事务**：一次性进程无法跨调用持事务状态；交互会话与 `txn status` 查询随
  daemon 化与长连接客户端接入（docs/cli.md §11）。
- **读路径线性一致**：txn 的 get/check_txn_status 走 leader 本地直读（M2 leader 本地读
  同款偏差）；与 ReadIndex 结合属后续一致性收口。
- **契约待追认**：M5-TXNERROR-DRAFT.md（三变体字段集、TsNotMonotonic 是否入 result 通道）。

## 6. 数字

- 测试：ctest 54/54（unit 32 / simulation 4 / integration 8 / bench 7 + cli smoke）；
  M5 新增登记 11 个测试文件。
- review：M5 共 18 轮（每子层 3 轮），全部 PASS；align 3/3 每层复核。
- CI：13 项门禁每层全绿（win-debug 与 build/ci 双目录）；复杂度超限 2 处回修。
- 提交（分支 4，基于分支 3 @ e252bcd）：672b139 ①② → 60decb8 ③ → e05af63 ④ →
  5352cbd ⑤ → df60414 ⑥ → ⑦（本笔记随提交）。
