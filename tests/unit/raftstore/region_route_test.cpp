// 文件用途：Region 路由与 epoch 校验单测（M4 第②层，FUNC-07/REL-05）。
// 归属：TinyKV-CPP（M4 Multi-Raft 路由层）。
// 使用场景理解：StoreMeta 维护 region 集合与按 end_key 的区间索引；Router 按 key 路由并校验 epoch。
// 必须证明：
//   (a) 每 key 恰好落一个 region（区间首尾相接、无重叠、无空洞）——REL-05 不变量；
//   (b) epoch 过旧的路由请求被拒并带 StaleEpoch{have,want}（hint 驱动客户端刷新）；
//   (c) key 越界被拒带 KeyNotInRegion；
//   (d) split 的 replace_region 原子替换：旧 region 消失、两个新 region 立即可路由。

#include "raftstore/region.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace tinykv;   // NOLINT(google-build-using-namespace)

// 构造一个 [start, end) 的 region
tinykv::raftstore::Region make_region(std::uint64_t id, std::string start, std::string end,
                                      RegionEpoch epoch = {1, 0}) {
    return tinykv::raftstore::Region{
        .id = RegionId{id},
        .start_key = std::move(start),
        .end_key = std::move(end),
        .peers = {},
        .epoch = epoch,
    };
}

TEST(RegionRoute, OverlappingRegionIsRejected) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "a", "z")).has_value());
    // 与既有 [a,z) 重叠
    EXPECT_FALSE(meta.add_region(make_region(2, "m", "zz")).has_value());
    // 完全落在 +∞ 区间之外（不存在 +∞ 区间时）允许
    EXPECT_TRUE(meta.add_region(make_region(3, "z", "")).has_value());
    // 再想加一个 +∞ 区间应被拒（已存在 [z,∞)）
    EXPECT_FALSE(meta.add_region(make_region(4, "zz", "")).has_value());
}

TEST(RegionRoute, FindsRegionByKeyWithHalfOpenRange) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "m")).has_value());
    ASSERT_TRUE(meta.add_region(make_region(2, "m", "")).has_value());

    auto a = meta.find_region("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, RegionId{1});
    auto m = meta.find_region("m");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->id, RegionId{2});          // 边界 key 归右区间 [m, ∞)
    auto z = meta.find_region("zzz");
    ASSERT_TRUE(z.has_value());
    EXPECT_EQ(z->id, RegionId{2});
}

TEST(RegionRoute, EveryKeyBelongsToExactlyOneRegion) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "c")).has_value());
    ASSERT_TRUE(meta.add_region(make_region(2, "c", "x")).has_value());
    ASSERT_TRUE(meta.add_region(make_region(3, "x", "")).has_value());

    for (char c = 'a'; c <= 'z'; ++c) {
        EXPECT_EQ(meta.find_region(std::string(1, c)).has_value(), true)
            << "key " << c << " 未落到任何 region";
    }
    EXPECT_EQ(meta.region_count(), 3U);
}

TEST(RegionRoute, StaleEpochIsRejectedWithHint) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", {5, 2})).has_value());
    tinykv::raftstore::Router router(meta);

    const RegionEpoch stale{4, 2};          // version 落后
    auto r = router.route("any", stale);
    EXPECT_FALSE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<StaleEpoch>(r.error()));
    const auto& se = std::get<StaleEpoch>(r.error());
    EXPECT_EQ(se.region_id, RegionId{1});
    EXPECT_EQ(se.have.version, 4U);
    EXPECT_EQ(se.want.version, 5U);
}

TEST(RegionRoute, FreshEpochPasses) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", {5, 2})).has_value());
    tinykv::raftstore::Router router(meta);

    auto r = router.route("any", RegionEpoch{5, 2});   // 完全一致
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(r->id, RegionId{1});
    auto newer = router.route("any", RegionEpoch{6, 2});   // 比 region 还新：可比且非 stale
    EXPECT_TRUE(newer.has_value());
}

TEST(RegionRoute, MixedOrderEpochIsRejected) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", {5, 5})).has_value());
    tinykv::raftstore::Router router(meta);
    // 混序：version 大、conf_ver 小 → 不可比，按 stale 拒绝
    auto r = router.route("any", RegionEpoch{6, 4});
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<StaleEpoch>(r.error()));
}

TEST(RegionRoute, SplitReplaceIsAtomicAndImmediate) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", {1, 0})).has_value());
    // split [,"") at "m" → 左 [,"m") id=1 version=2，右 [m,"") id=2
    ASSERT_TRUE(meta.replace_region(RegionId{1},
                                    make_region(1, "", "m", {2, 0}),
                                    make_region(2, "m", "")).has_value());
    auto a = meta.find_region("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, RegionId{1});
    EXPECT_EQ(a->epoch.version, 2U);
    auto z = meta.find_region("z");
    ASSERT_TRUE(z.has_value());
    EXPECT_EQ(z->id, RegionId{2});
    EXPECT_EQ(meta.region_count(), 2U);
    EXPECT_FALSE(meta.find_region_by_id(RegionId{1})->epoch.version == 1U);
}

TEST(RegionRoute, ConcurrentReadsDuringSplitStayConsistent) {
    tinykv::raftstore::StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", {1, 0})).has_value());
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                // 并发读期间，任意 key 必须命中唯一 region（不得出现空）
                for (char c = 'a'; c <= 'z'; c += 5) {
                    EXPECT_TRUE(meta.find_region(std::string(1, c)).has_value());
                }
                std::this_thread::yield();
            }
        });
    }
    ASSERT_TRUE(meta.replace_region(RegionId{1},
                                    make_region(1, "", "m", {2, 0}),
                                    make_region(2, "m", "")).has_value());
    done.store(true, std::memory_order_release);
    for (auto& t : readers) {
        t.join();
    }
}

}  // namespace
