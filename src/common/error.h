// 文件用途：结构化错误 KvError（六变体）与 result<T> 别名——全接口统一错误通道
//          （SPEC-0 §0.3.1 触发条件矩阵；UX-03 三要素 what/why/hint 全在字段里）。
// 归属：TinyKV-CPP（M0 基建，ENG-01/UX-03）。
//
// 变体 ⇄ 三要素对应（详见 SPEC-0 §0.3.1 矩阵，新增字段 = 接口变更须过 13 条清单评审）：
//   KeyNotInRegion  key 不落任何 region range   → why: 路由过期/未 split → hint: 请求新路由
//   StaleEpoch      请求 epoch 落后当前 epoch    → why: 路由缓存过期   → hint: 携带 have/want 刷新
//   NotLeader       请求打到非 leader peer      → why: leader 已变    → hint: leader_hint 重定向
//   IoFailed        文件读写/flush 失败         → why: OS 错误原因    → hint: 检查磁盘/权限
//   Corrupted       CRC 校验失败               → why: 数据损坏       → hint: 恢复/快照追赶
//   InvalidArgument 参数越界（空 key、limit<0）  → why: 调用方错误     → hint: 按字段名修正

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "common/compat.h"
#include "common/ids.h"

namespace tinykv {

// region 世代：version 随 split/merge 递增、conf_ver 随成员变更递增
// （完整语义与推进规则见 SPEC-3，M0 仅作 StaleEpoch 的载体字段）
//
// 为什么只有 == 而没有 <=>（2026-11-16 改）：两维计数器的正确关系不是全序——
// 若 have={v2,cv1}、want={v1,cv5}，词典序会判 have > want（"不落后"），
// 而 TiKV 语义下这是**混序**，请求必须按 stale 拒绝。默认 operator<=> 等于
// 免费提供一个会判错的比较入口，所以删掉，只留两个命名正确的判定函数。
struct RegionEpoch {
    uint64_t version{};
    uint64_t conf_ver{};

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
    friend constexpr bool operator==(RegionEpoch, RegionEpoch) = default;
#else
    // C++17 兜底：默认比较运算符是 C++20 特性，这里逐字段显式写
    friend constexpr bool operator==(const RegionEpoch& a, const RegionEpoch& b) noexcept {
        return a.version == b.version && a.conf_ver == b.conf_ver;
    }
    friend constexpr bool operator!=(const RegionEpoch& a, const RegionEpoch& b) noexcept {
        return !(a == b);
    }
#endif

    // 严格更新：两维均不小于对方，且至少一维更大（epoch 只单调推进，不允许回退）
    [[nodiscard]] constexpr bool is_newer_than(const RegionEpoch& other) const noexcept {
        const bool version_ge = version >= other.version;
        const bool conf_ge = conf_ver >= other.conf_ver;
        return version_ge && conf_ge && (version != other.version || conf_ver != other.conf_ver);
    }

    // 是否可比：两维不交叉。混序（一维大、一维小）在 TiKV 语义下不存在先后，
    // 调用方必须按 StaleEpoch 拒绝并要求客户端刷新，而不是猜一个方向。
    [[nodiscard]] constexpr bool is_comparable_with(const RegionEpoch& other) const noexcept {
        return (version >= other.version && conf_ver >= other.conf_ver) ||
               (version <= other.version && conf_ver <= other.conf_ver);
    }
};

// 六个错误变体：字段名即语义（what / why / hint 三要素见文件头矩阵）
struct KeyNotInRegion {
    std::string key;
    RegionId region_id;
};

struct StaleEpoch {
    RegionId region_id;
    RegionEpoch have;  // 请求方持有的世代
    RegionEpoch want;  // region 当前世代
};

struct NotLeader {
    RegionId region_id;
    std::optional<PeerId> leader_hint;  // 未知时必须为 nullopt，禁止伪造
};

struct IoFailed {
    std::string path;
    std::string reason;
};

struct Corrupted {
    std::string where;   // 位置（如 wal:seg-0001:offset-4096）
    std::string reason;  // 损坏原因（如 crc mismatch）
};

struct InvalidArgument {
    std::string what;  // 哪个参数、为何非法
};

// ---- M5 事务错误（SPEC-4 §4.3.3 错误矩阵；用户 2026-11-16 授权按 SPEC 落版） ----

struct WriteConflict {
    std::string key;                     // 冲突 key
    std::uint64_t start_ts{};            // 本事务 start_ts
    std::uint64_t conflict_commit_ts{};  // 对方 Write 的 commitTs（> start_ts 即冲突）
    // hint：立即 abort 自身（无死锁结构保证）；换 ts 重试或报用户
};

struct LockConflict {
    std::string key;                // 被锁住的 key
    std::string primary;            // 锁的 primary 指针（决议锚点）
    std::uint64_t lock_start_ts{};  // 持锁事务的 start_ts
    // hint：等待→TTL 过后 CheckTxnStatus 决议
};

struct Retryable {
    std::string what;  // 决议后锁已清，可安全重试的场合
};

// TSO 严格单调是不变量（红线6）；破坏走 fatal 异常而非 result 错误通道
// （异常分层见 CLAUDE.md §7-1：不变量破坏不进 result，直接拒绝服务并报告）
class TsNotMonotonic final : public std::runtime_error {
public:
    explicit TsNotMonotonic(std::uint64_t got, std::uint64_t last)
        : std::runtime_error("TSO 非严格单调: got=" + std::to_string(got) +
                             " last=" + std::to_string(last)),
          got_(got),
          last_(last) {}
    [[nodiscard]] std::uint64_t got() const noexcept { return got_; }
    [[nodiscard]] std::uint64_t last() const noexcept { return last_; }

private:
    std::uint64_t got_;
    std::uint64_t last_;
};

using KvError = std::variant<KeyNotInRegion, StaleEpoch, NotLeader, IoFailed, Corrupted,
                             InvalidArgument, WriteConflict, LockConflict, Retryable>;

// 全项目统一结果别名：API 边界一律 [[nodiscard]]（清单 #6；错误分层见 CLAUDE.md §7-1：
// 不变量破坏/环境致命错误走异常，不进此通道）
template <typename T>
using result = tinykv::expected<T, KvError>;

}  // namespace tinykv
