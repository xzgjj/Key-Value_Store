# 接口契约核对单（2026-11-18）

> 用途：本批改动触及对外契约的地方集中在这里，**逐条等你确认**。
> 每条给出：现状（改前）→ 变更 → 影响面 → 回退方式。
> 判定口径：CLI 子命令/参数/退出码、HTTP 线协议、proto 字段、配置键、数据目录布局。
> 未列入本文的改动都是内部实现（不影响外部调用方）。

## 1. 已经落地、需要你追认的契约变更

| # | 契约面 | 改前 | 改后 | 影响面 | 回退 |
|---|---|---|---|---|---|
| C-1 | `TxnPrewriteRequest`（`proto/kv.proto`） | 无加锁时刻字段 | 追加 `uint64 created_ms = 5`（可选；缺省 0 = 旧客户端） | 线上格式纯追加，旧客户端/旧数据互不影响 | 不再填该字段即回旧行为 |
| C-2 | `Peer::propose` 返回值（C++ 接口） | `result<void>` | `result<bool>`：`true`=已入队，`false`=去重命中（语义已成功） | 仅 C++ 调用方；3 处调用点已改 | 恢复 `result<void>` 并让调用方忽略该分支 |
| C-3 | 跨区 prewrite 行为 | 静默全写 primary 分区 | `InvalidArgument{"跨区事务未启用…"}` | 依赖"跨区事务能跑"的调用方会开始报错——这是**预期**方向（SPEC-6 §6.5 红线 1） | 不注入 `RegionOf` 即回旧路径（不建议） |
| C-4 | `tinykv-server` 命令行 | `<data_dir> [status_port]` | 追加可选 `--config <file>`（位置参数语义不变） | 旧启动脚本不受影响 | 不传 `--config` 即旧行为 |
| C-5 | 配置文件新增 `server:` 段 | 只有 `engine:` / `log:` | `server.host`、`server.auth_token` | 未知键仍报错（旧配置无需修改即可用） | 删该段 |
| C-6 | HTTP 连接语义 | 每请求一连接（响应后一定 close） | HTTP/1.1 默认 keep-alive；客户端显式 `Connection: close` 才关 | 只影响自研客户端；CLI 自带 close，行为不变 | 把 `wants_close` 恒真即回旧行为 |
| C-7 | HTTP 鉴权 | 无 | 配 `server.auth_token` 后必须 `Authorization: Bearer <token>`，否则 401 | 未配 token 时零影响 | 清空 token |
| C-8 | 数据目录新增 `meta/` | 无 | `meta/regions.pb` + `regions.pb.bak` | 旧库无此目录 = 首次启动路径，自动建并落盘 | 删 `meta/` 即回单 region 兼容路径 |
| C-9 | 就绪输出 | `stdout` 全缓冲（重定向时行不落盘） | 行缓冲 + 就绪行立即 flush | 解析该行的脚本从"等不到"变成"等得到" | 无（纯改善） |
| C-10 | `txn.cross_region_enabled` 默认值 | false（跨区写入报"跨区事务未启用"） | **true**（跨区事务按 region 分组两阶段提交；显式写 false 即回旧行为） | 依赖"跨区被拒绝"的调用方会看到事务真的跨区提交了——这是 ⑦ 转正的预期方向；配置文件显式声明 false 可完全回退 | 配置里显式写 `cross_region_enabled: false` |
| C-11 | `/status` 响应 | 无 `txn_cross_region` 字段 | 追加 `"txn_cross_region":true|false`（红线 6：开关状态可观测） | 纯追加，解析方不受影响 | 无 |

## 2. 需要你决定的三件事

| # | 议题 | 选项 | 我的建议 |
|---|---|---|---|
| D-1 | `/kv` 请求体限定"制表符分隔"，导致 **value 不能含制表符** | 你的结论：**保持制表符分隔**（已写进 `http-protocol.md §3`，含"value/key 不能含制表符"的明确边界） | 与我的建议一致；需要二进制内容时走事务 API 的 proto 载荷 |
| D-2 | 鉴权只做 Bearer token，未做 TLS（M7-SEC-02 的 TLS 部分） | (a) 维持"未鉴权强制回环 + token 明文" (b) 接 OpenSSL 做 TLS 传输装饰器 | **已裁决（2026-11-18）**：选 (a)，TLS 移出 M7 另立项。M7-SEC-02 以"未启用时强制绑定回环"部分达成，文档如实记录 |
| D-3 | ⑦ 跨区 2PC 状态与开关 | 已按你的指示补做：分组 → 状态机（PREPARE/TSO/COMMIT_PRIMARY/COMMIT_SECONDARY）→ 恢复（锁决议广播到各 region）。`txn.cross_region_enabled` 仍默认 false | **已裁决（2026-11-18）**：`txn_cross_region_chaos_random`（随机 kill，150 轮）通过后开关转正为默认 true（C-10）。半提交不出现的断言覆盖死亡点 0..4 × 主组侧 × 半途决议 |

## 3. CLI 交互面（与 `docs/cli.md` 一致，未变）

| 面 | 现状 |
|---|---|
| 入口模式 | 一次性执行（本地模式）/ `--endpoint` 集群模式；无交互 shell |
| 命令 | `put/get/del/scan/status`、`txn {begin,set,del,get,commit,rollback,gc}`、`regions/region/region split`、`scheduler status`、`trace` |
| 退出码 | 0 成功 / 1 用法错误 / 2 运行错误 / 3 key 不存在 |
| 配置加载 | 单文件显式路径（`--config`，默认 `demo.yaml`）；无 profile 合并、无环境变量覆盖 |
| 插件 | 无插件机制（设计如此：CLI 是薄客户端） |
| 环境变量 | 无。所有行为由命令行 + 配置文件决定 |

**已收口**：你选了"补服务端"，三个命令现已实现并有 `admin_endpoints_test` 守护
（`/region/peers`、`/scheduler/status`、`POST /trace`），响应形状写进 `docs/http-protocol.md §2`。
`region peers` 如实报告"单 store 单 peer/region、store_id=0"，不编造多副本表；
`scheduler status` 明确回 `mode=pd-lite-embedded` 与"单 store 无跨节点调度对象"。

## 4. 与本次改动无关但已在文档中固定的口径

- HTTP 端点表、请求体编码、响应形状：见 [http-protocol.md](http-protocol.md)（本次新增，此前只在源码里）。
- 语言标准与编译器支持面：见 [testing.md](testing.md) §9a（默认 C++23，17/20 为兼容档）。
- Region 元数据文件格式：见 `src/raftstore/meta_store.h` 文件头（magic/count/记录/CRC 布局）。
