# TinyKV HTTP 线协议（tinykv-server ⇄ tinykv-cli / curl）

> 性质：**线协议契约**。凡是通过 HTTP 访问服务端的调用方（CLI、脚本、curl、将来的 SDK）
> 都以本文为准。改动任一端点、字段或编码必须同时改本文与 `docs/cli.md`。
> 起因：`/kv` 的请求体一直是"制表符分隔的裸文本"，只写在源码里——本文把它补成契约。

## 1. 连接语义

| 项 | 规则 |
|---|---|
| 版本 | 只接受 `HTTP/1.1`。`HTTP/1.0` 或畸形请求行按"一请求一连接"处理（响应后立即关闭） |
| 持久连接 | 默认 keep-alive：同一连接可连续发多个请求。客户端要关闭时带 `Connection: close` |
| 流水线 | 支持（同一连接连发多个请求，按序回响应）；不保证乱序响应 |
| 请求体长度 | `Content-Length` 必填；缺省按 0 处理。头部 ≤ 8 KB、体 ≤ 1 MB，超限回 `ERR bad-request` |
| 响应 | 恒带 `Content-Length`；`Connection: keep-alive` 或 `close` 显式声明 |
| 鉴权 | 配置了 `server.auth_token` 时，所有请求必须带 `Authorization: Bearer <token>`，否则 `401` |

## 2. 端点总表

| 方法 | 路径 | 作用 | 请求体 |
|---|---|---|---|
| GET | `/status` | 节点状态 JSON | — |
| GET | `/kv?key=<k>` | 读 raw key（线性一致读：ReadIndex + applied 追平） | — |
| POST | `/kv` | 写 raw key | `put\t<key>\t<value>` 或 `del\t<key>` |
| POST | `/txn/begin` | 开事务会话 | 空 |
| POST | `/txn/set` | 会话内缓冲写 | `set\t<sid>\t<key>\t<value>` |
| POST | `/txn/del` | 会话内缓冲删 | `del\t<sid>\t<key>` |
| POST | `/txn/commit` | 提交会话 | `commit\t<sid>` |
| POST | `/txn/rollback` | 回滚会话 | `rollback\t<sid>` |
| POST | `/txn/gc` | GC 到安全点 | `gc` 或 `gc\t<safe_point>` |
| GET | `/txn/get?session=<sid>&key=<k>` | 会话内读（读己之写 + 快照读） | — |
| GET | `/txn/status?session=<sid>` | 会话 start_ts | — |
| GET | `/regions` | 全部 region 列表 JSON | — |
| GET | `/region?id=<n>` | 单个 region 详情（含 term/commit） | — |
| GET | `/region/peers?id=<n>` | 该 region 的副本分布 | — |
| GET | `/scheduler/status` | 调度器视图（单 store 事实 + 为什么没有 PD） | — |
| POST | `/trace` | 采样开关 | `trace\t<region_id>\ton` 或 `…\toff` |
| POST | `/region/split` | 手动 split | `split\t<region_id>\t<split_key>` |
| GET | `/metrics` | 拦截器计数 | — |
| GET | `/trace` | 请求轨迹（最近 N 条） | — |

观测端口（`observe_port`）只放行**读型**路径：`/status`、`/regions`、`/region`、`/metrics`、`/trace`。
事务写端点与 `/kv` 在观测端口一律 404——观测面不提供写入能力。

## 3. 请求体编码（此前未文档化，本文定版）

请求体是 **制表符（`\t`）分隔的裸文本**，第一段是操作名。刻意不引 JSON：写操作只有两三种形态，
文本协议两端都能一眼看懂，且不需要在服务端引 JSON 解析器。

| 端点 | 形态 | 约束 |
|---|---|---|
| `POST /kv` | `put\t<key>\t<value>` / `del\t<key>` | 段数必须精确匹配；否则 `ERR bad-request` |
| `POST /txn/set` | `set\t<sid>\t<key>\t<value>` | 同上 |
| `POST /txn/del` | `del\t<sid>\t<key>` | 同上 |
| `POST /txn/commit` | `commit\t<sid>` | 同上 |
| `POST /txn/rollback` | `rollback\t<sid>` | 同上 |
| `POST /txn/gc` | `gc` 或 `gc\t<safe_point>` | safe_point 必须是十进制 u64 |
| `POST /region/split` | `split\t<region_id>\t<split_key>` | region_id 必须是十进制 u64 |
| `POST /trace` | `trace\t<region_id>\ton|off` | region 必须存在（否则 `ERR no-region`）；开关值只能是 on/off |

**已知边界（不是 bug，是协议约束）**：

1. **value 不能含制表符**——它会被当成字段分隔符，段数不符即 `ERR bad-request`。
   需要存二进制/制表符内容时，走 raw key 的十六进制封装路径（规划中）或改用事务 API 的 proto 载荷。
2. **key 不能含制表符**，理由同上；key 允许 UTF-8 与换行。
3. 空 value 合法（`put\tk\t` 三个字段、第三段为空串），与"删除"以操作名区分。

## 4. 响应

| 形态 | 含义 |
|---|---|
| `OK` / `OK session=<sid>` / `OK commit_ts=<n>` / `OK split` / `OK gc` / `OK trace on|off` | 成功；形状随端点不同，以端点表为准 |
| `{"found":true,"value":"..."}` | 读命中（value 已做 JSON 转义：`"`、`\`、`\n`、`\r`、`\t`、其余控制字符转 `\u00XX`） |
| `{"found":false}` | 读未命中（**不是错误**） |
| `{"error":"<kind>[:<detail>]"}` | 读路径失败（如 `IoFailed:applied 未追上确认位点`） |
| `ERR <reason>` | 写/管理路径失败；`reason` 是稳定短标识（`bad-request`、`no-region`、`not-leader`、`apply-timeout`、`split-unsupported-in-multi-voter`） |
| `404` + 空体 | 未知路径 / 观测端口上的写端点 |
| `401` + 空体 | 鉴权失败（不建立持久连接，不回显任何凭据） |

读路径用 JSON、写路径用 `OK`/`ERR` 是历史选择：读结果需要承载 value/错误字段，写结果只需要成败。
两者都在本文固定下来，不再"看源码猜"。

## 5. 可直接复制执行的示例

```bash
# 1) 启动（未配置 token → 自动绑回环，并在 /status 里标 "auth":false）
./build/win-debug/src/server/tinykv-server.exe ./data 3401

# 2) 就绪探测：脚本应等这一行，而不是轮询端口（stdout 已改为行缓冲）
./build/win-debug/src/server/tinykv-server.exe ./data 3402 | grep -m1 "就绪"

# 3) raw 读写
curl -s -X POST --data-binary $'put\thello\tworld' http://127.0.0.1:3401/kv
curl -s 'http://127.0.0.1:3401/kv?key=hello'

# 4) 事务：begin 拿 sid → set → commit
SID=$(curl -s -X POST http://127.0.0.1:3401/txn/begin | sed 's/^OK session=//')
curl -s -X POST --data-binary $'set\t'"$SID"$'\tk1\tv1' http://127.0.0.1:3401/txn/set
curl -s -X POST --data-binary $'commit\t'"$SID" http://127.0.0.1:3401/txn/commit

# 5) 观测面
curl -s http://127.0.0.1:3401/regions
curl -s http://127.0.0.1:3401/status | head -c 200

# 6) 开了鉴权时
curl -s -H 'Authorization: Bearer <token>' http://127.0.0.1:3401/status
```

## 5a. 跨区事务开关的线上表现

`tinykv-server` 的 `txn.cross_region_enabled` 决定同一个事务能否写多个 region：

| 开关 | 线上表现 |
|---|---|
| `true`（默认，M7 转正） | 按 region 分组两阶段提交：每组一次 prewrite → 取 commit_ts → 先提交 primary 组 → 再提交其余组；中途崩溃由"读者遇锁 → 查 primary 状态"收敛 |
| `false`（显式关闭） | `POST /txn/commit` 返回 `ERR InvalidArgument: 跨区事务未启用（key=… 属 region N，primary=… 属 region M）`；**不会有任何写入落地** |

开关当前值经 `GET /status` 的 `txn_cross_region` 字段可查（红线 6）。
单 region 事务在两种开关下走同一条路径（无额外开销）。

## 6. 与 CLI 的关系

`tinykv-cli --endpoint <host:port> <命令>` 走的就是本协议：`status`→`/status`、`get`→`/kv?key=`、
`put/del`→`POST /kv`、`txn *`→`/txn/*`、`regions/region`→观测端点。
CLI 自己在请求里带 `Connection: close`（一次性进程，没有复用收益），服务端两种都支持。
