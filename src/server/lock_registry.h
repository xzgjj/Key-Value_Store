// 文件用途：⑨a 锁分片——按 RegionId 固定分桶的互斥注册表（M7-PERF-01 的落点）。
// 归属：TinyKV-CPP（M7 演进）。
//
// 为什么先有类、后接组合根（而不是一步改掉大闸）：
//   ARCH-3 §2 的并行约束写得很清楚——"⑨a 必须在 W1 之后"，且先要把 ⑦ 的协议语义跑对，
//   否则并发 bug 与协议 bug 纠缠、无法归因。因此本类先以 `shards=1`（等价改造前）随组合根
//   待命：默认行为逐字节不变，等 ⑦ 收口后再把 shard 数调大。
//
// 不变量（SPEC-6 §6.5 红线 5）：
//   * 同一 RegionId 的 apply 仍严格串行（同一桶，同一把 mutex）；
//   * 跨 region 之间没有全局顺序要求，桶间可并行；
//   * shards == 1 时退化为"一把大锁"，与改造前完全一致。

#pragma once

#include <cstddef>
#include <mutex>
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

    [[nodiscard]] std::size_t shard_count() const { return buckets_.size(); }
    [[nodiscard]] std::size_t bucket_of(RegionId id) const { return id.value % buckets_.size(); }

private:
    std::vector<std::mutex> buckets_;  // vector<mutex> 不可拷贝，故本类天然不可拷贝
};

}  // namespace tinykv::server
