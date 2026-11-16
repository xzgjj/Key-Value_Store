# SPEC-1 — M1 单机存储引擎（Standalone，白皮书 Project1）

> 上游：SPEC-0（基建）；需求：FUNC-01/02、REL-02、UX-02(本地)/05/06/07、PERF-01/02/05（S0 基线）、ENG-03/04。
> 状态：待实现。登记表联动：§1.7 测试名实现时登记进 [testing.md](testing.md) §4。

## 1.1 功能范围

- `Storage` 抽象接口 + 自研引擎：内存跳表 MemTable + WAL 追加 + 只读 SST 分层（LSM 雏形，**无 compaction**）；
- CF（Default/Write/Lock）前缀编码与范围迭代 `Scan(start, limit)`；
- Raw API：Get/Put/Delete/Scan（单进程可用）+ `tinykv-cli` 本地模式 + 单文件 YAML 配置（UX-05）；
- 掉电恢复：WAL 重放，fsync 可配（开/关对比，REL-02 首次落地）。

## 1.2 架构（P1 切片）

```
tinykv-cli ──► RawServer(进程内直调) ──► Storage 接口 ──► Engine
                                             │            ├─ MemTable（跳表，可变层，读写锁）
                                             │            ├─ WAL（追加段文件+CRC+group commit）
                                             │            └─ SST L0/L1（只读，flush 产生）+ Manifest
```

- **写路径**：`Write(batch)` → 编码为 CF 前缀 Modify 序列 → WAL 追加（group commit 窗口内合并 fsync）→ MemTable → 超限则 flush 为 L0 SST（原子换 Manifest）→ 换新 WAL 段 → 清 MemTable；
- **读路径**：MemTable → L0→L1 逐层查找（新层优先，层内按 key），首次命中返回；
- **Scan**：MemTable + 各层 SST 的有序迭代器做**堆归并**，同 key 取新层版本；
- 并发：单写线程（WAL 顺序性）+ 多读；SST 不可变故免锁；MemTable 读写锁 + 冻结期写阻塞。

## 1.3 接口设计

### 1.3.1 对上（Storage 抽象，设计文档 §4.5 定型稿）

> **状态：草案**。本块在 M1 开工前须与用户核对后才算接口契约（CLAUDE.md §15）；
> 核对通过后删掉本注，并把定版内容同步到 `docs/cli.md` 的调用示例。

```cpp
namespace tinykv {
enum class Cf : uint8_t { Default, Write, Lock };
struct GetResult { std::optional<std::string> value; };

// 写操作：Put/Delete 是具体类型（不返回 pair/tuple，清单 #9）
struct Put    { Cf cf; std::string key; std::string value; };
struct Delete { Cf cf; std::string key; };
struct Modify { std::variant<Put, Delete> op; };
using ModifyVec = std::vector<Modify>;   // §1.6 示例所用的批量类型

class Iterator;   // 前向声明：RAII 迭代器，seek/valid/next 语义在 M1 定版

class Storage {
public:
    virtual ~Storage() = default;
    virtual result<GetResult> get(Cf cf, std::span<const std::byte> key) = 0;
    virtual result<void> write(std::span<const Modify> batch) = 0;       // 批量原子
    virtual result<std::unique_ptr<Reader>> create_reader() = 0;         // RAII + 一致性快照
};
struct Reader {   // RAII：析构即释放快照
    virtual result<GetResult> get(Cf cf, std::span<const std::byte> key) = 0;
    virtual result<std::unique_ptr<Iterator>> scan(Cf cf,
                std::span<const std::byte> start, std::size_t limit) = 0;
    virtual ~Reader() = default;
};
}
```

### 1.3.2 对下（引擎实现侧，M1 新定）

```cpp
namespace tinykv {
struct EngineOptions {
    std::filesystem::path dir;             // 数据目录（含 manifest / wal/ / sst/）
    bool fsync_on_commit = true;           // REL-02 关键开关（默认安全）
    size_t memtable_limit_bytes = 64 << 20;
    size_t wal_segment_bytes   = 128 << 20;
};
class Engine final : public Storage {
public:
    static result<std::unique_ptr<Engine>> open(EngineOptions opt);  // 唯一入口，内部完成恢复
    result<void> close();                                            // 显式关闭（flush+落 manifest）
};
class WalWriter {   // 追加 + group commit；段内序号连续，段切换原子
public:
    result<uint64_t> append(std::span<const std::byte> payload);  // 返回序号；fsync 策略见 options
    result<void> sync();                                          // 手动刷（group commit 内部调用）
};
}
```

### 1.3.3 错误矩阵（本阶段可能产生的 KvError 全集）

| 变体 | 触发场景 | 建议动作（hint 语义） |
|---|---|---|
| IoFailed | WAL/SST/Manifest 读写失败 | 检查磁盘空间/权限；重试打开 |
| Corrupted | WAL CRC 失败、SST footer 损坏 | 恢复停在该条；SST 层损坏需重建（M1 无副本） |
| InvalidArgument | 空 key、value>上限、limit=0、非法 CF | 按字段名修正调用 |

## 1.4 核心逻辑骨架

### 1.4.1 跳表 MemTable（查找/插入）

```
跳表: MAX_LEVEL=12, p=1/4; 节点 = {key(编码后), value_or_tombstone, next[] }
find(key): 从顶层逐层右移-下沉（标准 skiplist 查找）
insert(key,val): 随机层高 → 逐层插入 + 更新 span 记账(size 用于 flush 阈值)
Delete = 写墓碑(tombstone)，flush 时落 SST，读取层归并时消除
```

### 1.4.2 WAL 记录与 group commit

```
记录: [u32 len][u32 crc32c][payload(Modify 批量, 自定义紧凑编码: varint key_len + key + ...)]
group commit: 写线程攒批 → 单次 write() + (fsync_on_commit ? fdatasync/write+sync : 无) → 唤醒等待者
段切换: 当前段达 wal_segment_bytes → 写新段 → 更新 Manifest 段表 → 旧段保留（恢复仍需重放至最近 SST 点）
```

### 1.4.3 恢复（open 内部，REL-02 的核心路径）

```
1) 读 Manifest → 载入 SST 层表（L0/L1 文件列表 + 序号区间）
2) 逐段读 WAL: 校验 len+crc32c；**CRC 失败/尾部截断 → 停在该条为止（绝不 truncate 文件！）**
3) 按序重放 Modify 至 MemTable（批内原子：半批损坏整批丢弃，回到上一批末尾）
4) Engine 只读可用；首次写前不做任何清理动作
```

### 1.4.4 flush 与 Scan 归并

```
flush: 冻结 MemTable（新写等待）→ 顺序导出为 L0 SST(含 tombstone) → 原子替换 Manifest 段表
       → 新 WAL 段 → 解冻 + 释放旧段
Scan(Reader): iterators = [MemTable迭代器] + [L0 SST 迭代器...] + [L1...]
              小顶堆按 key 归并；同 key 多版本 → 取"最新层"版本并跳过同 key 其余条目；
              tombstone → 该 key 不输出（但不得干扰后续 key 的归并序）
```

## 1.5 红线

1. **启动/恢复路径绝无写与清空动作**；WAL 损坏只截断内存重放点，文件不动（Persister 反例，REL-02）；
2. `Write(batch)` 原子性：部分失败不得留下半批；已确认提交（fsync 返回）的数据任何掉电不丢；
3. Scan 语义：`create_reader()` 拿到的是**一致性快照**（迭代期间 flush 不影响结果集）；
4. CF 前缀编码全局唯一实现点在 `common`（SPEC-0 §0.3），引擎/事务不得自拼 key；
5. tombstone 必须落 SST 参与归并，禁止在读路径"跳过不写"（否则 Delete 会复活旧 SST 版本）；
6. 无 detach 线程、无裸 new/delete 队列（ENG-04，CI 步骤扫描）。

## 1.6 示例

嵌入示例（UX-07，进 CI 编译检查，≤20 行）：

```cpp
auto eng = tinykv::Engine::open({.dir = "demo.db"}).value();
auto wr  = tinykv::ModifyVec{ tinykv::Put{tinykv::Cf::Default, "k1", "v1"} };
eng->write(wr).value();
auto v  = eng->get(tinykv::Cf::Default, std::as_bytes(std::span{"k1", 2})).value();
assert(v.value == "v1");
auto it = eng->create_reader().value()->scan(tinykv::Cf::Default, {}, 10).value();
```

掉电恢复测试（`wal_crash_test` 的红→绿样板）：

```cpp
// 1) open → 随机写 1000 条（记录每条"已确认"时间线）→ 模拟 kill -9（进程级：fork+exit 或独立进程池）
// 2) 重新 open
// 断言: a) 最后一条 fsync 确认的记录及之前全部存在（fsync=on 时零丢失）
//       b) fsync=off 时丢失量 ≤ 窗口预期且剩余前缀完整（对比曲线，数据用于 UX-05 文档注释）
//       c) WAL 文件长度与损坏前一致（证明"绝不 truncate"）
```

Scan 一致性示例（`engine_scan_concurrent_test`）：

```cpp
auto rdr = eng->create_reader().value();
auto it  = rdr->scan(Cf::Default, start, limit).value();
// 后台并发 flush 两次 → 迭代到底 → 结果集与 flush 前快照一致（版本归并不受冻结/换层影响）
```

CLI 会话（UX-02 本地模式）：`tinykv-cli --config demo.yaml put k1 v1` → `get k1` → `scan a z --limit 10`。

### 配置项表（UX-05"配置即文档"，demo.yaml 全项）

| 配置项 | 默认值 | 影响说明（写入配置文件注释） |
|---|---|---|
| `engine.dir` | `./demo.db` | 数据目录；删除即重置（无副本，谨慎） |
| `engine.fsync_on_commit` | `true` | 关闭后掉电可能丢最近窗口数据（仅供基准对比） |
| `engine.memtable_limit_mb` | 64 | 超限触发 flush；越大写吞吐越高、恢复越慢 |
| `engine.wal_segment_mb` | 128 | 段大小；恢复重放粒度 |
| `log.level` / `log.dir` | `info` / `log/` | spdlog 轮转 10MB×5（CLAUDE.md §4-7） |

## 1.7 验收与证据链

| 需求 | 验收口径（可判定） | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| FUNC-01 | 四原语 + 3 CF 全通过；跨 CF 互不可见 | `engine_get_put_test` `engine_scan_test` `engine_cf_test` | `ctest -L fast` | junit.xml |
| FUNC-01(Scan) | 一致性快照语义成立 | `engine_scan_concurrent_test` | `ctest -L slow` | junit.xml |
| REL-02 | 1000 条随机写入+kill 重启：fsync=on 零丢失；WAL 文件不被改写 | `wal_crash_test`（种子复现） | `ctest -R wal_crash_test` | junit.xml + 种子日志 |
| UX-07 | 嵌入示例 ≤20 行可编译运行 | `readme_example_build`（CI 编译步骤） | CI | 编译产物 |
| UX-02(本地) | get/put/delete/scan 子命令 + --help + 示例 | `cli_local_smoke` | `ctest -L fast` | junit.xml |
| UX-05 | 新人只改配置即可启动（演示脚本一遍过）；配置项注释齐全（§1.6 表） | — | `scripts/demo_local.sh` | 演示日志 |
| UX-06 | M1 实现笔记一篇 + 架构图一张 | —（人工） | — | `docs/notes-m1.md` |
| PERF-01/02/05 | S0：写 ≥2k QPS；batch=64 ≥8×；P99 ≤10ms | `write_bench`（smoke 进 fast） | `ctest -R write_bench` / 夜跑 | `build/bench/write.json` |
| CLAUDE.md §14 POC-A | 10 万 key 读写正确 + kill 重启完整 + fsync 开/关对比 | （由上行测试组合覆盖） | 同上 | 同上 |

**M1 完成判定** = 上表证据物齐备且口径达标（fsync=on 口径为准，off 仅作对比曲线）+ `notes-m1.md` 就位。

## 1.8 开放决策点

| # | 决策 | 建议默认 |
|---|---|---|
| D1-1 | SST 层数 | L0+L1 两层（够 Perf 基线，compaction 后置） |
| D1-2 | memtable 上限 | 64MB（可配） |
| D1-3 | WAL 编码 | 自定义紧凑（非 protobuf），payload 含批量 Modify |
| D1-4 | WiscKey value 分离 | 后置（stretch，非 M1） |
| D1-5 | 跳表参数 | MAX_LEVEL=12, p=1/4 |
