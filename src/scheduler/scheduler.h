// 文件用途：PD-lite 调度器（进程内 C++ 对象，M4 第⑥层，FUNC-12）。只动元数据，数据面走独立通道
//          （Mooncake 控制面/数据面分离，复用台账 M-1）。不生成 service stub、不开新 RPC（用户 A1）。
// 归属：TinyKV-CPP（M4 Multi-Raft 控制面）。
//
// 职责：收集 store/region 心跳 → 按 size 差产生 MovePeer 建议（限速）→ AskSplit 预分配 region/peer id。
// 均衡只在"建议层"：真正迁移由 store 侧按建议走 AddLearner→AddVoter→RemoveNode（复用④层机制），
// 调度器不直接改数据，也不阻塞数据通道。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <scheduler.pb.h>

#include "common/error.h"   // RegionEpoch（scheduler.proto 复用 common.proto 的 epoch）

namespace tinykv::scheduler {

// store 心跳视图
struct StoreStat {
    std::uint64_t store_id{0};
    std::uint32_t region_count{0};
    std::uint64_t total_size{0};
    std::uint64_t capacity{0};
};

// region 心跳视图（只存均衡所需字段）
struct RegionStat {
    std::uint64_t region_id{0};
    std::uint64_t approximate_size{0};
    std::vector<std::uint64_t> store_ids;   // 各副本所在 store
    tinykv::RegionEpoch epoch;
};

// 迁移建议（调度器只给建议，执行在 store 侧）
struct MovePeer {
    std::uint64_t region_id{0};
    std::uint64_t from_store{0};
    std::uint64_t to_store{0};
};

// 均衡参数（SPEC-3 D3-2：size 差 >20% 触发 MovePeer）
struct BalanceConfig {
    double trigger_diff{0.2};
    std::size_t max_moves_per_round{1};   // 限速：每轮最多迁移数（1 迁移/分钟的量级）
};

class Scheduler {
public:
    // 收集 store 心跳：覆盖更新（同一 store 重复上报取最新）
    void on_store_heartbeat(const schedulerpb::StoreHeartbeatReq& req) {
        StoreStat s;
        s.store_id = req.store_id();
        s.region_count = req.region_count();
        s.total_size = req.total_size();
        s.capacity = req.capacity();
        stores_[s.store_id] = s;
    }

    // 收集 region 心跳：更新大小与副本分布，并把分配基线抬到已见最大 id
    // （否则 AskSplit 会分配出与既有 region/peer 撞号的 id——review 第 20 轮发现并修复）
    void on_region_heartbeat(const schedulerpb::RegionHeartbeatReq& req) {
        RegionStat r;
        r.region_id = req.region_id();
        r.approximate_size = req.approximate_size();
        for (const auto& p : req.peers()) {
            r.store_ids.push_back(p.store_id());
            if (p.id() > max_peer_id_) {
                max_peer_id_ = p.id();
            }
        }
        r.epoch = tinykv::RegionEpoch{.version = req.epoch().version(),
                                      .conf_ver = req.epoch().conf_ver()};
        regions_[r.region_id] = r;
        if (r.region_id > next_region_id_) {
            next_region_id_ = r.region_id;
        }
    }

    // AskSplit：为新 region 预分配 id + 等长 peer_ids，并登记新 region（大小按对半估）。
    // 分配从"已见最大 id"之后起算，保证全局唯一（见 on_region_heartbeat 的基线抬升）。
    [[nodiscard]] schedulerpb::AskSplitResp ask_split(const schedulerpb::AskSplitReq& req,
                                                      std::uint32_t peer_count) {
        schedulerpb::AskSplitResp resp;
        const auto it = regions_.find(req.region_id());
        if (req.region_id() > next_region_id_) {
            next_region_id_ = req.region_id();   // 父 region 未登记过时也不撞号
        }
        const std::uint64_t new_region = ++next_region_id_;
        resp.set_new_region_id(new_region);
        std::vector<std::uint64_t> stores;
        if (it != regions_.end()) {
            stores = it->second.store_ids;
        }
        if (max_peer_id_ > next_peer_id_) {
            next_peer_id_ = max_peer_id_;   // 首次分配前先对齐已见最大 peer id
        }
        for (std::uint32_t i = 0; i < peer_count; ++i) {
            resp.add_new_peer_ids(++next_peer_id_);
        }
        // 登记新 region 元数据：沿用父 region 的副本分布（迁移由均衡另行决定）
        RegionStat r;
        r.region_id = new_region;
        r.store_ids = stores;
        if (it != regions_.end()) {
            r.approximate_size = it->second.approximate_size / 2;
        }
        regions_[new_region] = std::move(r);
        ++split_count_;
        return resp;
    }

    // 均衡：找 size 最大的 store 与最小的 store，差超阈值则建议迁一个 region。
    // 返回至多 max_moves_per_round 条建议（限速）；无建议时返回空。
    [[nodiscard]] std::vector<MovePeer> balance(const BalanceConfig& cfg = {}) const {
        std::vector<MovePeer> out;
        if (stores_.size() < 2) {
            return out;
        }
        const auto [lo, hi] = min_max_store_by_size();
        if (lo == nullptr || hi == nullptr || hi->total_size <= lo->total_size) {
            return out;
        }
        const double avg = average_size();
        if (avg <= 0.0 ||
            static_cast<double>(hi->total_size - lo->total_size) / avg <= cfg.trigger_diff) {
            return out;   // 差异未超阈值
        }
        // 从大 store 上挑一个 region 迁到小 store（跳过已在小 store 有副本的 region）
        for (const auto& [id, r] : regions_) {
            const bool on_hi = contains_store(r.store_ids, hi->store_id);
            const bool on_lo = contains_store(r.store_ids, lo->store_id);
            if (on_hi && !on_lo) {
                out.push_back(MovePeer{.region_id = id, .from_store = hi->store_id,
                                       .to_store = lo->store_id});
                if (out.size() >= cfg.max_moves_per_round) {
                    break;
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t store_count() const { return stores_.size(); }
    [[nodiscard]] std::size_t region_count() const { return regions_.size(); }
    [[nodiscard]] std::uint64_t split_count() const { return split_count_; }
    [[nodiscard]] std::uint64_t next_region_id() const { return next_region_id_; }

    [[nodiscard]] const std::map<std::uint64_t, StoreStat>& stores() const { return stores_; }
    [[nodiscard]] const std::map<std::uint64_t, RegionStat>& regions() const { return regions_; }

private:
    [[nodiscard]] static bool contains_store(const std::vector<std::uint64_t>& v, std::uint64_t id) {
        for (const auto s : v) {
            if (s == id) {
                return true;
            }
        }
        return false;
    }

    // size 汇总只信 store 心跳（SPEC-3 D3-2：按 store 上报 size 差触发）。
    // 曾用"region 近似大小回填 store 汇总"做兜底，已删除：proto3 下 total_size=0 与"未上报"
    // 不可区分，回填会把新建空 store 算成有量，抹掉均衡目标并让 size 差凭空消失。
    [[nodiscard]] double average_size() const {
        if (stores_.empty()) {
            return 0.0;
        }
        std::uint64_t sum = 0;
        for (const auto& [id, s] : stores_) {
            (void)id;
            sum += s.total_size;
        }
        return static_cast<double>(sum) / static_cast<double>(stores_.size());
    }

    // 返回最小/最大 size 的 store 指针（无 store 时为空）
    [[nodiscard]] std::pair<const StoreStat*, const StoreStat*> min_max_store_by_size() const {
        const StoreStat* lo = nullptr;
        const StoreStat* hi = nullptr;
        for (const auto& [id, s] : stores_) {
            (void)id;
            if (lo == nullptr || s.total_size < lo->total_size) {
                lo = &s;
            }
            if (hi == nullptr || s.total_size > hi->total_size) {
                hi = &s;
            }
        }
        return {lo, hi};
    }

    std::map<std::uint64_t, StoreStat> stores_;
    std::map<std::uint64_t, RegionStat> regions_;
    std::uint64_t next_region_id_{0};      // 已分配/已见的最大 region id
    std::uint64_t next_peer_id_{0};        // 已分配/已见的最大 peer id
    std::uint64_t max_peer_id_{0};         // 心跳里见过的最大 peer id（抬升分配基线）
    std::uint64_t split_count_{0};
};

}  // namespace tinykv::scheduler
