// 文件用途：强类型 ID 库——八类 ID 禁隐式互转与算术，防线放编译期（SPEC-0 §0.3.1，清单 #2/#10）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。
//
// 设计要点：
//   * 构造显式：uint64 必须手写一次类型名（如 Term{5}），强迫调用方自问"这是什么 ID"；
//   * 裸值取用显式：不提供 operator uint64_t()（红线 2），需要裸值必须写 .value，事故源可 grep；
//   * 比较/哈希可用、算术不可用：ID 是身份不是数量；"加一"这类推进必须走 next()/prev()，
//     语义落在函数名上而不是运算符上；
//   * 零开销：全部 constexpr，无 vtable、无额外存储（sizeof == 8 由测试锁定）。
//
// 默认构造的语义（2026-11-16 补）：default-construct 得到 value == 0，即 invalid()——
// 表示"未设置"，与 proto 里"0 表示可空"的约定一致。补默认构造不是放松约束，而是让
// std::vector<LogIndex>(n)、std::array<Term, k> 这类值初始化可用（此前无默认构造，
// 不满足 DefaultInsertable，容器层面直接编译失败）。有效值从 1 起，由 is_valid() 显式判断。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace tinykv {

// 强类型 ID 基座：同 Tag 可比较可排序，跨 Tag 无任何操作（由模板实例化天然隔离）
template <typename Tag>
struct StrongId {
    uint64_t value{};

    constexpr StrongId() noexcept = default;  // 值初始化 → 0（= invalid）

    constexpr explicit StrongId(uint64_t v) noexcept : value(v) {}

    // 显式的"未设置"：命名比裸写 LogIndex{0} 更难被误读（语义落在函数名上）
    [[nodiscard]] static constexpr StrongId invalid() noexcept { return StrongId{}; }

    // 有效值从 1 起（Raft log index / term 0 均非法，见 proto/raft.proto 字段注释）
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

    // 单调推进：Raft 里 index±1 极其频繁，用命名函数替代被禁用的 operator+
    [[nodiscard]] constexpr StrongId next() const noexcept { return StrongId{value + 1}; }
    [[nodiscard]] constexpr StrongId prev() const noexcept { return StrongId{value - 1}; }

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
    friend constexpr bool operator==(StrongId, StrongId) = default;
#else
    friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value == b.value; }
#endif
#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
    friend constexpr auto operator<=>(StrongId, StrongId) = default;
#else
    // C++17 兜底：显式六个比较（不写 <=> 的等价展开，避免与默认实现产生语义分歧）
    friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return !(a == b); }
    friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value < b.value; }
    friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return b < a; }
    friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return !(a < b); }
#endif
};

// 八类 ID 的 Tag：空类型仅用于实例化出互异类型
struct TermTag;      // Raft 任期
struct LogIndexTag;  // Raft 日志索引
struct RegionIdTag;  // region 标识（M4 起多 region）
struct PeerIdTag;    // region 内副本标识
struct StoreIdTag;   // 物理存储节点标识
struct ClientIdTag;  // 客户端会话标识（M2 去重表）
struct SeqNoTag;     // 客户端请求序号（M2 去重表）
struct TsTag;        // 事务时间戳（M5 Percolator）

using Term = StrongId<TermTag>;
using LogIndex = StrongId<LogIndexTag>;
using RegionId = StrongId<RegionIdTag>;
using PeerId = StrongId<PeerIdTag>;
using StoreId = StrongId<StoreIdTag>;
using ClientId = StrongId<ClientIdTag>;
using SeqNo = StrongId<SeqNoTag>;
using Ts = StrongId<TsTag>;

// 显式哈希器（保留以兼容既有调用）：需手写 Tag，属实现细节外泄；
// 新代码请直接用 std::hash<StrongId<Tag>>（见文件末尾偏特化）。
template <typename Tag>
struct HashFor {
    size_t operator()(StrongId<Tag> id) const noexcept { return std::hash<uint64_t>{}(id.value); }
};

}  // namespace tinykv

// 无序容器开箱可用：unordered_map<RegionId, ...> 不再要求调用方知道 RegionId == StrongId<Tag>。
// 偏特化写在 std 内且在类型定义之后；operator() 为 constexpr，编译期容器也可用。
namespace std {

template <typename Tag>
struct hash<tinykv::StrongId<Tag>> {
    // splitmix64 收尾混合：直接拿 uint64 低位做桶号，在 region/peer 连号时会大量碰撞
    constexpr size_t operator()(tinykv::StrongId<Tag> id) const noexcept {
        uint64_t x = id.value + 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<size_t>(x);
    }
};

}  // namespace std
