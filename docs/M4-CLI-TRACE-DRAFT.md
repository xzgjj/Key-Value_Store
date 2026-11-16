# M4 第⑦b层契约核对草稿：trace region 的 --endpoint 依赖

> 状态：按"开关状态在 store 侧"落版，留痕以备用户追认。

## 1. 与已核对契约的差异（1 处）

`docs/cli.md` §10.1 原表：`tinykv-cli trace region <id> on|off`——**未带 --endpoint**。

实际实现：trace 开关与 trace 日志（`log/trace-region-<id>.log`）都存在于 **store 进程**，
CLI 是一次性进程，不传 `--endpoint` 就没有任何可操作的载体。故实现为：

```
tinykv-cli --endpoint <host:port> trace region <id> on|off
```

未提供 `--endpoint` 时返回退出码 1（用法错误），与其他集群观测命令一致。

## 2. 服务端新增端点（AdminHandler，docs/cli.md §10 未细化部分）

| 方法 | 路径 | 输入 | 输出 |
|---|---|---|---|
| GET | /regions | - | region 数组 JSON |
| GET | /region?id=N | 十进制正整数 | 单 region 详情（不存在 → 404 → CLI 退出码 3） |
| GET | /region/peers?id=N | 十进制正整数 | 副本数组（store_id:voter\|learner[:leader]） |
| GET | /scheduler/status | - | 调度器视图（stores/regions/splits/next_region_id） |
| POST | /region/split | `split\t<id>\t<key>` | `OK new_region_id=N` / `ERR ...`（退出码 2） |
| POST | /trace | `trace\t<id>\ton\|off` | `OK trace on\|off` / `ERR region-not-found` |

- 端点复用 `StatusServer` 的极简 HTTP（D2-6 不引框架约定不变），handler 由宿主注入
  （与 /status、/kv 同一装配模式）；M4 内由测试装配，未来 daemon 化时同一签名直接复用。
- `region split` 服务端链路：AskSplit 预分配（⑥）→ admin 提案（③）→ 共识 → apply 切分
  （红线1/2）→ 路由更新。开区间外 key 返回 `ERR <InvalidArgument 描述>`。

## 3. trace 落盘规格（§10.3 已定，实现照此）

`log/trace-region-<id>.log`，spdlog rotating：单文件 ≤10MB、保留 ≤5 个（CLAUDE.md §4 同规格）。
开关重复切换不重复建 sink（避免绕过保留窗口）；未开启 region 的 trace 写入零开销丢弃。

## 4. 已知限制（记入 notes-m4）

- trace 的**事件埋点**（apply/慢日志/leader 变更等 region 粒度事件写入该文件）尚未接入
  Peer 内部路径——本层交付的是开关 + 落盘通道 + 开关事件；埋点随 daemon 化与 UX-04 演练评估补。
- `region <id>` 详情中的 leader/term/commit 取自注入 handler 的 Peer；多 region 多 peer 的
  完整映射需 daemon 化时的 Peer→region 注册表（M4 测试内单 region 简化）。

## 5. 待用户追认项

1. `trace` 是否补 `--endpoint`（本实现已按补齐落版，如需维持原契约需改为本地 IPC 方案）。
2. `region split` 成功回执格式 `OK new_region_id=N` 是否沿用（当前为文本协议，与 /kv 一致）。
