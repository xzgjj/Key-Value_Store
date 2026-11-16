# tinykv-cli 使用契约

> **状态：本地模式 + M2 集群命令已与实现对齐；§10 的 region 观测命令契约定版、实现随 M4 推进**。
> 本文件是 CLI 的对外契约：子命令、参数、退出码与配置加载方式以本文为准；
> 改动任一子命令或退出码必须先改本文、再改实现（反之视为文档漂移）。
> 集群模式（M2 起）在本文追加"集群子命令"一节，不推翻现有本地语义。
> M4 region 观测命令经用户 2026-11-16 核对定版（见 `docs/M4-CONTRACT-DRAFT.md`）。

## 1. 入口模式

| 模式 | 调用形态 | 说明 |
|---|---|---|
| 一次性执行 | `tinykv-cli <命令> [参数]` | 每条命令独立进程、独立打开引擎；适合脚本 |
| 集群模式 | `tinykv-cli --endpoint <host:port> <status|get|put|del>` | 状态走 `/status`，读写走 `/kv`；`scan` 未开放 |

无交互式 shell、无常驻子命令——单机场景下"每条命令一个进程"最简单也最不容易留下不一致状态。

## 2. 命令列表

| 命令 | 作用 | 是否需要引擎 |
|---|---|---|
| `put <key> <value>` | 写入（同 key 覆盖） | 是 |
| `get <key>` | 读取；不存在时退出码 3 | 是 |
| `del <key>` | 删除；删不存在的 key 视为成功（幂等） | 是 |
| `scan [start]` | 按 key 升序扫描，`--limit` 截断 | 是 |
| `status` | 本地模式打印生效配置；带 `--endpoint` 时取集群节点 `/status` | 本地：否；集群：是 |
| `--help` | 打印帮助 | 否 |

## 3. 语法与参数

全局选项可放在命令前后任意位置，均作用于本次调用。

| 选项 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `--config <path>` | 文件路径 | `demo.yaml` | 配置文件；不存在即报错，不回落默认值 |
| `--cf <default\|write\|lock>` | 三个列族之一 | `default` | 事务列族在 M5 才有写入方，此处仅暴露选择能力 |
| `--limit <n>` | 正整数 | `10` | 仅 `scan` 生效 |
| `--endpoint <host:port>` | 节点地址 | 空（本地模式） | 非空即进入集群模式，当前只支持 `status` |

`scan [start]` 的 `start` 为闭区间起点；省略表示从该列族起点开始。

## 4. 退出码契约

脚本化调用只能依赖退出码，因此它是契约的一部分，不允许随意变更。

| 码 | 含义 | 典型触发 |
|---|---|---|
| 0 | 成功 | 写入/删除成功、读到值、扫描完成、status/help |
| 1 | 用法错误 | 参数个数不对、未知命令、未知列族或选项缺值 |
| 2 | 运行错误 | 配置文件打不开或语法非法、引擎打开失败、WAL/SST 读写失败 |
| 3 | key 不存在 | `get` 未命中（stdout 无内容，stderr 打印 `NOT_FOUND`） |

## 5. 配置加载机制

- **单文件、显式路径**：只有 `--config` 一个入口，没有多级 profile 合并、没有环境覆盖、没有隐式搜索路径。
  理由是"配置从哪来"必须唯一可判定，否则排障时第一件事就变成猜配置。
- **最小 YAML 子集**：一层分组 + `键: 值`，`#` 起注释，值可用引号包裹。数组、多行字符串、锚点一律**报错**。
- **未知键报错**：拼错的配置项会让启动失败，而不是静默忽略（静默失效比启动失败更难查）。
- **文件不存在报错**：不提供"缺省配置兜底"，避免以为改生效了其实读的是另一份。

配置项语义见仓库根 `demo.yaml` 的逐项注释（配置即文档）。

## 6. 插件管理

**当前不支持，且短期内不引入。** CLI 的扩展点设计为"新增子命令"而非运行期加载插件：
单机工具的插件机制会带来 ABI 兼容、版本协商与信任边界三类问题，收益与规模不匹配。
若后续确需扩展（如 M4 调度器诊断命令），走"新增子命令 + 编译期接入"。

## 7. 集群模式（M2）

`--endpoint` 非空即切到集群模式：读该节点 HTTP `/status` 并原样打印 JSON，便于脚本消费。

写入走 `POST /kv`，请求体是制表符分隔的简单文本（`put\tkey\tvalue` / `del\tkey`）。
刻意不引 JSON 库：CLI 只需表达两三种写操作，文本协议两端都能一眼看懂、也便于用 curl 手测。
服务端在 apply 后按 (client, seq) 回执，CLI 收到 `OK` 才算成功——写成功即已提交，不是"已发出"。

`scan` 在集群模式下**未开放**（退出码 1）：范围读要处理分页游标与 region 边界，
留到 M4 多 region 一起做。

`/status` 返回字段（M2）：`peer_id`、`is_leader`、`term`、`committed`、`last_index`。
`/status` 端口独立于 Raft 帧端口（SPEC-2 D2-6 原意是复用，偏差原因见 implementation-notes）。

示例：

```bash
tinykv-cli --endpoint 127.0.0.1:3380 status
```

## 8. 环境变量与权限

| 项 | 现状 |
|---|---|
| 环境变量 | 无。所有行为由命令行与配置文件决定，便于复现 |
| 文件权限 | 依赖操作系统对数据目录的权限；CLI 内部不做鉴权 |
| 网络暴露 | 无。本地模式不经网络，不使用端口 |

集群模式引入 RPC 后，鉴权与端口将成为必须解决的问题，届时在此表补入。

## 9. 可直接复制执行的示例

安装与构建（Windows MSYS2 与 Linux 均为同一组命令）：

```bash
python scripts/build.py --preset win-debug
```

初始化演示库并确认配置生效：

```bash
tinykv-cli --config demo.yaml status
```

日常操作：

```bash
tinykv-cli --config demo.yaml put user:1 alice
tinykv-cli --config demo.yaml get user:1
tinykv-cli --config demo.yaml scan user: --limit 20
tinykv-cli --config demo.yaml del user:1
```

脚本化调用（用退出码判定，不解析输出文本）：

```bash
if tinykv-cli --config demo.yaml get user:1 >/dev/null 2>&1; then
  echo "存在"
else
  case $? in
    3) echo "不存在" ;;
    *) echo "调用失败" >&2 ;;
  esac
fi
```

批量导入（每条一次调用，适合几千行以内的初始化）：

```bash
while IFS=$'\t' read -r k v; do
  tinykv-cli --config demo.yaml put "$k" "$v" || break
done < seed.tsv
```

---

## 10. Region 观测命令（M4，契约已定版，实现随 M4 对应层交付）

> 用户 2026-11-16 核对通过（证据 `docs/M4-CONTRACT-DRAFT.md`）。这些命令**尚未实现**，
> 随 M4 第②~⑦层逐步交付；未实现前调用返回退出码 1（用法错误）。

### 10.1 命令列表

| 命令 | 作用 | 交付层 |
|---|---|---|
| `tinykv-cli --endpoint <host:port> regions` | 列出该 store 全部 region | 第②层 |
| `tinykv-cli --endpoint <host:port> region <id>` | 单个 region 详情（range/leader/term/commit/apply_lag/size_hint/epoch） | 第②层 |
| `tinykv-cli --endpoint <host:port> region peers <id>` | 副本分布（每 peer：store_id / voter\|learner / 是否 leader） | 第④层 |
| `tinykv-cli --endpoint <host:port> region split <id> <key>` | 手动触发 split（调试用，走 admin 提案） | 第③层 |
| `tinykv-cli --endpoint <host:port> scheduler status` | 调度器状态（节点数/region 数/待迁移队列/最近动作） | 第⑥层 |
| `tinykv-cli trace region <id> on\|off` | region 级 trace 开关 | 第⑦层 |

### 10.2 参数与语义

- `<id>`：RegionId（十进制正整数）。非法格式 → 退出码 1。
- `region split <id> <key>` 的 `<key>` 必须落在 `(start_key, end_key)` 开区间，否则服务端回 `StaleEpoch`/`InvalidArgument`。
- 退出码沿用 §4（0/1/2/3），不新增。

### 10.3 trace 日志位置

`trace region <id> on` 后，region 粒度的 trace 日志写 `log/trace-region-<id>.log`
（spdlog 轮转：单文件 ≤10MB、保留 ≤5 个，CLAUDE.md §4 硬约束）。

### 10.4 典型示例（实现后可直接复制执行）

列出 region：

```bash
tinykv-cli --endpoint 127.0.0.1:3380 regions
```

查看单个 region 与其副本分布：

```bash
tinykv-cli --endpoint 127.0.0.1:3380 region 7
tinykv-cli --endpoint 127.0.0.1:3380 region peers 7
```

手动触发 split（调试）：

```bash
tinykv-cli --endpoint 127.0.0.1:3380 region split 7 user:m
```

调度器状态：

```bash
tinykv-cli --endpoint 127.0.0.1:3380 scheduler status
```

开启 region trace 后复现问题：

```bash
tinykv-cli trace region 7 on
# ... 复现操作 ...
tinykv-cli trace region 7 off
# 日志在 log/trace-region-7.log
```
