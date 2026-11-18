// 文件用途：1M key 均匀分布下的连续 split 基准（PERF-07，SPEC-3 §3.7）。
// 归属：TinyKV-CPP（M4 Multi-Raft，LEVEL=bench → nightly）。
// 使用场景理解：S0 要求"1M key 写入过程中连续 split 完成 ≤60s"。这里测的是
// **split 机制本身在持续写入下的开销**：单 peer 走最快共识路径（Replication 不在考察范围），
// 每 25% 进度对当前左区间做一次 admin split（apply_split_admin 只作用于 my_region_，
// 故 split key 必须落在左半，取递减序列）。断言只做结构性的：split 全部生效、
// 区间首尾相接（REL-05）、抽查键可读；S0 阈值由人工读数判定（机器差异太大）。

#include "common/error_format.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "peer_cluster.h"
#include "raft_test_base.h"
#include "temp_dir.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::raftstore::Peer;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;

constexpr std::int64_t kKeys = 1'000'000;
constexpr int kBatch = 512;          // Envelope 攒批上限内一次多塞

// 定宽键/批量提案/单 peer 装配/有界轮询：M8 ⑬ 提取到 testlib/peer_cluster.h 共享
// （make_padded_key / make_put_batch / open_single_leader_peer / wait_reply）。

TEST(SplitBench, MillionKeysWithContinuousSplits) {
    TempDir tmp("tinykv_split_bench");
    auto opened = tinykv::Engine::open({.dir = tmp.path() / "db", .fsync_on_commit = false});
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<tinykv::Engine> engine = std::move(*opened);

    StoreMeta meta;
    auto peer = open_single_leader_peer(*engine, meta);
    ASSERT_NE(peer, nullptr) << "单 peer 装配失败";

    // 递减 split 点：apply_split_admin 只作用于左区间（my_region_），每次切完左半收窄
    const std::vector<std::int64_t> split_points = {kKeys / 2, kKeys / 4, kKeys / 8, kKeys / 16};
    std::uint64_t seq = 0;
    std::size_t split_ok = 0;

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t next_split = 0;
    for (std::int64_t begin = 0; begin < kKeys; begin += kBatch) {
        const std::int64_t end = std::min(begin + kBatch, kKeys);
        ++seq;
        ASSERT_TRUE(peer->propose(make_put_batch(1, seq, begin, end)).has_value());
        (void)wait_reply(*peer, 1, seq, 400);  // 批量写入等待应用（有界轮询）
        // 进度到 split 点：插入一次 admin split（当前左区间内递减取点）
        if (next_split < split_points.size() && end > split_points[next_split]) {
            ++seq;
            const auto key = make_padded_key(split_points[next_split]);
            ASSERT_TRUE(peer->propose(make_split_admin(1, seq, key, next_split + 2)).has_value());
            std::string apply_err;
            const bool applied = wait_reply(*peer, 1, seq, 400, &apply_err);
            EXPECT_TRUE(applied) << "split@" << key << " apply 失败：" << apply_err;
            ++next_split;
            if (applied) {
                ++split_ok;
            }
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("[bench] 1M key 连续 split %zu 次 : %.1f s（%.0f keys/s）\n", split_ok, secs,
                static_cast<double>(kKeys) / secs);
    std::printf("[bench] 最终 region 数 : %zu\n", meta.region_count());

    // 结构性断言：split 全部生效、区间连续、抽查首尾键可读
    EXPECT_EQ(split_ok, split_points.size());
    EXPECT_EQ(meta.region_count(), split_points.size() + 1U);
    EXPECT_TRUE(regions_contiguous(meta.all_regions())) << "split 后区间必须首尾相接（REL-05）";
    for (const std::int64_t probe : {std::int64_t{0}, std::int64_t{kKeys / 2}, std::int64_t{kKeys - 1}}) {
        auto got = peer->get(tinykv::Cf::Default, make_padded_key(probe));
        ASSERT_TRUE(got.has_value());
        EXPECT_TRUE(got->value.has_value()) << "probe=" << probe;
    }
}

}  // namespace
