# M6 最终交付说明（daemon 化与运行时）

## 1. 交付内容

| 项 | 实现 | 证据 |
|---|---|---|
| 组合根 | `ServerApp`：config → TSO → Storage×N → Peer×regions → Worker → StatusServer(handlers) | `src/server/server_app.h` |
| 优雅关闭 | `main`：SIGINT/SIGTERM → 原子标志 → 逆序 stop | `src/server/main.cpp` |
| Service 抽象 | `Service`/`LifecycleManager`：顺序 start、逆序 stop、失败回滚 | `src/server/service.h` |
| Worker 泵循环 | joinable 线程 + 原子停止 + cv 等待；泵异常不杀循环 | `src/server/worker.h` |
| 会话作用域 | `TxnSessionManager`：服务端持 TxnClient，CLI 交互式事务解锁 | `src/server/txn_sessions.h` |
| Interceptor 链 | log/metrics/trace 可插拔；before 正序 / after 逆序 | `src/server/interceptor.h` |
| GC | `MvccTxn::gc` + Admin 走共识；多 region 下广播每个 peer | `src/txn/mvcc.h`、`src/server/peer_txn.h` |
| region→peer 动态注册表 | `RegionPeerRegistry`：按 key 路由，split 后动态建 peer + 数据迁移 | `src/server/peer_registry.h` |

## 2. 关键架构决策

**每 region 独立 Engine（Storage×N）**：raft log 以固定前缀 `"\0re"`/`"\0lm"` 存于 Default CF；
两个 Peer 共用 Engine 会写同一把 log key（都从 index=1 起），必然串日志。故每个 region 一份
独立 Engine（子目录 `data_dir/regions/<id>`），初始 region 1 用 data_dir 根（兼容既有布局）。

**split 动态建实例**：Peer 在 `apply_split_admin` 成功后通过 `SplitObserver` 回调宿主；
`ServerApp` 通过 Registry 的 observer 工厂给每个 peer（含新 peer）挂钩，支持多级 split。
回调在 apply 路径内同步执行，只做存储迁移 + 实例登记，绝不重入 peer 的 pump。

**数据迁移**：扫描旧 Engine 的三列族，把 user key 落在右区间 `[split_key, end)` 的条目
写入新 Engine 并从旧 Engine 删除。跳过 `\0` 前缀的 raft log 内部键；Default/Write 去掉
末尾 8 字节大端 ts 得 user key，Lock 键即 user key。新 peer 从空 log 起，状态机只含
迁移过去的数据，与路由一致。

**事务跨 region 路由**：`PeerTxnExecutor` 的三个回调（peer/pump/applied）都带 key；
get 按被读 key、prewrite 按 primary、commit/rollback 按首 key 定位 region。
GC 是 Admin 例外：广播到所有 peer，每个 region 各自走共识。

**并发模型**：`peer_mu_` 是全部 Peer/Storage 访问的串行闸（课程级粗粒度锁）；
Worker 后台泵、HTTP handler、executor 内部泵共用它。observer 回调在持闸的 apply 路径内，
不再重复加锁。

## 3. 已知限制（明确记录，不伪装完成）

- **跨 region 事务**：一个事务的 primary 与写入 key 若落在不同 region，2PC 的两阶段会
  分别路由到不同 peer；课程级未提供分布式事务协调器（TiKV 由 2PC + lock resolver 跨 region
  解决）。当前实现保证单 region 事务正确，跨 region 事务属未来工作。
- **split 持久化**：重启后 region 元数据从单 region 1 重建（不扫描已有子目录），
  多级 split 的目录数据不会被自动纳管。生产系统由 store meta 持久化解决。
- **GC 安全点**：取活跃会话最小 start_ts；无会话时取当前 TSO。锁冲突解析的读路径 GC 后
  版本裁剪语义见 SPEC-4 §4.4。

## 4. 验证

- cmake 全量构建绿；`ctest -j1` 60/60；CI PASS×13。
- 新增用例：`interceptor_test`（5）、`peer_registry_test`（2，含真实数据迁移）、
  `server_app_test` 的 `SplitCreatesPeerAndRoutesByKey` / `RepeatedSplitCreatesThirdPeer`（2）
  与 Interceptor e2e（1）。
- `snapshot_bench` 在默认并发 ctest 下偶发 9.7MB/s（门槛 10），单跑过——资源竞争抖动，
  复测用 `ctest -j1`。
