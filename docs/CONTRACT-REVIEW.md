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

## 2. 需要你决定的三件事

| # | 议题 | 选项 | 我的建议 |
|---|---|---|---|
| D-1 | `/kv` 请求体限定"制表符分隔"，导致 **value 不能含制表符** | (a) 保持并写进契约（已做） (b) 引入转义（如 `\t`/`\\`）并升版本 (c) 请求体改 JSON | (a)：代价明确、实现简单；真正需要二进制时走 proto 载荷（事务 API 已经是） |
| D-2 | 鉴权只做 Bearer token，未做 TLS（M7-SEC-02 的 TLS 部分） | (a) 维持"未鉴权强制回环 + token 明文" (b) 接 OpenSSL 做 TLS 传输装饰器 | (a)：TLS 需要新依赖与证书生命周期管理，属独立议题；当前形态已能挡住"裸奔在公网" |
| D-3 | ⑦ 跨区 2PC 只交付到"分组内核" | (a) 就此打住（护栏继续拒跨区） (b) 继续做满状态机 + 恢复 | (a)：SPEC-6 §6.1.2 明确要求拆 PR；半成品 2PC 比不开更危险 |

## 3. CLI 交互面（与 `docs/cli.md` 一致，未变）

| 面 | 现状 |
|---|---|
| 入口模式 | 一次性执行（本地模式）/ `--endpoint` 集群模式；无交互 shell |
| 命令 | `put/get/del/scan/status`、`txn {begin,set,del,get,commit,rollback,gc}`、`regions/region/region split`、`scheduler status`、`trace` |
| 退出码 | 0 成功 / 1 用法错误 / 2 运行错误 / 3 key 不存在 |
| 配置加载 | 单文件显式路径（`--config`，默认 `demo.yaml`）；无 profile 合并、无环境变量覆盖 |
| 插件 | 无插件机制（设计如此：CLI 是薄客户端） |
| 环境变量 | 无。所有行为由命令行 + 配置文件决定 |

**已知不一致（需要你决定怎么收口）**：`region peers`、`scheduler status`、`trace region ... on|off`
三个命令仍会打到服务端不存在的端点（404）。收口方式二选一：
(a) 服务端补实现；(b) 从 CLI 移除并更新 `docs/cli.md`。当前先记录，未动。

## 4. 与本次改动无关但已在文档中固定的口径

- HTTP 端点表、请求体编码、响应形状：见 [http-protocol.md](http-protocol.md)（本次新增，此前只在源码里）。
- 语言标准与编译器支持面：见 [testing.md](testing.md) §9a（默认 C++23，17/20 为兼容档）。
- Region 元数据文件格式：见 `src/raftstore/meta_store.h` 文件头（magic/count/记录/CRC 布局）。
