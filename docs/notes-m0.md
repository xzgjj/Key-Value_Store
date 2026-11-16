# M0 实现笔记（notes-m0）

> UX-06 交付物：每完成一个阶段写一篇实现笔记，记录**为什么这样做**而不是**做了什么**（做了什么看
> `git log` 与 `implementation_plan.md`）。本笔记对应 SPEC-0 / 分支 `0`。
>
> 读法建议：只想知道"M0 能不能用"看 §2；想知道"当初为什么这么定"看 §3；想接着干看 §7。

## 1. 这篇笔记要回答的三个问题

1. 一套还没有业务代码的工程，凭什么说它"做完了"？
2. 哪些决定是**不可逆**的（改起来贵），哪些是可随时调的？
3. 本地能跑绿，能不能说明 CI 也能绿？

## 2. M0 交付与验收对照

SPEC-0 §0.7 的八行验收，逐行落到证据：判定语汇沿用该节规定——证据物存在且口径达标才算 PASS。

| 验收行 | 口径 | 证据 |
|---|---|---|
| ENG-01 三库过评审清单 | 编码 / 强类型 ID / 结构化错误可用且负例编译不过 | `encode_test`、`strong_id_test`、`kv_error_test` 全绿；评审见 §4 |
| ENG-02 双平台 CI | Linux 与 Windows 各有一条流水线 | `.github/workflows/ci.yml` 三 job（远端首跑待验证） |
| ENG-03 日志与测试框架 | spdlog 装配可用、gtest 锁版 | `common_log_test`；gtest FetchContent 锁 v1.18.0 |
| ENG-04 违禁模式零命中 | detach / 裸数组 new / sleep 对齐启动 | `align` 阶段的 banned 检查，11 个源文件 0 命中 |
| UX-03 错误三要素 | 六变体字段可承载 what/why/hint | `kv_error_test` 逐变体断言字段可达 |
| testing 机制 | 登记遗漏会让 CI 失败 | registry 检查在编译前执行；登记表与仓库双向对账 |
| UX-06 实现笔记 | 本篇 | 本文件 |
| ENG-05 载体 | 夜跑 job 可调度 | workflow nightly job，`ctest -L nightly` 空集通过 |

## 3. 不可逆与可逆的决定（这张表比清单重要）

**不可逆的**（改动会波及全仓，成本随时机指数上升）：

- **仓库根即工程根**（D0-1）。放弃设计文档里的 `tinykv/` 前缀——因为 tests/、tools/、docs/
  本来就在根，登记表的 Scope 以根为基准；一旦改成子目录，所有登记行、CI 路径、脚本全要改。
- **测试登记表是唯一事实来源**。不是"顺便记一下"，而是 CI 第一步就校验。这样"忘了登记"会在
  编译前暴露，而不是在三个月后没人知道某个测试为什么存在。
- **错误通道统一走 `std::expected` + 六变体**。新增变体或加字段等于接口变更，须过评审；
  这是为了防止错误语义在 M2 之后各处自行扩展。
- **proto 三文件的消息集**。七字段复用的代价是字段语义随 `id` 变化，好处是消息类型不膨胀；
  换掉它等于重写 M2 的整个 Step 分支。

**可逆的**（调参数即可，不必开会）：

- 阈值类（函数行数、重复块窗口、diff 预算）——改 `ci/rules.toml`，收紧自动放行、放松要批准。
- preset 名、构建目录、日志轮转大小。
- 依赖引入方式（本机 pacman / CI vcpkg / FetchContent），只要 CMake 侧 `find_package` 不变。

## 4. 评审记录（ENG-01 / UX-03）

按工作区 API 设计原则 §10 的十三条逐条过三个库，结论只记"哪几条真的被触发了"：

| 条目 | 结论 | 落点 |
|---|---|---|
| #2 同类型参数交换是否报错 | 过——八个 ID 跨 Tag 比较、隐式转换、算术全部编译不过 | `strong_id_test` 用标准概念做负例断言 |
| #4 有没有 ptr+len 分开传 | 过——key 一律 `std::span<const std::byte>` | `encode.h` |
| #5/#6 失败是否可能被静默丢弃 | 过——`result<T>` 别名统一带 `[[nodiscard]]` | `error.h` |
| #10 单位/语义是否只写在名字里 | 过——`Term`/`LogIndex`/`Ts` 都是类型 | `ids.h` |
| #1 调用点能否看出实参含义 | **未过**——`log::init(dir, base, console)` 第三个参数是裸 bool | 已记入待办，M1 改 Options 聚合 |
| #11 新增参数是否逼所有调用点改 | **未过**——同上，加一个日志选项就要改所有调用 | 同上 |

两条未过项有意留到 M1：现在调用点少，过早抽象反而增加表面积。

## 5. 环境基线（实测值，不是文档传抄）

| 项 | 值 |
|---|---|
| 编译器 | MSYS2 ucrt64 g++ 16.2.0（C++23） |
| 构建 | CMake 3.30.3 + Ninja 1.13 |
| 依赖 | 本机 pacman：spdlog / protobuf 36.2 / asio / unordered_dense；gtest 走 FetchContent |
| 工具 | Python 3.13、clang-format 22、clang-tidy 22、git 2.55 |
| 缺口 | 本机**无 MSVC、无 vcpkg**——故 CI 的 Windows 线改用 MSYS2，与本机同源 |

## 6. 踩过的坑（按"再犯一次会浪费多久"排序）

1. **Git for Windows 的旧 libstdc++ 抢占**：可执行文件在 PATH 上先找到异源 DLL，启动即
   `0xc0000139`。对策是 MinGW 下 `-static` 链接，让产物自包含。
2. **protobuf 的导入目标目录不可跨作用域**：在子目录里 `find_package` 拿到的位置信息出不了作用域。
   对策是把依赖解析提到根 CMakeLists。
3. **测试进程的 DLL 搜索路径不能指望调用方 shell**：改为在注册测试时注入 `ENVIRONMENT_MODIFICATION`，
   测试自己负责环境，`ctest` 无需前置 PATH。
4. **点目录是天然的工具噪声区**：三方库自带的 `.md` 一旦被文档检查器扫到就会被计入预算与查重。
   把 FetchContent 产物统一放 `build/.deps`（点目录）后，规则资产一行都不用改。
5. **"绿"的含义要写死**：本地 CI 曾是九项检查，其中 `build` 阶段只跑 `-L fast`；格式检查不在本地跑。
   于是出现过"本地全绿但格式不合规"的情况——这不是检查错，是**检查范围与 CI 流水线不一致**。
   现在格式检查进了流水线，本地跑 `align` 可提前发现同类问题。

## 7. 遗留与交接

- `.github/workflows/ci.yml` **尚未远端首跑**：本地无 Actions 环境，只能证明等价命令在本机绿。
- 依赖双通道（本机 pacman / CI 清单）是环境适配的偏差，非设计选择，待用户追认后写入决策记录。
- proto 契约仍有 `RaftCmdResponse.cmd_responses` 为占位字节序列，M2 定版时会变更该字段——
  因为尚无任何代码依赖它，现在改的成本最低。
- `cf` 字段在 `kv.proto` 是枚举、在 `raft_cmdpb.proto` 是字符串，属同一概念的两种建模，
  两层映射关系未定义。这是 M1 开工前要与用户核对的第一件事。
