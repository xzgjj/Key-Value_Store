# 审查台账（review-log）

> 每阶段收尾的"三轮检查"记录在此，随提交入库、跨会话可查。
> 写入走 `python tools/review_rounds.py record`，门禁走 `gate`；**不要手工改标记区**，
> 表格列宽由工具维护，手改会破坏解析。
> 每轮设不同重点：可判定性 → 契约与复用 → 文档与红线（见 AGENT_EXECUTION_PROTOCOL.md §8）。

## 轮次记录

<!-- review:begin v1 -->
| 阶段 | 轮次 | 重点 | 结论 | 备注 |
|---|---|---|---|---|
| M0 | 1 | 可判定性：CI 全阶段 + 单元测试 | PASS | run_ci PASS×13；ctest 5/5 |
| M0 | 2 | 契约与复用：对齐检查 + 登记对账 + 检查器自测 | PASS | align 三项全 PASS；registry PASS；selftest PASS |
| M0 | 3 | 文档与红线：预算/查重/链接 + scope/guard + SPEC-0 红线抽查 | PASS | docs PASS×4；scope/guard PASS；src 无空目录、StrongId 无隐式转换 |
| M1 | 1 | 可判定性：CI 全阶段 + 全部测试 | PASS | run_ci PASS×13；ctest 15/15（含 bench） |
| M1 | 2 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 检查器自测 | PASS | align 3/3；registry PASS；bugfix PASS；selftest PASS |
| M1 | 3 | 文档与红线：预算/查重/链接 + guard + 恢复路径不改写/无 detach 抽查 | PASS | docs PASS×4；guard PASS；src 无空目录；恢复路径无 truncate |
| M2 | 1 | 可判定性：CI 全阶段 + 19 个测试可执行 | PASS | run_ci PASS×13；ctest 19/19 |
| M2 | 2 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 自测 | PASS | align 3/3；registry/bugfix/selftest 全 PASS |
| M2 | 3 | 文档与红线：预算/查重/链接 + guard + 心跳不带日志/无空目录 | PASS | docs PASS×4；guard PASS；红线抽查通过 |
| M2 | 4 | 可判定性：CI 全阶段 + 22 个测试可执行 + 150 轮长跑 | PASS | run_ci PASS×13；ctest 22/22（含 raft_chaos_n150） |
| M2 | 5 | 契约与复用：对齐三项 + 登记对账 + 缺陷门禁 + 自测 | PASS | align 3/3；registry/bugfix/selftest 全 PASS |
| M2 | 6 | 文档与红线：预算/查重/链接 + guard + 心跳不带日志抽查 | PASS | docs PASS×4；guard PASS；无空目录 |
| M3 | 1 | 可判定性：CI 全阶段 + 23 个测试可执行 | PASS | run_ci PASS×13；ctest 23/23（含 readindex_test） |
| M3 | 2 | 契约与复用：对齐三项 + 登记对账 + 自测 | PASS | align 3/3；registry/selftest PASS |
| M3 | 3 | 文档与红线：预算/查重/链接 + guard + 租约默认关抽查 | PASS | docs PASS×4；guard 无改动；lease 默认 false |
| M3 | 4 | 可判定性：CI 全阶段 + 26 个测试 + 三模式基准 | PASS | run_ci PASS×13；ctest 26/26；read_bench 58.7x/3652.8x |
| M3 | 5 | 契约与复用：对齐三项 + 登记对账 + 自测 | PASS | align 3/3；registry/selftest PASS |
| M3 | 6 | 文档与红线：预算/查重/链接 + guard + 租约默认关 | PASS | docs PASS×4；guard PASS |
| M2 | 7 | 可判定性：29 个测试 + 四项欠账证据 | PASS | ctest 29/29；CI PASS×13；snapshot/crash/bench/cluster-write 全绿 |
| M2 | 8 | 契约与复用：对齐三项 + 登记对账 | PASS | align 3/3；registry PASS |
| M2 | 9 | 文档与红线：预算/查重/链接 + guard | PASS | docs PASS×4；guard PASS |
| M2 | 10 | 可判定性：30 个测试 + 真 TCP 压测驱动 | PASS | ctest 30/30；CI PASS×13；tcp_perf_bench A/B/C 三阶段出数 |
| M2 | 11 | 契约与复用：对齐三项 + 登记对账 | PASS | align 3/3；registry PASS |
| M2 | 12 | 文档与红线：预算/查重/链接 + guard | PASS | docs PASS×4；guard PASS |
| M2 | 13 | A-group commit 可判定性：CI 13 项 + wal_group_commit 3 例 + crash/wal 回归 | PASS | run_ci PASS×13；ctest 31/31（含 wal_group_commit 3 例、crash_recovery、raft_chaos_n150）；单机 write_bench 3184→74728 QPS |
| M2 | 14 | A-group commit 契约与复用：对齐三项 + 登记对账 + 延迟窗口语义核对 | PASS | align 3/3；registry PASS（wal_group_commit_test 已登记）；behavior_change_approved 留痕；proto 契约未改 |
| M2 | 15 | A-group commit 文档与红线：预算/查重/链接 + guard + 打开即持久语义回归 | PASS | docs PASS×4；guard PASS；crash_recovery_test 绿证明进程级崩溃不丢；notes-m2 §8.1 如实记 S0 未达标 |
| M4 | 1 | ①可判定性：CI 13 项 + entry_batch 5 例 + 全量 32 测试 + PERF-01 达标 | PASS | run_ci PASS×13；ctest 32/32；entry_batch_test 5/5；管道化 batch=1 fsync=on 28433/s > S0 2000 |
| M4 | 2 | ①契约与复用：proto 未改 + 对齐三项 + 回执不串/去重/非leader拒绝回归 | PASS | align 3/3；registry PASS；proto 零改动；raft_kv_test 非leader拒绝回归经全量测试抓出并修复 |
| M4 | 3 | ①文档与红线：预算/查重/链接 + scope/guard + Envelope 向后兼容/无 detach | PASS | docs PASS×4；guard PASS；try_unpack 兼容旧 data；committer 无 detach；banned 0 命中 |
| M4 | 4 | ②可判定性：CI 13 项 + region_route 8 例 + 全量 33 测试 | PASS | run_ci PASS×13；ctest 33/33；region_route 8/8（路由/epoch/split原子/并发读/重叠拒绝） |
| M4 | 5 | ②契约与复用：复用 RegionEpoch/RegionId/错误变体 + 对齐三项 | PASS | align 3/3；registry PASS；复用 error.h 的 epoch 双判定与 6 变体错误；std::map 不引新库（语言机制优先） |
| M4 | 6 | ②文档与红线：预算/查重/链接 + scope/guard + REL-05不变量 | PASS | docs PASS×4；guard PASS；每 key 恰好一 region 由区间索引+重叠拒绝双保险；shared_mutex 并发安全 |
| M4 | 7 | ③可判定性：CI 13 + region_route 11 + split 3 + 全量 34 | PASS | ctest 34/34；split 端到端 3/3（切分/边界拒绝/数据保留）；run_ci PASS×13 |
| M4 | 8 | ③契约与复用：复用 SplitRequest proto + epoch + 对齐三项 | PASS | align 3/3；proto 复用现有 SplitRequest 未改；split_region 纯函数复用 RegionEpoch 推进 |
| M4 | 9 | ③文档与红线：预算/查重/链接 + scope/guard + 红线1/2 | PASS | docs/guard PASS；红线1开区间校验+红线2 apply后立即更新路由经 split_test 证明 |
| M4 | 10 | ④a可判定性：CI 13 + confchange 5 + 全量 35 | PASS | ctest 35/35；confchange 5/5（AddLearner/AddVoter/RemoveNode/落盘type/非leader拒绝）；run_ci PASS×13 |
| M4 | 11 | ④a契约与复用：Entry.type proto 已核对 + 复用 Cluster fixture + 对齐三项 | PASS | align 3/3；Entry.type/ConfChange proto 用户核对通过(M4-CONFCHANGE-DRAFT)；复用 add_node 扩展 fixture |
| M4 | 12 | ④a文档与红线：预算/查重/链接 + scope/guard + apply时才生效 | PASS | docs/guard PASS；ConfChange 在 collect_apply 时生效，幂等可重建；已知遗留(learner追平门槛/两节点移除边界)留④b |
| M4 | 13 | ④b可判定性：CI13项 + sim_confchange 3例 + 全量36 + confchange追平升级 | PASS | run_ci PASS×13；ctest 36/36；sim_confchange 3/3（未追平拒绝/两节点移除leader拒绝/三节点移除允许）；confchange AddVoterPromotes 升级为真实追平后提升 |
| M4 | 14 | ④b契约与复用：proto未改 + 对齐三项 + 登记对账 + 门槛语义核对 | PASS | align 3/3（contract/doc_cmd/banned）；registry 36/36；proto未改；InvalidArgument复用现成变体未扩契约；D3-5=差<100持续5s按tick可复现 |
| M4 | 15 | ④b文档与红线：预算/查重/链接 + scope/guard + 红线5与learner不计majority | PASS | docs PASS×4；guard PASS；scope 278行/6文件在预算；红线5两节点移除leader守卫经用例证明；learner回执不调advance_commit不污染多数派 |
| M4 | 16 | ⑤可判定性：CI13 + snapshot_chunk 2例 + 全量绿 | PASS | run_ci PASS×13；ctest 全绿；snapshot_chunk 2/2（多块追平且数据落盘/坏块CRC拒绝）；M2 snapshot_test 兼容绿 |
| M4 | 17 | ⑤契约与复用：SnapshotChunk proto落版 + 复用 install_snapshot/crc32c | PASS | align 3/3；proto 按 SPEC D3-3 默认(4MB+seq+CRC)；复用 crc32c/install_snapshot/restore_snapshot；MSG_SNAPSHOT单块保留兼容 |
| M4 | 18 | ⑤文档与红线：分块CRC不装脏数据 + scope/guard | PASS | docs PASS；guard PASS；坏块Corrupted拒绝不安装(红线)；窗口流控防内存打爆；begin不重置防卡死 |
| M4 | 19 | ⑥可判定性：CI13项 + sim_scheduler 4例 + 全量38 | PASS | run_ci PASS×13；ctest 38/38；sim_scheduler 4/4（心跳汇总/AskSplit分配/三轮单调收敛/限速1迁移） |
| M4 | 20 | ⑥契约与复用：scheduler.proto 未改 + AskSplit不撞号(本轮修复) + 复用RegionEpoch | PASS | align 3/3(contract/doc_cmd/banned)；proto/scheduler.proto 保持用户核对版未动；复用 common. RegionEpoch 未新造；review 发现 next_region_id_ 从0起算会与已见region撞号→改为心跳抬升基线+新peer对齐已见max，用例 EXPECT_GT(new_region_id,5) 固化 |
| M4 | 21 | ⑥文档与红线：控制面/数据面分离 + 限速 + docs/guard/scope | PASS | docs PASS×4；guard PASS；scope 已声明含 src/scheduler；调度器只产出 MovePeer 建议、不直接改数据不走数据通道(M-1)；限速 max_moves_per_round=1 由 BalanceRespectsRateLimit 证明；size 汇总只信 store 心跳，删除region回填兜底(会把空store算成有量) |
| M4 | 22 | ⑦a可判定性：CI13 + transfer_leader 4例 + sim_transfer 3例 + 全量40 | PASS | run_ci PASS×13；ctest 40/40；transfer_leader 4/4（转移成功/转移期拒提案/超时放弃/非法目标）；sim_transfer 3/3（不丢提案/拒绝可重试/放弃后继续服务）；变异检查2次：去campaign→2例红、放宽deadline→1例红，用例咬得住实现 |
| M4 | 23 | ⑦a契约与复用：MSG_TIMEOUT_NOW 追加式枚举 + TransferLeaderRequest 零改动 + 对齐三项 | PASS | align 3/3；raft.proto 仅追加 MSG_TIMEOUT_NOW=12（七字段复用，不新增 Message 字段）；TransferLeaderRequest 用既有契约零改动；M4-TRANSFERLEADER-DRAFT 留痕待追认；不落日志（易失意图，与etcd leadTransferee 同构）；复用 campaign/handle_vote_request 现成路径 |
| M4 | 24 | ⑦a文档与红线：红线6三条逐条证明 + scope/guard/docs | PASS | docs PASS；guard PASS；scope 已补声明(CMakeLists/测试/草稿)；红线6：转移期拒提案(例2)、一个election timeout放弃(例3)、目标追平才发TimeoutNow(maybe_send_timeout_now)；once-notified防重复抬term；测试修正2处场景假设(TimeoutNow会被flush送达/红线4需当前term条目)均非实现缺陷；登记 40⇄40 |
| M4 | 25 | ⑦b可判定性：CI13 + cli_region 6例 + 全量41 | PASS | run_ci PASS×13；ctest 41/41；cli_region 6/6（regions列表+split后两region/详情+peers/开区间外拒/scheduler视图/trace开关落盘/退出码1-3-2契约）；重构后 cmd_cluster 拆四个子函数、serve 拆 parse_request_line+handle_get+respond，复杂度全部回到限内 |
| M4 | 26 | ⑦b契约与复用：§10 六命令落版 + AdminHandler 端点表留痕 + trace偏差待追认 | PASS | align 3/3；docs/M4-CLI-TRACE-DRAFT.md 留痕（trace 补 --endpoint 的偏差与 2 项待追认）；region split 服务端链路 AskSplit→admin提案→共识→apply→路由更新全部复用③⑥现成机制；RegionTrace 复用 spdlog rotating 与全局日志同规格（10MB×5） |
| M4 | 27 | ⑦b文档与红线：红线1经CLI路径再证明 + docs/guard/scope | PASS | docs PASS；guard PASS；scope 补声明 region_trace.h 与 CLI-TRACE 草稿；region split 'zz' 开区间外被 apply 拒（红线1 的 CLI 端到端证据）；cli.md §10 状态行更新为已交付；已知限制(trace埋点未接Peer内部)记草稿§4移交notes-m4 |
| M4 | 28 | ⑦c可判定性：CI13 + mr_chaos_n150 150轮 + 全量42 | PASS | run_ci PASS×13；ctest 42/42；mr_chaos_n150 150/150 种子（~10.5s）；变异检查：去 split 注入→150轮全红（region_count!=2），还原后绿 |
| M4 | 29 | ⑦c契约与复用：复用 PeerCluster/enable_split 每peer独立StoreMeta + 对齐三项 | PASS | align 3/3；peer_cluster.h 加 enable_split（每 peer 一份 StoreMeta：真实拓扑各 store 各持视图，共享单视图会让后apply的peer撞region不存在且apply不重试）；confchange 复用 raft().propose_conf_change + raft级apply；split 复用③层 admin 提案路径 |
| M4 | 30 | ⑦c文档与红线：REL-05 经150轮混沌证明 + CI复杂度/重复回修 | PASS | docs PASS；guard PASS；REL-05 断言形式化：区间排序后首尾相接无重叠（contiguous函数）且三副本指纹一致；分区随机注入20%/拍；run_round 拆 inject/converged/mr_invariants 三助手（复杂度26→≤15）；include 序言撞车以 <utility> 打断 |
| M4 | 31 | ⑦d可判定性：UX-04演练真注入+CLI定位 + 全量42 + CI13 | PASS | 注入 peer.cpp execute split 短路缺陷→cli_region 红（regions 无 id2/epoch 未推进）→还原→绿；演练记录 docs/notes-m4-troubleshoot.md；build/evidence/nightly/mr_chaos_150.txt 生成；ctest 42/42；run_ci PASS×13 |
| M4 | 32 | ⑦d验收对照：§3.7 逐行核对（9✅/3挂账/2部分） | PASS | notes-m4 §4 逐行表：FUNC-07~12/ENG-05/UX-04/UX-06 全✅；FUNC-08 部分（1M规模挂PERF-07）；PERF-07/08/09 bench 未建如实挂账（§5 待用户决策）；SPEC 建议名↔实际登记名映射注明；登记名以 testing.md §4 为唯一事实来源 |
| M4 | 33 | ⑦d文档收尾：notes-m4 UX-06 + guard/scope/docs + 契约待追认汇总 | PASS | docs PASS；guard PASS；scope 补 notes-m4-troubleshoot；notes-m4 含 P3 踩坑 6 条与 4 份契约草稿待追认清单；M4 30+3 轮 review 全 PASS |
| M4 | 34 | bench补齐①：split_bench 1M key 6.3s + 心跳不变量 + 快照 11MB/s，全量45 | PASS | ctest 45/45；CI PASS×13；split 6.3s（S0≤60s 余10×）/ 心跳体积与日志长度无关（varint 增量合法，结构断言 entries==0）/ 快照 11MB/s（S0≥10）；证据物 build/evidence/bench/{split,heartbeat,snapshot}.txt |
| M4 | 35 | bench补齐②：踩坑与修复（字典序≠数值序/分区忘解除/tick驱动缺失）+ 共享助手收口 | PASS | 三真坑：key 未补零致字典序≠数值序（split 点判断被打乱）→定宽7位补零；计时循环忘解除分区→set_partition({})；pump-only 循环 leader 永不发消息→补 tick。重复代码上收 testlib（regions_contiguous/fingerprint/make_split_admin/count_leaders）；implementation_plan 压缩回预算内 |
| M4 | 36 | bench补齐③：notes-m4 验收表三项改✅实测 + 挂账仅剩 trace 埋点与契约追认 | PASS | notes-m4 §4 PERF-07/08/09 全改✅实测数字；§5 挂账移除 bench 项；ctest 45/45；CI PASS×13；登记 45⇄45 |
| M5 | 1 | ①②可判定性：CI13 + tso 6例 + latch 4例 + 全量47 | PASS | run_ci PASS×13；ctest 47/47；tso 6/6（批量单调/跨调用不回退/空批量拒/fatal非单调/调度器适配/1e5 ts/s 吞吐）；latch 4/4（同key串行观察点/交叉key集无死锁/释放唤醒/重复key去重） |
| M5 | 2 | ①②契约与复用：KvError 9变体⇄RegionError 对称 + 调度器兼任TSO + encode_ts_key 复用 | PASS | align 3/3；KvError 6→9 与 RegionError oneof 4/5/6 对称（不进豁免）；TsNotMonotonic 走 fatal 异常（SPEC-4 §4.3.3 明示 fatal 路径，不进 result）；留痕 M5-TXNERROR-DRAFT.md；Ts/encode_ts_key 复用 M0 骨架零改动；TSO 适配器挂 Scheduler（架构图要求） |
| M5 | 3 | ①②文档与红线：红线4（latch 全序加锁防死锁）+ 红线6（TSO 单调）+ banned/scope 回修 | PASS | docs PASS；guard PASS；latch 排序加锁=全局全序（交叉 key 集无环的结构保证）；测试踩坑 2 处：同线程持有期间重取=自我死锁（测试语义错）、std::move 到局部变量生命周期到作用域尾（释放时机错）——均已记测试内注释；sleep_for 违禁（ENG-04）改 join 同步点；task_scope M5 重写生效 |
| M5 | 4 | ③可判定性：CI13 + mvcc 9例 + 全量48 | PASS | run_ci PASS×13；ctest 48/48；mvcc 9/9（roundtrip/写写冲突立即abort/锁冲突/幂等重放/commit锁归属校验+幂等/rollback墓碑+不动他者之锁/CheckTxnStatus三态/多版本快照读） |
| M5 | 5 | ③契约与复用：三CF布局红线5 + Storage语义澄清 + dup收口 | PASS | 对账 3/3；数据布局=Default@start_ts/Lock@key/Write@commit_ts（ts大端，红线5）；关键澄清：Engine::get/write/scan 内部自带 CF 前缀（encode_user），MvccTxn 传 user_key+ts 后缀——首跑双重前缀致全挂；版本归属=剥8B尾后按用户key等值（starts_with 会把 k1 误配给 k）；be64_str 首版写反（110×2^56 现形）已修；重复块收口：for_each_write_version 共享骨架+include 序言重排 |
| M5 | 6 | ③文档与红线：红线1（冲突立即abort）+ 红线2（rollback不动他者之锁）+ 确定性时钟偏差留痕 | PASS | docs PASS；guard PASS；prewrite 检查阶段先查后写失败即 abort（红线1）；rollback 只动本事务锁（红线2 红线2判定）+ 墓碑语义；TTL 判定显式收 now_ms 参数（与④b tick 同取向，不读墙钟可复现）——偏差记 notes-m5 待记 |
| M5 | 7 | ④可判定性：CI13 + si_semantics 6例 + txn_resolve 2例 + 全量50 | PASS | run_ci PASS×13；ctest 50/50；si_semantics 6/6（同key写写冲突首提交胜/不同key并发/可重复读/无脏读blocked/读己之写/abort无痕）；txn_resolve 2/2（崩溃TTL决议回滚+无半提交+清锁后B可写/primary已提交→secondary正向推进） |
| M5 | 8 | ④契约与复用：TxnClient 编排 + resolve_locks_of（三件套收口）+ TxnFixture 上收 | PASS | align 3/3；TxnClient client驱动2PC（primary=字典序最小key，primary先提交，红线8 server不跨region协调）；MvccTxn 加 resolve_locks_of（只认同 primary+start_ts 的锁，红线2）；决议编排 committed→正向推进/过期→清锁/未过期→blocked；Fixture 上收 testlib/txn_fixture.h（si/resolve 共用） |
| M5 | 9 | ④文档与红线：测试踩坑（TSO占号撞号/begin清writes_/move生命周期）+ 确定性时钟 | PASS | 测试踩坑留痕：①tso.current()+1 占号不消费→begin 撞出同 start_ts（读己之写误命中）→一律真实分配；②begin() 清空 writes_（先 set 后 begin=空事务）；③move 到局部变量生命周期到作用域尾（同 latch 层教训）；决议时序全程注入 now_ms 零真实等待（ENG-04 禁 sleep 的确定性解法）；SPEC txn_resolve 标 slow 系真实 TTL 等待场景，注入时钟后 fast 可判定 |
| M5 | 10 | ⑤可判定性：CI13 + txn_peer 3例（经raft三副本）+ 全量51 | PASS | run_ci PASS×13；ctest 51/51；txn_peer 3/3（提交经raft三副本MVCC一致/写写冲突从apply侧结构化传回/崩溃锁经决议走raft清锁后副本一致可再提交） |
| M5 | 11 | ⑤契约与复用：TxnExecutor 双端 + Peer txn 路径（marker/kvpb/Latch/结构化错误） | PASS | align 3/3；TxnExecutor 抽象（LocalExecutor/Peer 端同语义）；Peer 载荷 [0x2B][type][kvpb]（cmdpb 起始字节只会 0x0A/0x12/0x1A 不相撞）；apply 侧 Latch 显式取闩（红线4）+ 结构化 KvError 错误表（client 编排要 LockConflict 字段）；决议写入经 raft（否则副本分叉）；读路径 leader 本地直读（M2 同款偏差记 notes-m5） |
| M5 | 12 | ⑤文档与红线：全副本收敛等待 + 执行端单例（悬垂教训）+ begin_two 上收 | PASS | docs PASS；guard PASS；call_through_raft 等**全部副本** apply 到位（只等 leader 会让 follower 读到锁残留旧态）；PeerTxnExecutor 单例（client 持引用，重复重建悬垂→CI 构建 SEGFAULT 首跑踩坑）；双事务开局收口 TxnFixture::begin_two（client() 虚分派切换执行端）；CI build/ci 目录与 win-debug 双验证 |
| M5 | 13 | ⑥可判定性：CI13 + cli_txn_smoke 4例 + lincheck_si 5种子 + txn_chaos_n150 150轮 + 全量54 | PASS | run_ci PASS×13；ctest 54/54；cli_txn_smoke 4/4（--help/demo/顶层露出/交互式退码1）；lincheck_si 审计三条 SI 不变量（无脏读含读己之写/首提交不被覆盖/终态账本一致）；txn_chaos_n150 150/150（崩溃级联决议+三副本一致） |
| M5 | 14 | ⑥契约与复用：CLI txn 契约（§11 --help+demo）+ 审计器 + 复杂度回修 | PASS | align 3/3；CLI txn 按 SPEC 验收口径（--help+示例）落版：一次性进程无法跨调用持事务→交互式子命令退码1指向用法（偏差记 notes-m5）；cli.md §11 契约文档；lincheck 审计器无脏读含读己之写分支；chaos 崩溃级联决议语义（prewrite 撞未过期锁→按编排先决议再重试）；复杂度 20/17→抽 crash_prewrite/resolve_and_verify |
| M5 | 15 | ⑥文档与红线：红线3（决议后无永久锁）150轮证明 + 读己之写最后胜出修复 | PASS | docs PASS；guard PASS；红线3 经 150 轮崩溃级联决议证明（全部副本锁全清）；发现并修复 TxnClient::get 读己之写取首写而非末写（与 prewrite 落盘不一致）——lincheck 审计器捕获的第三个真实缺陷；登记 54⇄54 |
| M5 | 16 | ⑦可判定性：CI13 + notes-m5 + 证据表逐行核对 + 全量54 | PASS | run_ci PASS×13；ctest 54/54；notes-m5 §4 逐行：8行✅（P0 全达）+ FUNC-17 P2 挂账；SPEC 建议名↔登记名映射注明；证据物 build/evidence/nightly/txn_chaos_150.txt |
| M5 | 17 | ⑦契约与复用：M5 全程档1-2复核（复用判定顺序遵守） | PASS | align 3/3；复用台账：档1 复用 kv.proto 事务骨架（M0 已核对零改动）/Engine 三CF/encode_ts_key/scheduler 位置；档2 语言机制（variant/expected/optional/虚分派）；档3/4 未触发（零新依赖，不引 grpc/不做 JSON）；X-1~X-6 无回潮 |
| M5 | 18 | ⑦文档收尾：挂账清单 + M5 累计18轮全PASS | PASS | 挂账：GC(FUNC-17 P2)/CLI 交互式事务(随daemon化)/txn读路径线性一致(ReadIndex结合)/M5-TXNERROR-DRAFT 追认；M5 18 轮全 PASS；ctest 54/54；CI PASS×13；登记 54⇄54 |
| M6 | 1 | ①可判定性：构建+61测试+CI13 | PASS | cmake 全绿；ctest -j1 61/61（53.49s）；run_ci PASS×13；http_frame_test 5/5 覆盖 BUG-0002/0003/0004 |
| M6 | 2 | ②契约与复用：对齐三项+登记+缺陷门禁+自测 | PASS | check_align 3/3（contract/doc_cmd/banned）；registry 61⇄61；bugfix 4 条全就位；ci/selftest PASS |
| M6 | 3 | ③文档与红线：docs/guard/scope+键空间红线抽查 | PASS | CI docs×4+guard+scope PASS（570行/14文件已声明）；detach 0、proto 0 改动、新依赖 0、banned 0；README/CLAUDE/notes-m6 已回填 |
| M7 | 1 | ①可判定性：applied_recovery 6例 + 全量62 + CI13 | PASS | cmake --preset win-debug 全绿；applied_recovery 6/6（位点重开保持/committed缺口重放/commit后apply前崩溃/put/split/txn 重放幂等）；ctest 62/62；run_ci PASS×13 |
| M7 | 2 | ②契约与复用：独立键+双游标+登记 + bugfix门禁 + 自测 | PASS | check_align 3/3；registry 62⇄62；bugfix 5 条全就位（新增 BUG-0005）；复用 Storage::write 批量与 RaftLog meta 持久化，不引新依赖；split 重放幂等守卫由测试抓出后补 |
| M7 | 3 | ③文档与红线：核心模块授权 + docs/guard/scope + 未决留痕 | PASS | 用户在确认区块后明确选"方案1：独立键"，src/raft/ 改动已逐条批准；CI docs×4+guard+scope PASS；6×u64 meta 格式零改动、无迁移脚本需求；implementation-notes §8.7 留痕 compact 判据与 txn 伪错误未决项 |
| M7 | 4 | ④可判定性：第2–5项合并验证（压缩/ReadIndex/观测/多副本）+ 全量65 + CI13 | PASS | cmake --preset win-debug 全绿；ctest 65/65（57.7s）；engine_compaction 2/2（阈值压表数+墓碑不复活+重开一致+threshold=0 不合并）；readindex_path 2/2（raw/txn 读先 ReadIndex 再读）；multi_replica 2/2（真 TCP 三副本选主+复制+leader ReadIndex 读+多 voter 拒 split）；run_ci PASS×13；align 3/3 |
| M7 | 5 | ⑤契约与复用：ReadIndex 墙钟兜底 + 多 voter 路由边界 + 存储快照共享 | PASS | ReadIndex 新增可选 timeout（轮数与墙钟取先到者）——多副本下纯轮数自旋会在心跳回执到达前耗尽，实测 2000 轮 < 5ms 对端 5ms 泵周期，诊断确证后改为 5s 墙钟；多 voter 显式拒绝 split：raftpb::Message 只带 to=PeerId 无 region_id，多 region×多 voter 会路由歧义，拒绝优于造不可路由 peer；MemTable 改 shared_ptr<const> + 写时复制，create_reader O(1)；scan 改 k 路流式归并，O(k) 内存、命中 limit 即停 |
| M7 | 6 | ⑥文档与红线：CI 全门禁回修（复杂度/行数/段错误）+ docs/guard/scope | PASS | 首跑 static 报 4 处（peer.cpp on_ready 16 / server_app start 120行 / engine merge_stream 109行/26），抽 compact_log_if_needed、start_transport_locked/start_http_locked、Cursor 类+merge_stream 两遍归并后全过；CI 构建段错误根因=Cursor 构造期调 key() 时 ssts_ 未绑定（win-debug 恰好短路未崩），改直接持有 SstReader* 后 build/ci 50/50 全绿；registry 65⇄65；bugfix 5 条；guard/scope PASS |

<!-- review:end -->

## 约定

- 「结论」只允许 PASS / FAIL；FAIL 轮次保留在台账里，作为修复依据，不删除。
- 三轮全 PASS 后跑对齐检查（`python tools/ci/check_align.py`），两者都过才可进入推送环节。
- 台账本身不替代证据：每轮「备注」应写清跑了什么命令、产物在哪。
