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
using tinykv::cmdpb::RaftCmdRequest;
using tinykv::raftstore::Peer;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;

constexpr std::int64_t kKeys = 1'000'000;
constexpr int kBatch = 512;          // Envelope 攒批上限内一次多塞

// 定宽补零键：字典序 == 数值序（否则 split 点的"落在左区间"判断会被字符串序打乱——
// 本 bench 首跑的真实踩坑："k125000" < "k62500"，字典序与数值序相反）
std::string make_key(std::int64_t i) {
    auto digits = std::to_string(i);
    return "k" + std::string(7 - digits.size(), '0') + digits;
}

RaftCmdRequest make_batch(std::uint64_t client, std::uint64_t seq, std::int64_t begin,
                                 std::int64_t end) {
    RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    for (std::int64_t i = begin; i < end; ++i) {
        auto* one = req.add_requests();
        one->mutable_put()->set_key(make_key(i));
        one->mutable_put()->set_value("v");
    }
    return req;
}

TEST(SplitBench, MillionKeysWithContinuousSplits) {
    TempDir tmp("tinykv_split_bench");
    auto opened = tinykv::Engine::open({.dir = tmp.path() / "db", .fsync_on_commit = false});
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<tinykv::Engine> engine = std::move(*opened);

    tinykv::raft::Config cfg{.id = PeerId{1}, .voters = {PeerId{1}},
                             .election_timeout = 4, .heartbeat_interval = 2};
    auto peer_result = Peer::open(cfg, *engine);
    ASSERT_TRUE(peer_result.has_value());
    std::unique_ptr<Peer> peer = std::move(*peer_result);

    StoreMeta meta;
    Region region{.id = tinykv::RegionId{1}, .start_key = "", .end_key = "",
                  .peers = {}, .epoch = {1, 0}, .size_hint = 0};
    ASSERT_TRUE(meta.add_region(region).has_value());
    peer->set_region(region);
    peer->set_store_meta(&meta);
    for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
        peer->tick();
    }
    ASSERT_TRUE(peer->is_leader());

    // 递减 split 点：apply_split_admin 只作用于左区间（my_region_），每次切完左半收窄
    const std::vector<std::int64_t> split_points = {kKeys / 2, kKeys / 4, kKeys / 8, kKeys / 16};
    std::uint64_t seq = 0;
    std::size_t split_ok = 0;

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t next_split = 0;
    for (std::int64_t begin = 0; begin < kKeys; begin += kBatch) {
        const std::int64_t end = std::min(begin + kBatch, kKeys);
        ++seq;
        ASSERT_TRUE(peer->propose(make_batch(1, seq, begin, end)).has_value());
        for (int round = 0; round < 400; ++round) {
            if (peer->on_ready().has_value() &&
                peer->take_reply(1, seq).has_value()) {
                break;
            }
            peer->tick();
        }
        // 进度到 split 点：插入一次 admin split（当前左区间内递减取点）
        if (next_split < split_points.size() && end > split_points[next_split]) {
            ++seq;
            const auto key = make_key(split_points[next_split]);
            ASSERT_TRUE(peer->propose(make_split_admin(1, seq, key, next_split + 2)).has_value());
            bool applied = false;
            std::string apply_err;
            for (int round = 0; round < 400 && !applied; ++round) {
                auto r = peer->on_ready();
                if (!r) {
                    apply_err = std::string(tinykv::describe(r.error()));   // apply 拒绝：记下原因
                    continue;
                }
                if (peer->take_reply(1, seq).has_value()) {
                    applied = true;
                }
            }
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
        auto got = peer->get(tinykv::Cf::Default, make_key(probe));
        ASSERT_TRUE(got.has_value());
        EXPECT_TRUE(got->value.has_value()) << "probe=" << probe;
    }
}

}  // namespace
