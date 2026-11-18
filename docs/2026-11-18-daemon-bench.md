# 单机 daemon 实机数据（2026-11-18）

环境：Windows 11 + MSYS2 g++（win-debug 构建），本机 NVMe，127.0.0.1 回环。
方法：真实进程 + 真实 TCP（每请求新建连接，与 CLI 一致）；量 `tinykv-server` 二进制，非测试夹具。
规模：300 次/组顺序，8×40 并发，30 个事务；未做长跑。

## 1. 单机 daemon（当前唯一可从命令行部署的形态）

| 指标 | 数值 |
|---|---|
| 冷启动到可服务 | 520–550 ms |
| 重启到可服务（WAL 恢复 1 条日志） | 516–535 ms |
| RSS（~3000 次写、3 MB 数据后） | 17.7 MB |
| 数据体积（3000 次 1 KB 写） | 2.95 MB |
| apply_errors | 0 |

| 路径 | p50 | p99 | mean | 吞吐 |
|---|---|---|---|---|
| GET /status | 0.90 ms | 19–25 ms | 4.4–5.0 ms | — |
| put 64 B 串行 | 1.01–1.22 ms | 22–25 ms | 5.2 ms | 187–193 ops/s |
| put 1 KB 串行 | 1.10 ms | 21–24 ms | 5.0 ms | 182–199 ops/s |
| get 1 KB 串行 | 0.90–0.98 ms | 22–25 ms | 4.0–4.7 ms | 214–247 ops/s |
| put 64 B 8 线程 | 1.34–1.40 ms | 24 ms | 3.0–3.6 ms | 1377–1749 ops/s |
| txn 5 key 端到端 | 32 ms | 81 ms | 32.8 ms | 153 key/s（≈31 txn/s） |

延迟分布呈双峰：p50 ~1 ms，约两成请求落在 20 ms 量级。怀疑与 Worker 泵周期（10 ms）量化及
WAL 组提交窗口有关，未单独验证。

## 2. 3 副本（进程内 3×ServerApp + 真 TCP，非 daemon）

`tcp_perf_bench`：

| 场景 | 数值 |
|---|---|
| 串行 batch=1 fsync=on | 465 records/s |
| 串行 batch=1 fsync=off | 657 records/s |
| batch=64 fsync=off | 32 560 records/s（49.5×） |
| 流水线 window=256 fsync=on | 25 566 records/s |

`multi_replica_test`：选主 + 1 次写复制 360 ms；多 voter 下 split 明确拒绝 298 ms。

## 3. 缺陷（实机复现，非测试夹具）

### P0 重启后空闲态读路径不恢复

```
put k1 v1            → OK
kill -9
重启                 → 就绪 516 ms
GET /kv?key=k1 ×12   → 全部 {"error":"IoFailed:applied 未追上确认位点"}（6 s 内不自愈）
/status              → leader=true, term=2, commit=1, applied=0, read_index_reads=0
put k2 v2            → OK
GET k2 / GET k1      → 均命中（applied 从 0 直接到 2）
```

数据没丢，是**无人写入时 pending 重放不推进**：`applied` 停在 0，ReadIndex 永远等不到追平。
纯读客户端在崩溃重启后会一直失败，直到有写入把管线踢活。

### P1 部署入口不支持多副本

`src/server/main.cpp` 只解析 `<data_dir> [status_port]`，没有 `voters`/`replica_id`/`peer_ports`。
`ServerOptions` 里已有字段，但 daemon 不暴露 → 3 节点只能靠测试夹具拉起。

### P1 HTTP 层单线程 + 无 keep-alive

一个 `io_context` 线程处理全部请求，每请求新建 TCP（CLI 侧 `Connection: close`）。
串行 190 ops/s 与并发 1400–1750 ops/s 的差距主要来自这里，不是引擎。

### P2 请求体格式未文档化

`/kv` 用制表符分隔（`put\tkey\tvalue`），见 `http_helpers.h::split_frame`；`docs/cli.md` 未写。

### P2 重定向 stdout 后拿不到就绪行

`std::cout` 全缓冲，`tinykv-server data 3401 > log` 时日志里没有"就绪"行，脚本化部署只能轮询端口。

## 4. 复现脚本

`build/live/{measure,crash,verify}.py`（构建目录内，不入库）。
