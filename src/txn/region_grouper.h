// 文件用途：⑦ 跨区事务的分组内核（RegionGrouper）——把一批 mutations 按 region 分桶。
// 归属：TinyKV-CPP（M7 演进 ⑦ 的第一步，SPEC-6 §6.4）。
//
// 为什么先只做分组（而不是直接上协调器）：
//   SPEC-6 §6.1.2 把 ⑦ 拆成"护栏 → 分组 → 状态机 → 恢复"四步，并明确写了
//   "不允许一次性提交完整 2PC"——这类改动一旦不收敛，无法定位是分组错还是阶段错。
//   本文件是第 1 步：一个**纯函数**，只依赖路由查询，不碰存储、不发提案，
//   因此可以用单测把"分组正确性"与"协议正确性"彻底分开验证。
//
// 不变量：
//   * 分组是划分（每个 mutation 恰属一组，不重不漏）；
//   * 组的顺序按 RegionId 升序（同输入同输出，便于日志比对与重放）；
//   * 只有一组时 caller 应走原单区路径（零额外开销，SPEC-6 §6.4）。

#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "common/compat.h"
#include "common/error.h"
#include "common/ids.h"
#include "txn/mvcc.h"

namespace tinykv::txn {

struct RegionGroup {
    RegionId region_id;
    std::vector<Mutation> mutations;
};

// 按 region 分组；primary 所在组用 primary 标记（该组是 2PC 的锚点）
struct RegionGroups {
    std::vector<RegionGroup> groups;
    RegionId primary_region;
    // 主组在 groups 里的下标（primary 一定属于某组：prewrite 要求 primary ∈ mutations）
    std::size_t primary_index{0};

    [[nodiscard]] bool single_region() const { return groups.size() == 1; }
    [[nodiscard]] std::size_t region_count() const { return groups.size(); }
};

// 路由查询：key → region。空 = 该 key 不落任何 region（路由过期）。
using RegionOfFn = std::function<std::optional<RegionId>(std::string_view key)>;

// 分组入口。任一 key 找不到 region → KeyNotInRegion（不做"猜一个 region"的兜底）。
[[nodiscard]] inline result<RegionGroups> group_by_region(span<const Mutation> mutations,
                                                          std::string_view primary,
                                                          const RegionOfFn& region_of) {
    if (mutations.empty()) {
        return tinykv::unexpected(InvalidArgument{"mutations 不得为空"});
    }
    std::map<RegionId, std::vector<Mutation>> by_region;  // map：RegionId 升序，输出确定
    for (const auto& m : mutations) {
        const auto region = region_of(m.key);
        if (!region.has_value()) {
            return tinykv::unexpected(KeyNotInRegion{.key = m.key, .region_id = RegionId{0}});
        }
        by_region[*region].push_back(m);
    }
    const auto primary_region = region_of(primary);
    if (!primary_region.has_value()) {
        return tinykv::unexpected(
            KeyNotInRegion{.key = std::string(primary), .region_id = RegionId{0}});
    }
    if (by_region.find(*primary_region) == by_region.end()) {
        // primary 不在 mutations 里：prewrite 的决议锚点不成立（SPEC-4 红线 2）
        return tinykv::unexpected(InvalidArgument{"primary 必须在 mutations 中（决议锚点）"});
    }
    RegionGroups out;
    out.primary_region = *primary_region;
    out.groups.reserve(by_region.size());
    for (auto& [region_id, muts] : by_region) {
        if (region_id == *primary_region) {
            out.primary_index = out.groups.size();
        }
        out.groups.push_back(RegionGroup{.region_id = region_id, .mutations = std::move(muts)});
    }
    return out;
}

}  // namespace tinykv::txn
