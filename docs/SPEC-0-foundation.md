# SPEC-0 — M0 工程基建（Foundation）

> 上游：设计文档 §3.6（ENG-01~05）、§4.3/4.5；下游：SPEC-1~4 的公共载体。
> 状态：**待实现**（代码 0 行）。登记表联动：本文 §0.7 引用的测试名落地时必须登记进 [testing.md](testing.md) §4。

## 0.0 里程碑 ⇄ SPEC 映射（阶段推进总览）

设计文档定义 6 个里程碑（M0–M5），SPEC 限 2–5 份，故 **M2 与 M3 合册 SPEC-2**（册内两阶段各自独立验收，
见 SPEC-2 §2.0 阶段划分），其余逐一对应：

| 里程碑 | SPEC | 交付一句话 | 完成判定位置 |
|---|---|---|---|
| M0 基建 | SPEC-0（本册） | 骨架+三库+CI+登记机制 | §0.7 |
| M1 单机 | SPEC-1 | Storage 引擎+Raw API+CLI 本地 | §1.7 |
| M2 Raft | SPEC-2 上篇（§2.1–2.5 M2 部分） | 3 节点 RaftKV+快照压缩 | §2.7 M2 表 |
| M3 读优化 | SPEC-2 下篇（§2.6 M3 部分） | ReadIndex/LeaseRead+读基准 | §2.7 M3 表 |
| M4 Multi-Raft | SPEC-3 | Split/ConfChange/调度器 | §3.7 |
| M5 事务 | SPEC-4 | Percolator SI 全套 | §4.7 |

**全局需求覆盖矩阵见 §0.9**——设计文档 §3 全部 44 项需求逐条映射到 SPEC 验收行，该矩阵是"是否涵盖所有需求"的唯一对账依据。

## 0.1 功能范围

| 交付物 | 内容 | 需求映射 |
|---|---|---|
| 工程骨架 | 仓库根即工程根：`proto/ src/ tests/ tools/ docs/`；CMake ≥3.22 + presets + `scripts/build.py` 统一 build 入口 | ENG-02 |
| 依赖清单 | vcpkg 锁版：protobuf、asio、spdlog、gtest、unordered_dense | ENG-02/03 |
| 强类型 ID 库 | `Term/LogIndex/RegionId/PeerId/StoreId/ClientId/SeqNo/Ts` 禁隐式互转 | ENG-01（清单 #2/#10） |
| 错误基建 | `KvError`（variant 结构化错误）+ `result<T>` 别名，全接口 `[[nodiscard]]` | ENG-01（#5/#6）、UX-03 |
| 编码工具 | CF 前缀编码、事务时间戳编码（大端，保持字节序=比较序） | FUNC-01/13 前置 |
| proto 骨架 | `raft.proto`（七字段消息）、`kv.proto`（Raw+Txn）、`raft_cmdpb.proto`（requests[] 批量一等公民） | FUNC-03/04/13 前置 |
| 测试四层接入 | `add_tinykv_test()` CMake 封装；`tools/check_test_registry.py` 进 CI 前置步骤 | testing.md §2/§7 |
| CI | Windows+Linux 双平台矩阵：登记检查 → 静态检查 → 编译 → `ctest -L fast`；夜跑 job 骨架 | ENG-02/04、ENG-05 载体 |

## 0.2 架构

```
TinyKV-White-Paper-main/            # 仓库根 = 工程根（决策 D0-1，见 §0.8）
├── proto/                          # 消息定义即接口文档
│   ├── raft.proto  raft_cmdpb.proto  kv.proto
├── src/
│   ├── common/     # 强类型 ID、KvError、编码、spdlog 封装 ← 被所有层依赖
│   ├── storage/ wal/ raft/ raftstore/ scheduler/ txn/ server/ client/ cli/   # 按里程碑增量创建（CLAUDE.md §10），禁一次建空树
├── tests/{testlib,unit,simulation,integration,bench}/   # 布局与登记见 testing.md
├── tools/check_test_registry.py    # 登记表 ⇆ 仓库一致性检查（CI 第一步）
└── scripts/build.py                # 配置+构建+测试统一入口（presets 封装）
```

**模块依赖方向（无环）**：`common ← {storage, wal, raft} ← raftstore ← {scheduler, txn, server} ← client/cli`。
proto 生成物归 `proto/` 目标，所有层可依赖；common 不依赖任何上层。

## 0.3 接口设计

### 0.3.1 强类型与错误（common 库，全部定版于 M0）

```cpp
namespace tinykv {

// 强类型 ID：禁止隐式与 uint64 互转（清单 #2/#10）；比较/哈希可用，算术禁止。
template <typename Tag>
struct StrongId {
    uint64_t value{};
    constexpr explicit StrongId(uint64_t v) noexcept : value(v) {}
    friend constexpr bool operator==(StrongId a, StrongId b) = default;
    friend constexpr auto operator<=>(StrongId a, StrongId b) = default;
};
struct TermTag; struct LogIndexTag; struct RegionIdTag; struct PeerIdTag;
struct StoreIdTag; struct ClientIdTag; struct SeqNoTag; struct TsTag;
using Term = StrongId<TermTag>;        using LogIndex = StrongId<LogIndexTag>;
using RegionId = StrongId<RegionIdTag>; using PeerId = StrongId<PeerIdTag>;
using StoreId = StrongId<StoreIdTag>;   using ClientId = StrongId<ClientIdTag>;
using SeqNo = StrongId<SeqNoTag>;       using Ts = StrongId<TsTag>;   // 事务时间戳

// 结构化错误（UX-03 三要素的载体：what/why/hint 全在字段里）
struct KeyNotInRegion { std::string key; RegionId region_id; };
struct StaleEpoch     { RegionId region_id; RegionEpoch have; RegionEpoch want; };
struct NotLeader      { RegionId region_id; std::optional<PeerId> leader_hint; };
struct IoFailed       { std::string path; std::string reason; };
struct Corrupted      { std::string where; std::string reason; };
struct InvalidArgument{ std::string what; };
using KvError = std::variant<KeyNotInRegion, StaleEpoch, NotLeader,
                             IoFailed, Corrupted, InvalidArgument>;
template <typename T> using result = std::expected<T, KvError>;

enum class Cf : uint8_t { Default = 0, Write = 1, Lock = 2 };   // P4 直接复用

// 编码：CF 前缀（白皮书 P1 继承）；key 组内保序 → 前缀扫描可用
result<std::string> encode_key(Cf cf, std::span<const std::byte> user_key);
result<std::string> encode_ts_key(std::span<const std::byte> user_key, Ts ts);  // 大端 ts 前缀
}
```

**KvError 触发条件矩阵**（UX-03"三要素"按变体评审；M2 起错误清单逐条核对 what/why/hint）：

| 变体 | 触发条件 | what | why | hint（修复线索） |
|---|---|---|---|---|
| KeyNotInRegion | key 不落任何 region range | key | 路由过期/未 split | 请求新路由（回源调度器） |
| StaleEpoch | 请求 epoch 落后 region 当前 epoch | 拒绝事实 | 路由缓存过期 |携带 have/want 供刷新 | 
| NotLeader | 请求打到非 leader peer | 非 leader | leader 已变 | leader_hint 供直接重定向 |
| IoFailed | 文件读写/flush 失败 | path | OS 错误原因 | 检查磁盘/权限 |
| Corrupted | CRC 校验失败 | 位置 | 数据损坏 | 恢复流程/快照追赶 |
| InvalidArgument | 参数越界（如空 key、limit<0） | 字段名 | 调用方错误 | 按字段名修正 |

### 0.3.2 raft.proto 核心消息（七字段复用，白皮书消息集对齐）

| MessageId | 方向 | 字段（复用集合） | 语义 |
|---|---|---|---|
| MsgHup | 内部 | — | 选举超时触发竞选 |
| MsgBeat | 内部 | — | leader 心跳计时维持 |
| MsgPropose | leader | entries[] | 客户端提案（批量 entries 一等公民） |
| MsgAppend | leader→f | term, index(prev), log_term(prev), entries[], commit | 追加日志 |
| MsgAppendResp | f→leader | term, index, reject, log_term(冲突term), commit | 拒绝时带冲突 term 供整段回退 |
| MsgRequestVote | c→all | term, index(last), log_term(last) | 拉票 |
| MsgRequestVoteResp | all→c | term, reject | 拒绝时不重置对方计时器 |
| MsgHeartbeat | leader→f | term, commit | **只带元数据**（PERF-08 红线） |
| MsgHeartbeatResp | f→leader | term, index | ReadIndex quorum 确认载体（M3） |
| MsgSnapshot | leader→f | term, index, snapshot 元 | 快照模式入口 |
| MsgInstallSnapshot(M3快照流) | leader→f | 分块流 | 走独立 SnapshotStream 通道 |

`raft_cmdpb.proto`：`RaftCmdRequest{ header{region_id,epoch,peer}, requests[]（Get/Put/Delete/Scan/ReadIndex）, admin（Split/CompactLog/…） }` —— **requests 数组从第一天支持批量**（白皮书作者 QPS 0 分教训）。

## 0.4 核心逻辑骨架

```cmake
# cmake/AddTest.cmake —— 一切测试的唯一注册入口（禁止散装 add_test）
function(add_tinykv_test NAME)
  cmake_parse_arguments(T "" "" "SRCS;LIBS;ARGS;ENV" ${ARGN})
  add_executable(${NAME} ${T_SRCS})
  target_link_libraries(${NAME} PRIVATE tinykv_common gtest gtest_main ${T_LIBS})
  add_test(NAME ${NAME} COMMAND ${NAME} ${T_ARGS})
  set_tests_properties(${NAME} PROPERTIES ENVIRONMENT "${T_ENV}")
endfunction()
# Level→LABEL、TIMEOUT、RUN_SERIAL、SKIP_RETURN_CODE 由调用方以 PROPERTIES 补充；
# 登记表（docs/testing.md §4）为唯一事实来源，本函数只做投影。
```

CI 步骤顺序（证据链的生成线，每步绿才算过）：

```
1 python tools/check_test_registry.py            # 登记对账（遗漏在编译前失败）
2 clang-format --dry-run -Werror && clang-tidy -p build   # 静态检查（ENG-04 禁模式的载体之一）
3 grep -rE "std::(async\(.+detached|thread\(.+detach\(\)\))|new .*\[\]|sleep_for.*启动对齐" src/ && exit 1 || true
                                                  # ENG-04 违禁模式扫描（detach/裸数组 new/sleep 对齐启动）
4 cmake --preset <os>-debug && cmake --build --preset <os>-debug
5 ctest -L fast --output-junit build/evidence/junit.xml
6 (夜跑) ctest -L nightly                          # 150 轮纪律 + bench 采数
```

## 0.5 红线

1. 编码必须**大端**：字节序 = 比较序 = Scan 迭代序（白皮书 P1 核心技巧）；
2. StrongId 不提供 `operator uint64_t()`，需要裸值必须显式 `.value`（事故源可 grep）；
3. `result<T>` 永不省略 `[[nodiscard]]`；错误变体新增字段 = 接口变更，须过 13 条清单评审；
4. proto 文件即接口文档：字段注释必须写清语义/单位/可空性；proto 契约变更先与用户核对（CLAUDE.md §15）；
5. CI 第一步必须是登记检查——遗漏测试在编译前就失败；
6. `src/` 子目录按里程碑增量创建，禁止一次建空树（CLAUDE.md §10）。

## 0.6 示例

```cpp
// tests/unit/common/encode_test.cpp —— M0 首批测试的形态（红→绿）
TEST(EncodeKey, CfPrefixKeepsOrderWithinCf) {
  auto a = tinykv::encode_key(tinykv::Cf::Default, std::as_bytes(std::span{"abc", 3}));
  auto b = tinykv::encode_key(tinykv::Cf::Default, std::as_bytes(std::span{"abd", 3}));
  ASSERT_TRUE(a && b);
  EXPECT_LT(*a, *b);                       // 字节序=比较序
}
TEST(EncodeKey, CfIsolation) {             // 不同 CF 同 key 不冲突
  auto d = tinykv::encode_key(tinykv::Cf::Default, std::as_bytes(std::span{"k", 1}));
  auto w = tinykv::encode_key(tinykv::Cf::Write,    std::as_bytes(std::span{"k", 1}));
  ASSERT_TRUE(d && w);
  EXPECT_LT(*d, *w);                       // cf_id 前缀定序，三 CF 互不可见
}
TEST(StrongId, NoImplicitConversion) {     // 负例思想：类型即注释
  tinykv::Term t{5};  tinykv::LogIndex i{5};
  // EXPECT_FALSE(t == i);               // 编译期即不可比较（不同 Tag 的 StrongId 无 operator==）
  EXPECT_EQ(t, tinykv::Term{5});
}
```

## 0.7 证据链规范（全局，SPEC-1~4 复用）

任何功能/修复宣称"完成"，**四环节缺一不可**：

```
[需求编号] ──→ [SPEC 验收标准] ──→ [测试登记名] ──→ [证据物]
(设计文档§3)   (本系列 §x.7 表)   (testing.md §4)   (CI 日志/JUnit/bench JSON/150轮报告)
```

| 环节 | 载体 | 谁维护 |
|---|---|---|
| 需求编号 | 设计文档 §3（REL/FUNC/UX/PERF/ENG，共 44 项） | 用户资产，只读 |
| 验收标准 | SPEC-x.7 表：可判定口径（"≥/≤/等于/存在"） | 随 SPEC 修订（留痕） |
| 测试登记 | testing.md §4 行；ctest 名全局唯一 | 实现者（登记先行） |
| 证据物 | CI artifacts：`build/evidence/{junit.xml, bench/*.json, nightly/*.txt}`、排障演练记录 | CI 自动产出 |

**判定语汇**：`PASS = 证据物存在且口径达标`；缺证据物或口径未达 = 未完成，不得标记里程碑进度。

### M0 验收表

| 需求 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| ENG-02 | 双平台 CI 全绿 | — | CI 矩阵 | workflow run 绿勾 |
| ENG-03 | spdlog/gtest 装配可用 | `common_log_test` | `ctest -R common_log_test` | junit.xml |
| ENG-01 | 编码/ID/错误三库过 13 条清单 | `encode_test` `strong_id_test` `kv_error_test` | `ctest -L fast` | junit.xml + 评审记录 |
| ENG-04 | CI 违禁模式扫描步骤存在且当前 0 命中 | —（CI 步骤 3） | CI | 扫描步骤绿 |
| UX-03 | KvError 六变体三要素矩阵评审通过（§0.3.1 表） | `kv_error_test`（hint 字段断言） | `ctest -R kv_error_test` | junit.xml + 评审记录 |
| testing.md 机制 | 登记遗漏→CI 失败（人为制造一次遗漏并复现失败） | `check_test_registry`（CI 步骤） | 见 testing.md §7 | 失败演练日志（一次性） |
| UX-06 | M0 实现笔记一篇（基建决策/环境基线回填） | —（人工） | — | `docs/notes-m0.md` |
| ENG-05 载体 | 夜跑 job 可调度 | — | `ctest -L nightly`（空集通过） | workflow 配置 |

## 0.8 开放决策点（需用户批准）

| # | 决策 | 建议默认 | 影响 |
|---|---|---|---|
| D0-1 | 仓库根即工程根（放弃设计文档 §4.3 的 `tinykv/` 前缀） | 是 | tests/tools/docs 已在根且登记表路径以根为基准 |
| D0-2 | 包管理 | vcpkg（Conan 备选） | ENG-02 |
| D0-3 | CMake presets + build.py 双入口 | 是 | UX-05 前置 |
| D0-4 | 三方库版本 pin 表 | M0 定版（见 CLAUDE.md §7-5） | 后续升级走补充式变更 |
| D0-5 | ENG-04 违禁模式用 CI grep 扫描 | 是 | 简单可审计；clang-tidy 规则 M0 一并配置 |

## 0.9 全局需求覆盖矩阵（对账唯一依据）

> 设计文档 §3 全部 44 项 → SPEC 验收行逐条映射。**任何一项找不到 SPEC 行 = 规划遗漏**（修改走补充式）。

| 需求 | SPEC 验收位置 | 需求 | SPEC 验收位置 |
|---|---|---|---|
| FUNC-01 | §1.7 行1/2 | PERF-01 | §2.7-M2 行 PERF-01 |
| FUNC-02 | §1.7 行3（同 REL-02） | PERF-02 | §2.7-M2 行 PERF-02 |
| FUNC-03 | §2.7-M2 行 FUNC-03(选举/复制/压缩) | PERF-03 | §2.7-M3 行 PERF-03 |
| FUNC-04 | §2.7-M2 行 FUNC-04 | PERF-04 | §2.7-M3 行 PERF-04 |
| FUNC-05 | §2.7-M3 行 REL-01 | PERF-05 | §2.7-M2 行 PERF-05 |
| FUNC-06 | §2.7-M3 行 FUNC-06/PERF-04 | PERF-06 | §2.7-M2 行 PERF-06 |
| FUNC-07 | §3.7 行1 | PERF-07 | §3.7 行 PERF-07 |
| FUNC-08 | §3.7 行2 | PERF-08 | §2.7-M2 + §3.7（MR 规模化） |
| FUNC-09 | §3.7 行3 | PERF-09 | §3.7 行 PERF-09 |
| FUNC-10 | §3.7 行4 | UX-01 | §2.7-M2 行 UX-01 |
| FUNC-11 | §3.7 行5 | UX-02 | §1.7(本地)+§2.7-M2(集群)+§4.7(txn) |
| FUNC-12 | §3.7 行6 | UX-03 | §0.7（矩阵评审）+ §2.7-M2（清单核对） |
| FUNC-13 | §4.7 行1 | UX-04 | §2.7-M2（端点）+ §3.7（排障演练完整） |
| FUNC-14 | §4.7 行2 | UX-05 | §1.7 行 UX-05 |
| FUNC-15 | §4.7 行3 | UX-06 | §0.7/§1.7/§2.7/§3.7/§4.7 末行（每里程碑一篇笔记） |
| FUNC-16 | §4.7 行4 | UX-07 | §1.7 行 UX-07 |
| FUNC-17 | §4.7 行 GC(P2) | ENG-01 | §0.7 行 ENG-01（+全程评审） |
| REL-01 | §2.7-M3 行 REL-01 | ENG-02 | §0.7 行 ENG-02 |
| REL-02 | §1.7（WAL 层）+§2.7-M2（Raft 层） | ENG-03 | §0.7 行 ENG-03 |
| REL-03 | §3.7 行5（同 FUNC-11） | ENG-04 | §0.7 行 ENG-04 |
| REL-04 | §3.7 行3（同 FUNC-09） | ENG-05 | §2.7-M2 / §3.7 / §4.7 各 150 轮行 |
| REL-05 | §3.7 行1/2 | — | — |
| REL-06 | §4.7 行1/端到端 | — | — |
