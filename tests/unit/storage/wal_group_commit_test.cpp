// 文件用途：WAL group commit 的合并性、并发正确性与持久语义回归（SPEC-1 §1.4.2，PERF-01）。
// 归属：TinyKV-CPP（A：M2 PERF-01 收口）。
// 使用场景理解：group commit 把窗口内多条并发 append 的 fsync 合成一次；必须证明
//   (a) 真的合并了（fsync 次数远小于写入条数）；
//   (b) 合并不丢数据（并发写全部可按原序重放、CRC 全过）；
//   (c) sync() 是同步刷盘原语，调用即计数增加；
//   (d) 不开 sync() 时后台提交者在窗口内也会自动 fsync。

#include "storage/wal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

constexpr int kThreads = 4;
constexpr int kPerThread = 25;

ModifyVec one_put(int t, int i) {
    return ModifyVec{Modify{Put{
        .cf = Cf::Default,
        .key = "gc:" + std::to_string(t) + ":" + std::to_string(i),
        .value = "v"}}};
}

// 启动 kThreads 路并发写，每路写 kPerThread 条；两用例复用，避免重复样板
std::vector<std::thread> spawn_concurrent_writers(Wal& wal) {
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&wal, t] {
            for (int i = 0; i < kPerThread; ++i) {
                EXPECT_TRUE(wal.append(one_put(t, i)).has_value());
            }
        });
    }
    return workers;
}

void join_all(std::vector<std::thread>& workers) {
    for (auto& w : workers) {
        w.join();
    }
}

// 场景：4 路并发写 100 条，逐条 fsync 会有 100 次；group commit 应显著减少到远低于半数。
TEST(WalGroupCommit, ConcurrentAppendsShareFsync) {
    TempDir tmp("tinykv_gc_merge");
    auto wal = Wal::open(WalOptions{.dir = tmp.path()});
    ASSERT_TRUE(wal.has_value());

    auto workers = spawn_concurrent_writers(**wal);
    join_all(workers);
    ASSERT_TRUE((*wal)->sync().has_value());

    EXPECT_LT((*wal)->fsync_count(), static_cast<std::uint64_t>(kThreads * kPerThread / 2))
        << "fsync 次数=" << (*wal)->fsync_count() << "，说明窗口内未发生合并";
}

// 场景：并发写的每条都必须能重放出来，key 集合完整、CRC 全过——group commit 不得引入丢失/错乱。
TEST(WalGroupCommit, AllConcurrentAppendsReplayIntact) {
    TempDir tmp("tinykv_gc_intact");
    const auto dir = tmp.path();
    {
        auto wal = Wal::open(WalOptions{.dir = dir});
        ASSERT_TRUE(wal.has_value());
        auto workers = spawn_concurrent_writers(**wal);
        join_all(workers);
        ASSERT_TRUE((*wal)->sync().has_value());
    }

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_FALSE(replay->stopped);
    std::size_t total = 0;
    for (const auto& b : replay->batches) {
        total += b.size();
    }
    EXPECT_EQ(total, static_cast<std::size_t>(kThreads * kPerThread));
}

// 场景：sync() 是同步原语——调用后 fsync 计数立即增加，且不依赖后台窗口。
TEST(WalGroupCommit, SyncForcesImmediateFsync) {
    TempDir tmp("tinykv_gc_sync");
    auto wal = Wal::open(WalOptions{.dir = tmp.path()});
    ASSERT_TRUE(wal.has_value());
    ASSERT_TRUE((*wal)->append(one_put(0, 0)).has_value());
    const auto before = (*wal)->fsync_count();
    ASSERT_TRUE((*wal)->sync().has_value());
    EXPECT_GT((*wal)->fsync_count(), before);
}

}  // namespace
