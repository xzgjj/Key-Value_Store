// 文件用途：KvError 的人可读渲染——UX-03"三要素"的唯一实现点（发生了什么/为什么/建议动作）。
// 归属：TinyKV-CPP（M1 单机存储；补 ENG-01/UX-03 的落地缺口）。
//
// 为什么要集中在这里：结构体字段已经承载了三要素，但每个消费方（CLI、日志、将来的 RPC 回执）
// 若各写一份 std::visit，错误措辞会立刻分叉，且分叉发生在最难统一的地方——用户看到的文案。
// 集中一处后，措辞调整只改这个文件，测试也只断言这一处。

#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "common/error.h"

namespace tinykv {

// 变体名：给日志/CLI 做短标识（稳定、可 grep，不随文案变动）
[[nodiscard]] inline std::string_view describe_kind(const KvError& error) {
    return std::visit(
        [](const auto& v) -> std::string_view {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, KeyNotInRegion>) {
                return "KeyNotInRegion";
            } else if constexpr (std::same_as<T, StaleEpoch>) {
                return "StaleEpoch";
            } else if constexpr (std::same_as<T, NotLeader>) {
                return "NotLeader";
            } else if constexpr (std::same_as<T, IoFailed>) {
                return "IoFailed";
            } else if constexpr (std::same_as<T, Corrupted>) {
                return "Corrupted";
            } else if constexpr (std::same_as<T, WriteConflict>) {
                return "WriteConflict";
            } else if constexpr (std::same_as<T, LockConflict>) {
                return "LockConflict";
            } else if constexpr (std::same_as<T, Retryable>) {
                return "Retryable";
            } else {
                return "InvalidArgument";
            }
        },
        error);
}

// 三要素全文：<变体>: <发生了什么>；原因=<为什么>；建议=<怎么办>
[[nodiscard]] inline std::string describe(const KvError& error) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, KeyNotInRegion>) {
                return "KeyNotInRegion: key 不在该 region 范围内 region_id="
                       + std::to_string(v.region_id.value) + "；原因=路由过期或刚发生 split；"
                       "建议=回源调度器刷新路由后重试";
            } else if constexpr (std::same_as<T, StaleEpoch>) {
                return "StaleEpoch: 请求 epoch 落后 region_id=" + std::to_string(v.region_id.value)
                       + "；原因=路由缓存过期；建议=按 have/want 刷新路由后重试";
            } else if constexpr (std::same_as<T, NotLeader>) {
                return std::string("NotLeader: region_id=") + std::to_string(v.region_id.value)
                       + "；原因=leader 已变更；建议="
                       + (v.leader_hint ? "按 leader_hint 重定向" : "leader 未知，回源查询后重试");
            } else if constexpr (std::same_as<T, IoFailed>) {
                return "IoFailed: path=" + v.path + "；原因=" + v.reason
                       + "；建议=检查磁盘空间与目录权限";
            } else if constexpr (std::same_as<T, Corrupted>) {
                return "Corrupted: 位置=" + v.where + "；原因=" + v.reason
                       + "；建议=从副本重拉或按快照恢复";
            } else if constexpr (std::same_as<T, WriteConflict>) {
                return "WriteConflict: key=" + v.key + " start_ts=" + std::to_string(v.start_ts)
                       + " conflict_commit_ts=" + std::to_string(v.conflict_commit_ts)
                       + "；原因=他事务已提交新版本（SI 写写冲突）；建议=换 start_ts 重试或报用户";
            } else if constexpr (std::same_as<T, LockConflict>) {
                return "LockConflict: key=" + v.key + " primary=" + v.primary
                       + " lock_start_ts=" + std::to_string(v.lock_start_ts)
                       + "；原因=他事务持锁；建议=等待 TTL 过期后 CheckTxnStatus 决议";
            } else if constexpr (std::same_as<T, Retryable>) {
                return "Retryable: " + v.what + "；原因=锁已决议清除；建议=安全重试";
            } else {
                return "InvalidArgument: " + v.what + "；原因=调用方参数非法；建议=按字段修正后重试";
            }
        },
        error);
}

}  // namespace tinykv
