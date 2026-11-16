// 文件用途：强类型 ID 库——八类 ID 禁隐式互转与算术，防线放编译期（SPEC-0 §0.3.1，清单 #2/#10）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。
//
// 设计要点：
//   * 构造显式：uint64 必须手写一次类型名（如 Term{5}），强迫调用方自问"这是什么 ID"；
//   * 裸值取用显式：不提供 operator uint64_t()（红线 2），需要裸值必须写 .value，事故源可 grep；
//   * 比较/哈希可用、算术不可用：ID 是身份不是数量；
//   * 零开销：全部 constexpr，无 vtable、无额外存储。

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace tinykv {

// 强类型 ID 基座：同 Tag 可比较可排序，跨 Tag 无任何操作（由模板实例化天然隔离）
template <typename Tag>
struct StrongId {
    uint64_t value{};

    constexpr explicit StrongId(uint64_t v) noexcept : value(v) {}

    friend constexpr bool operator==(StrongId, StrongId) = default;
    friend constexpr auto operator<=>(StrongId, StrongId) = default;
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

// 无序容器哈希：按 .value 取 uint64 哈希（比较/哈希可用，SPEC-0 §0.3.1）
template <typename Tag>
struct HashFor {
    size_t operator()(StrongId<Tag> id) const noexcept { return std::hash<uint64_t>{}(id.value); }
};

}  // namespace tinykv
