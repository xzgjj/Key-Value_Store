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
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "common/ids.h"

namespace tinykv {

// region 世代：version 随 split/merge 递增、conf_ver 随成员变更递增
// （完整语义与推进规则见 SPEC-3，M0 仅作 StaleEpoch 的载体字段）
struct RegionEpoch {
    uint64_t version{};
    uint64_t conf_ver{};

    friend constexpr bool operator==(RegionEpoch, RegionEpoch) = default;
    friend constexpr auto operator<=>(RegionEpoch, RegionEpoch) = default;
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

using KvError =
    std::variant<KeyNotInRegion, StaleEpoch, NotLeader, IoFailed, Corrupted, InvalidArgument>;

// 全项目统一结果别名：API 边界一律 [[nodiscard]]（清单 #6；错误分层见 CLAUDE.md §7-1：
// 不变量破坏/环境致命错误走异常，不进此通道）
template <typename T>
using result = std::expected<T, KvError>;

}  // namespace tinykv
