// 文件用途：Region 元数据 + StoreMeta 区间索引 + Router 路由与 epoch 校验（M4 第②层，FUNC-07/REL-05）。
// 归属：TinyKV-CPP（M4 Multi-Raft 控制面）。
//
// REL-05 不变量：任意时刻每个 key 恰好属于一个 Region。实现为"按 end_key 排序的半开区间
// [start,end) 首尾相接、无重叠、无空洞"，find_region 用 upper_bound(end_key) 取第一个覆盖者。
// 区间索引用 std::map（有序查找 upper_bound）——C++23 无标准 btree，按"优先语言机制"不引新库。
// 世代比较复用 common/error.h 的 RegionEpoch::is_comparable_with / is_newer_than：
// 混序（一维大一维小）与 region 更新都按 StaleEpoch 拒绝，hint 带 have/want 供客户端定向刷新。

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error.h"
#include "common/ids.h"

namespace tinykv::raftstore {

struct PeerInfo {
    PeerId id;
    std::uint64_t store_id{0};
    bool is_learner{false};
};

// region 元信息：[start_key, end_key) 半开区间，空 end_key = +∞
struct Region {
    RegionId id;
    std::string start_key;
    std::string end_key;
    std::vector<PeerInfo> peers;
    RegionEpoch epoch;
    std::int64_t size_hint{0};   // 近似字节数（Put/Delete apply 时增减，供 split-check 估计）

    // key 是否落在本 region：start <= key < end（空 end = 无限）
    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        const bool ge_start = start_key.empty() || key >= std::string_view(start_key);
        const bool lt_end = end_key.empty() || key < std::string_view(end_key);
        return ge_start && lt_end;
    }
};

// Split 校验与切分（SPEC-3 红线1）：split_key 必须落在 (start,end) 开区间。
// 成功返回左右两个新 region：左继承原 id、右用 new_id；两者 version 各 +1（数据世代推进）。
[[nodiscard]] inline result<std::pair<Region, Region>> split_region(
    const Region& r, std::string_view split_key, RegionId new_id) {
    const bool gt_start = r.start_key.empty() || split_key > std::string_view(r.start_key);
    const bool lt_end = r.end_key.empty() || split_key < std::string_view(r.end_key);
    if (!gt_start || !lt_end || split_key.empty()) {
        return std::unexpected(InvalidArgument{"split_key 必须落在 region 开区间内"});
    }
    Region left = r;
    left.end_key = std::string(split_key);
    left.epoch.version += 1;
    Region right = r;
    right.id = new_id;
    right.start_key = std::string(split_key);
    right.epoch.version += 1;
    // size_hint 按 key 均分（近似）：无精确分布时按区间对半是保守估计
    left.size_hint = r.size_hint / 2;
    right.size_hint = r.size_hint - left.size_hint;
    return std::make_pair(std::move(left), std::move(right));
}

// 每个 store 的 region 元数据视图：regions map + 按 end_key 的区间索引，RWMutex 保护。
// split/conf-change apply 时更新，路由读与更新并发安全。
class StoreMeta {
public:
    StoreMeta() = default;
    StoreMeta(const StoreMeta&) = delete;
    StoreMeta& operator=(const StoreMeta&) = delete;

    // 新增 region；与既有区间重叠则拒绝（保护 REL-05 不变量）
    [[nodiscard]] result<void> add_region(Region region) {
        std::unique_lock lock(mu_);
        if (overlaps_locked(region)) {
            return std::unexpected(InvalidArgument{"region 区间与既有 region 重叠"});
        }
        insert_locked(std::move(region));
        return {};
    }

    // split 原子替换：删旧 region、加入左右两个新 region；三者须在同一把锁内完成。
    // 校验：旧 id 存在；左右两区间首尾相接且覆盖旧区间；不与其他 region 重叠。
    [[nodiscard]] result<void> replace_region(RegionId old_id, Region left, Region right) {
        std::unique_lock lock(mu_);
        auto it = regions_.find(old_id);
        if (it == regions_.end()) {
            return std::unexpected(InvalidArgument{"待 split 的 region 不存在"});
        }
        const std::string old_start = it->second.start_key;
        const std::string old_end = it->second.end_key;
        if (left.start_key != old_start || right.end_key != old_end ||
            left.end_key != right.start_key) {
            return std::unexpected(InvalidArgument{"split 左右区间未覆盖原区间"});
        }
        regions_.erase(it);
        ranges_.erase(old_end);
        insert_locked(std::move(left));
        insert_locked(std::move(right));
        return {};
    }

    // 按 key 找覆盖它的 region（区间索引 upper_bound）。
    // 注意：+∞ 区间的 end_key 为空串，字典序里最小，不能进 upper_bound 索引；
    // 在非空 end 索引里找第一个 end > key 的区间，找不到则落到唯一的 +∞ 区间。
    [[nodiscard]] std::optional<Region> find_region(std::string_view key) const {
        std::shared_lock lock(mu_);
        auto it = ranges_.find("");   // +∞ 区间（end 为空）
        const bool has_infinite = (it != ranges_.end());
        auto lit = ranges_.upper_bound(std::string(key));   // 第一个 end > key
        if (lit == ranges_.end() || lit->first.empty()) {
            // 没有更近的有界区间 → 若存在 +∞ 区间则命中它
            if (has_infinite) {
                return regions_.at(it->second);
            }
            return std::nullopt;
        }
        return regions_.at(lit->second);
    }

    [[nodiscard]] std::optional<Region> find_region_by_id(RegionId id) const {
        std::shared_lock lock(mu_);
        auto it = regions_.find(id);
        if (it == regions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::size_t region_count() const {
        std::shared_lock lock(mu_);
        return regions_.size();
    }

    // M4 ⑦b：全量 region 快照（观测端点 GET /regions 的数据源；值拷贝，与路由读并发安全）
    [[nodiscard]] std::vector<Region> all_regions() const {
        std::shared_lock lock(mu_);
        std::vector<Region> out;
        out.reserve(regions_.size());
        for (const auto& [id, r] : regions_) {
            out.push_back(r);
        }
        return out;
    }

private:
    // 调用方持唯一锁：检查新 region 是否与既有区间重叠。
    // 半开区间 [s1,e1) 与 [s2,e2) 重叠 ⟺ s1<e2 && s2<e1（空 end = +∞，视为最大）。
    // region 数量课程级（几十~几百），正确优先用全扫描；索引只服务高频的 find_region。
    [[nodiscard]] bool overlaps_locked(const Region& r) const {
        for (const auto& [id, existing] : regions_) {
            (void)id;
            const bool starts_before_other_end =
                existing.end_key.empty() || r.start_key < existing.end_key;
            const bool other_starts_before_end =
                r.end_key.empty() || existing.start_key < r.end_key;
            if (starts_before_other_end && other_starts_before_end) {
                return true;
            }
        }
        return false;
    }

    void insert_locked(Region r) {
        const RegionId id = r.id;
        const std::string end = r.end_key;
        ranges_[end] = id;   // 空 end（+∞）作为最大 key，upper_bound 永远排在最后
        regions_.emplace(id, std::move(r));
    }

    mutable std::shared_mutex mu_;
    std::map<RegionId, Region> regions_;
    std::map<std::string, RegionId> ranges_;   // end_key → RegionId（有序）
};

// 路由入口：按 key 找 region 并校验客户端携带的 epoch。
class Router {
public:
    explicit Router(const StoreMeta& meta) : meta_(meta) {}

    // 成功返回 region；失败返回 StaleEpoch{have,want,hint} 或 KeyNotInRegion
    [[nodiscard]] result<Region> route(std::string_view key, const RegionEpoch& client_epoch) const {
        auto region = meta_.find_region(key);
        if (!region) {
            return std::unexpected(KeyNotInRegion{.key = std::string(key), .region_id = RegionId{0}});
        }
        if (!client_epoch.is_comparable_with(region->epoch) ||
            region->epoch.is_newer_than(client_epoch)) {
            return std::unexpected(StaleEpoch{.region_id = region->id,
                                              .have = client_epoch,
                                              .want = region->epoch});
        }
        return *region;
    }

private:
    const StoreMeta& meta_;
};

}  // namespace tinykv::raftstore
