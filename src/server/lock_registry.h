// 文件用途：⑨a 锁分片——按 RegionId 固定分桶的互斥注册表（M7-PERF-01 的落点）。
// 归属：TinyKV-CPP（M7 演进）。
//
// 接线约定（⑦ 收口后接入组合根，SPEC-6 §6.1.2"一次只放开一个自由度"）：
//   * 分区闸：单 region 操作只锁该 region 的桶——桶间并行是本类的全部收益；
//   * 全局闸：跨 region 操作（Worker 泵、split、status、GC 广播、锁决议扫描）
//     按"桶下标升序"锁全部桶——固定加锁顺序，不可能成环；
//   * 多副本（multi-voter）不启用分片：泵路径触碰全部 peer + transport，
//     收益为零、风险不为零，保守退回一把大锁（等价 shards=1）。
//
// 不变量（SPEC-6 §6.5 红线 5）：
//   * 同一 RegionId 的 apply 仍严格串行（同一桶，同一把 mutex）；
//   * 跨 region 之间没有全局顺序要求，桶间可并行；
//   * shards == 1 时退化为"一把大锁"，与改造前完全一致。

#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "common/ids.h"

namespace tinykv::server {

class LockRegistry {
public:
    // shards 固定桶数：按 RegionId 取模。shards=0 视作 1（退化档，永不出现"零桶"）
    explicit LockRegistry(std::size_t shards = 16) : buckets_(shards == 0 ? 1 : shards) {}

    // 取该 region 所属桶的独占锁；返回 RAII 句柄，析构即释放
    [[nodiscard]] std::unique_lock<std::mutex> lock(RegionId id) {
        return std::unique_lock<std::mutex>(buckets_[bucket_of(id)]);
    }

    // 全局闸：按桶下标升序逐把锁——所有全局操作共用这一顺序，锁序一致即无死锁
    [[nodiscard]] std::vector<std::unique_lock<std::mutex>> lock_all() {
        std::vector<std::unique_lock<std::mutex>> out;
        out.reserve(buckets_.size());
        for (auto& bucket : buckets_) {
            out.emplace_back(bucket);
        }
        return out;
    }

    // 全局闸的非阻塞版（观测端点专用：拿不到全部桶就回退缓存快照，绝不因业务泵卡住观测）
    [[nodiscard]] std::optional<std::vector<std::unique_lock<std::mutex>>> try_lock_all() {
        std::vector<std::unique_lock<std::mutex>> out;
        out.reserve(buckets_.size());
        for (auto& bucket : buckets_) {
            out.emplace_back(bucket, std::try_to_lock);
            if (!out.back().owns_lock()) {
                return std::nullopt;  // 已持有的锁随 out 析构按逆序释放，无残留
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t shard_count() const { return buckets_.size(); }
    [[nodiscard]] std::size_t bucket_of(RegionId id) const { return id.value % buckets_.size(); }

private:
    std::vector<std::mutex> buckets_;  // vector<mutex> 不可拷贝，故本类天然不可拷贝
};

}  // namespace tinykv::server
