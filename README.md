# TinyKV-CPP

用现代 C++（C++20/23）复刻 [PingCAP Talent Plan TinyKV](https://github.com/tidb-incubator/tinykv) 四阶段能力
（单机 KV → Raft KV → Multi-Raft → Percolator 事务），工业级正确性/性能标准。



- 工程化

tinykv-server 组合根：config → TSO → Storage×N → Peer×regions → Worker 泵循环 → tcp_transport → StatusServer(handlers) + 信号触发优雅关闭

 Service 抽象 + region→peer 动态注册表（split 后动态建 peer 实例） 

会话作用域：连接级 TxnClient 状态（解锁 CLI 交互式事务）

 Interceptor 链（log/metrics/trace 插拔位）+ GC（活跃 ts 上报有了宿主）




## 仓库导航

| 文档 | 内容 |
|---|---|
| [TinyKV-CPP-设计思路与需求.md](TinyKV-CPP-设计思路与需求.md) | 需求规格（REL/FUNC/UX/PERF/ENG 编号体系）、架构、里程碑 M0–M5 |
| [docs/](docs/README.md) | TinyKV 白皮书四篇笔记（Project1–4）与分阶段规格 SPEC-0~4 |
| [docs/testing.md](docs/testing.md) | 测试策略唯一事实来源：登记表 + 一致性检查 SOP |
| [CI.md](CI.md) / [SPEC.md](SPEC.md) | TinyKV-CI 操作手册与规则（机器可读事实来源在 [ci/rules.toml](ci/rules.toml)） |



## 快速开始

```bash
python scripts/build.py        # 配置 → 构建 → 测试 一条龙（M0 落地后可用）
python tools/ci/run_ci.py      # 本地 CI（提交前必跑）
```



## 阶段规划（分支 = 阶段编号，规格 = docs/SPEC-x）

| 阶段 | 分支 | SPEC | 交付 |
|---|---|---|---|
| M0 基建 | `0` | [SPEC-0](docs/SPEC-0-foundation.md) | 骨架 + common 三库 + proto + CI + 测试登记机制 |
| M1 单机 | `1` | [SPEC-1](docs/SPEC-1-standalone.md) | Storage 引擎 + Raw API + CLI 本地 |
| M2 Raft | `2` | [SPEC-2](docs/SPEC-2-raft-kv.md) 上篇 | 3 节点 RaftKV + 快照压缩 |
| M3 读优化 | `3` | [SPEC-2](docs/SPEC-2-raft-kv.md) 下篇 | ReadIndex/LeaseRead + 读基准 |
| M4 Multi-Raft | `4` | [SPEC-3](docs/SPEC-3-multi-raft.md) | Split/ConfChange/调度器 |
| M5 事务 | `5` | [SPEC-4](docs/SPEC-4-transaction.md) | Percolator SI 全套 |
