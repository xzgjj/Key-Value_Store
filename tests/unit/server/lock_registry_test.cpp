// 文件用途：⑨a 锁分片用例——同 region 串行、跨 region 并行、shards=1 退化等价。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `lock_registry_test`）。
//
// 使用场景理解：分片锁的正确性不能用"跑得快"证明，只能用两件事证明：
//   1) 同一个 region 的两个临界区不能同时进入（否则同 region 的 apply 顺序被打乱）；
//   2) 不同 region 的临界区可以同时进入（否则分片没意义）。
// 另外必须钉住退化档：shards=1 时所有 region 共用一把锁，行为与改造前逐字节一致。

#include "server/lock_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using tinykv::RegionId;
using tinykv::server::LockRegistry;

// 有界空转（ENG-04 禁 sleep_for：它既不可控也掩盖真实争用）
void spin_a_while() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
}

// 两个线程各自加锁一次，返回"同时进入临界区的最大线程数"。
// 互斥类用例的断言面只有这一个数字：1 = 串行，2 = 并行。
int max_concurrent_entries(LockRegistry& locks, RegionId first, RegionId second,
                           std::atomic<int>* started = nullptr) {
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> local_started{0};
    std::atomic<int>& gate = started != nullptr ? *started : local_started;
    const auto body = [&](RegionId id) {
        auto guard = locks.lock(id);
        ++inside;
        max_inside = std::max(max_inside.load(), inside.load());
        ++gate;
        spin_a_while();
        --inside;
    };
    std::thread a(body, first);
    std::thread b(body, second);
    a.join();
    b.join();
    return max_inside.load();
}

// 同 region：两次加锁必须互斥（第二个线程只能在第一个释放后进入）
TEST(LockRegistryTest, SameRegionIsSerialized) {
    LockRegistry locks(16);
    EXPECT_EQ(max_concurrent_entries(locks, RegionId{7}, RegionId{7}), 1)
        << "同 region 的临界区被并发进入（串行性被破坏）";
}

// 不同 region 且落在不同桶：可同时进入（否则分片形同虚设）
TEST(LockRegistryTest, DifferentRegionsCanRunConcurrently) {
    LockRegistry locks(4);  // region 1 与 region 2 落桶 1/2，必然不同桶
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> started{0};
    const auto body = [&](RegionId id) {
        auto guard = locks.lock(id);
        ++inside;
        max_inside = std::max(max_inside.load(), inside.load());
        ++started;
        // 等到两个线程都进来（或超时），以此判定"真的并行"
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        --inside;
    };
    std::thread a(body, RegionId{1});
    std::thread b(body, RegionId{2});
    a.join();
    b.join();
    EXPECT_EQ(max_inside.load(), 2) << "不同 region 未能并行（分片未生效）";
}

// 退化档：shards=1 → 全部 region 同桶，等价改造前的一把大锁
TEST(LockRegistryTest, SingleShardDegradesToGlobalLock) {
    LockRegistry locks(1);
    EXPECT_EQ(locks.shard_count(), 1U);
    EXPECT_EQ(locks.bucket_of(RegionId{1}), locks.bucket_of(RegionId{99}));

    EXPECT_EQ(max_concurrent_entries(locks, RegionId{42}, RegionId{99}), 1)
        << "shards=1 时必须退化为全局串行";
}

// 桶分配是纯函数：同一 id 永远落同一桶（否则同一 region 会被两把锁保护）
TEST(LockRegistryTest, BucketAssignmentIsStable) {
    LockRegistry locks(8);
    EXPECT_EQ(locks.bucket_of(RegionId{5}), locks.bucket_of(RegionId{5}));
    EXPECT_EQ(locks.bucket_of(RegionId{5}), 5U);
    EXPECT_EQ(locks.bucket_of(RegionId{13}), 5U);  // 13 % 8
    LockRegistry zero(0);
    EXPECT_EQ(zero.shard_count(), 1U);  // 0 视作 1，不出现零桶
}

}  // namespace
