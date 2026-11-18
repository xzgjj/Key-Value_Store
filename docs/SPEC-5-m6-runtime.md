# SPEC-5 — M6 常驻运行时与可运维（daemon 化）

> 上游：SPEC-4（事务）；需求：UX-01/02/03/04/05/06/07、REL-02、ENG-04/05、FUNC-17(P2)。
> 状态：**已交付（追记版，2026-11-18）**。M6 先落地后补册，证据链按实测回填；
> 未做项在 §5.8 明列，不以"规划口径"冒充"已实现"。

## 5.0 追记说明

M0–M5 按 SPEC-0~4 推进，M6（daemon 化）当时以迁移清单直接施工，没有成册。补册的目的不是补流程，
而是补**边界**：后续 M7 要动的组合根、锁、注册表都在这册里定过位。三条口径先说清：

1. 本册只写 M6 实际交付的内容。`observe_port` 独立观测端点属 M7 ④，这里只提它挂在哪，不当 M6 成果。
2. 证据链只引已登记用例，未登记的不写进表（登记表是唯一事实来源）。
3. 与设计文档的偏差（PD-lite 不独立成进程、跨 region 事务未做）在 §5.8 留痕，不改上游规格。

## 5.1 功能范围

| 交付物 | 内容 | 需求 |
|---|---|---|
| 常驻进程 | `tinykv-server <data_dir> [port]`，一条命令起服务，打印真实监听地址 | UX-01/05 |
| 组合根 | `ServerApp` 唯一装配点；`Service`/`LifecycleManager` 管顺序启停 | ENG-04 |
| 泵循环 | Worker 线程批量 drain → 分发 → apply；异常不杀线程 | REL-02 |
| 会话事务 | `TxnSessionManager` 持服务端 `TxnClient`，CLI 交互式事务落地 | UX-02 |
| 观测 | `/status` 汇总 term/leader/commit/applied/错误计数；`/admin` 六个 region 观测路由 | UX-04 |
| 拦截器 | log / metrics / trace 三段可插拔链，before 正序 after 逆序 | UX-04 |
| 动态注册表 | `RegionPeerRegistry` 按 key 路由，split 后动态建 peer 并迁数据 | FUNC-08 运行时 |
| GC | `MvccTxn::gc` 走 Admin 共识，多 region 广播 | FUNC-17(P2) |
| 边界护栏 | 请求解析不抛、帧读满、raw 键空间隔离、会话与回执有界回收 | UX-03、REL-02 |

## 5.2 架构

```
main ──sig──► ServerApp(Service)
               ├── TSO（scheduler 兼任）
               ├── Storage×N（每 region 一份 Engine：data_dir/regions/<id>）
               ├── RegionPeerRegistry（key → peer；split observer 工厂）
               ├── Worker（joinable 线程 + 原子停止位 + 条件变量）
               ├── TxnSessionManager（会话级 TxnClient）
               └── StatusServer（/status、/kv、/txn/*、/admin）
Interceptor 链：log → metrics → trace（before 正序，after 逆序）
并发闸：peer_mu_ 串行化全部 Peer/Storage 访问（HTTP、泵、executor 共用）
```

### 5.2.1 启停时序

```
start:  config → TSO → Storage×N → Peer×regions → Worker → StatusServer
stop :  StatusServer（先拒新请求）→ Worker → Peer → Engine（逆序，失败回滚已启组件）
```

顺序不是风格问题：观测端点必须先能拒绝请求再拆 Worker，否则关闭窗口里会有 handler 打到已释放的 peer。

### 5.2.2 split 的运行时路径

apply 到 Split admin 后，Peer 通过 `SplitObserver` 回调宿主；`ServerApp` 用 observer 工厂给每个 peer
（含新建 peer）挂钩 → `RegionPeerRegistry::on_split` 建右半 Engine、迁数据、登记路由。
回调在 apply 路径内同步执行，只做存储迁移与实例登记，**不重入 pump**（重入会自锁）。

## 5.3 接口设计

### 5.3.1 Service 契约

```cpp
class Service {
public:
    virtual ~Service() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual result<void> start() = 0;   // 幂等：已启动直接返回成功
    virtual void stop() noexcept = 0;                  // 幂等：未启动调用无副作用
};

class LifecycleManager {          // 顺序 start、逆序 stop、失败回滚
public:
    void add(std::unique_ptr<Service> svc);
    [[nodiscard]] result<void> start_all();
    void stop_all() noexcept;
};
```

### 5.3.2 组合根选项

```cpp
struct ServerOptions {
    std::filesystem::path data_dir;                 // 必填
    std::string host{"127.0.0.1"};
    std::uint16_t status_port{0};                   // 0 = 系统分配
    std::uint64_t election_timeout{8};
    std::uint64_t heartbeat{3};
    std::chrono::milliseconds worker_interval{10};
    LogSink log_sink;                               // 空 = 不输出请求日志
};
```

### 5.3.3 端点表

| 端点 | 方法 | 语义 | 错误 |
|---|---|---|---|
| `/status` | GET | term/leader/commit/applied/apply_errors/region 数 | 503 未就绪 |
| `/kv` | GET/POST | raw 读写（集群写走共识） | 400 参数、409 非 leader |
| `/txn/begin`、`/txn/set`、`/txn/get`、`/txn/commit` | POST | 会话事务 | 400/409 |
| `/admin/regions`、`/admin/region`、`/admin/split` | GET/POST | region 观测与切分 | 400/404 |
| `/admin/scheduler`、`/admin/trace` | GET/POST | 调度视图、trace 开关 | 400 |

### 5.3.4 错误矩阵（本册新增/收口部分）

| 变体 | 触发 | 对外行为 |
|---|---|---|
| `InvalidArgument` | 畸形查询串、超大请求体、空 key | 400，进程存活（BUG-0002/0003 回归守住） |
| `IoFailed` | apply 失败、磁盘错误 | 计数进 `/status`，请求返回 500 |
| `NotLeader` | 集群写落到 follower | 409 + leader hint（UX-03） |
| `KeyNotInRegion` | 路由表查不到 key | 400 + 缺失区间描述 |

## 5.4 核心逻辑骨架

```
Worker 泵循环（每轮）:
  取停止位 → 持闸 → 每个 peer: tick → on_ready(apply) → take_messages → 发传输 → 释放闸 → 睡 interval
  异常：catch 后累加 apply_errors，继续下一轮（线程不退出，错误靠 /status 暴露）

split 路径:
  apply(SplitAdmin) → 校验开区间 → StoreMeta::replace_region → SplitObserver(左, 右)
    → Registry::on_split: 建右 Engine → 扫三 CF 迁 [split_key, end) → 从旧 Engine 删 → 登记 key 路由
    → 失败：返回结构化错误，apply 记错，不吞

会话回收（主循环每秒）:
  advance_clock(1000) → 清理空闲 >30s 的 TxnSession → 回收已取走的 replies_ / txn_errors_
```

**每 region 独立 Engine 的理由**：Raft 日志键 `\0re`/`\0lm` 在每个 peer 都从 index 1 起，
共用一份引擎会互相覆盖，回放时串日志。这是硬约束，不是优化选择。

## 5.5 红线

1. **装配唯一入口**：只有 `ServerApp` 创建并连接组件，组件内部不自建线程管理生命期。
2. **禁 detached 线程**：后台线程一律 joinable + 原子停止位 + 条件变量，`stop()` 必须能等到线程退出。
3. **逆序停止**：先拒新请求再拆依赖；顺序反了就是关闭期崩溃。
4. **泵异常不杀线程**：异常被循环吞掉并计数，错误通过 `/status` 暴露，不允许"静默死亡"。
5. **split 回调不重入**：observer 只动存储与注册表，绝不在持闸状态下再发起提案。
6. **raw 键保留前缀**：raw 命名空间用 `\0rk` 隔离，禁止与 MVCC 键混排（BUG-0004 根因）。
7. **请求边界不抛**：HTTP 解析走无异常路径，任何畸形输入只能产生 4xx。
8. **有界回收**：会话、回执、错误队列都必须有上限与回收路径，长跑不允许无界增长。

## 5.6 示例

### 起服务 + CLI 读写

```bash
./build/win-debug/src/server/tinykv-server ./data 3380
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 status
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 put k v
./build/win-debug/src/cli/tinykv-cli --endpoint 127.0.0.1:3380 regions
```

### 交互式事务（会话作用域）

```bash
SID=$(tinykv-cli --endpoint 127.0.0.1:3380 txn begin | sed 's/session=//')
tinykv-cli --endpoint 127.0.0.1:3380 txn set "$SID" k v
tinykv-cli --endpoint 127.0.0.1:3380 txn commit "$SID"
```

### 配置项（增量）

| 配置项 | 默认 | 影响 |
|---|---|---|
| `server.host` | 127.0.0.1 | 绑定地址；默认只本机可访问 |
| `server.status_port` | 0（随机） | 观测端口；0 由系统分配并打印 |
| `worker.interval_ms` | 10 | 泵周期；过小空转、过大延迟抖动 |
| `session.idle_timeout_s` | 30 | 事务会话回收阈值 |

## 5.7 验收与证据链

| 需求 | 验收口径 | 测试登记名 | 证据物 |
|---|---|---|---|
| ENG-04 | 顺序启、逆序停、启动失败回滚 | `lifecycle_test`、`server_app_test` | junit.xml |
| REL-02 | 泵异常不杀线程；apply 失败计数可见 | `worker_test`、`server_app_test` | junit.xml |
| UX-04 | `/status` 汇总字段完整；`/admin` 六路由可用 | `cli_status_test`、`cli_region_test` | junit.xml |
| UX-03 | 畸形参数/超大请求只产生 4xx，进程存活 | `http_frame_test` | junit.xml |
| FUNC-08(运行时) | split 后新 peer 接管、按 key 路由、数据迁移正确 | `server_app_test`、`peer_registry_test` | junit.xml |
| UX-02 | CLI 本地与集群双模式、txn 子命令 | `cli_local_smoke`、`cli_txn_smoke` | junit.xml |
| UX-06 | M6 实现笔记（含评审修复表） | —（人工） | `docs/notes-m6.md` |
| FUNC-17(P2) | GC 后旧版本清除、活跃读不受影响 | `gc_test` | junit.xml |

**M6 完成判定**：上表齐备 + M6 三轮审查台账 PASS（`docs/review-log.md`）。

## 5.8 开放决策点与遗留

| # | 事项 | 现状/结论 |
|---|---|---|
| D5-1 | PD-lite 是否独立进程 | 不独立：进程内对象。代价是多 store 拓扑需手工传 `voters`/端口（M7 ⑤ 已补进组合根） |
| D5-2 | 并发模型 | 一把 `peer_mu_` 串行闸；已知吞吐上限，拆锁移 M7 ⑨a |
| D5-3 | 跨 region 事务 | 未做；M7 ⑦ 处理，且现状存在静默写错分区的洞（见 SPEC-6 §6.2.1） |
| D5-4 | split 元数据持久化 | 未做；重启按单 region 重建，M7 ⑧ 处理 |
| D5-5 | 鉴权与 TLS | 未做；M7 ⑩ 处理，默认关 + 启动告警 |
| D5-6 | trace 的 Peer 内事件埋点 | 仅路由级，Peer 内埋点未接（`cli_region_test` 只验开关与输出格式） |
