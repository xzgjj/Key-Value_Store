// 文件用途：规模档 split 实测（M8 ⑬ PERF-07，SPEC-7 §7.2.3）——1M key 均匀写入完成后
//          对中点做一次 admin split（含数据迁移）计时，对照 S0 ≤60s / S1 ≤30s / S2 ≤15s。
// 归属：TinyKV-CPP（M8 优化，LEVEL=bench → nightly;serial;timeout:3600s）。
//
// 与 split_bench 的分工：split_bench 考察"写入过程中连续 split 的机制开销"（4 次小规模
// 递进切分）；本用例考察"装满 1M key 后单次 split 的数据迁移代价"——PERF-07 的原始口径
// （1M key 均匀分布 → split 完成时间）。计时留痕由 perf_report.py（⑫）采数判读，
// bench 不做硬失败断言（testing.md §3，D7-4）。
// 公共装置（定宽键/批量提案/单 peer 装配/有界轮询）在 testlib/peer_cluster.h。

#include "common/error_format.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "peer_cluster.h"
#include "raft_test_base.h"
#include "temp_dir.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::raftstore::Peer;
using tinykv::raftstore::StoreMeta;

constexpr std::int64_t kKeys = 1'000'000;
constexpr int kBatch = 512;

TEST(ScaleSplit, MillionKeysUniformThenSingleSplitTimed) {
    TempDir tmp("tinykv_scale_split");
    auto opened = tinykv::Engine::open({.dir = tmp.path() / "db", .fsync_on_commit = false});
    ASSERT_TRUE(opened.has_value());
    std::unique_ptr<tinykv::Engine> engine = std::move(*opened);

    StoreMeta meta;
    auto peer = open_single_leader_peer(*engine, meta);
    ASSERT_NE(peer, nullptr) << "单 peer 装配失败";

    // 1M key 均匀写入（不计入 split 计时——考察点是 split 本身的迁移代价）
    std::uint64_t seq = 0;
    for (std::int64_t begin = 0; begin < kKeys; begin += kBatch) {
        const std::int64_t end = std::min(begin + kBatch, kKeys);
        ++seq;
        ASSERT_TRUE(peer->propose(make_put_batch(1, seq, begin, end)).has_value());
        ASSERT_TRUE(wait_reply(*peer, 1, seq, 400)) << "批量写入未在 400 轮内应用";
    }

    // 单次中点 split（含数据迁移）：计时对象
    const auto t0 = std::chrono::steady_clock::now();
    ++seq;
    const auto split_key = make_padded_key(kKeys / 2);
    ASSERT_TRUE(peer->propose(make_split_admin(1, seq, split_key, 2)).has_value());
    std::string apply_err;
    const bool applied = wait_reply(*peer, 1, seq, 400, &apply_err);
    ASSERT_TRUE(applied) << "中点 split apply 失败：" << apply_err;
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const double s0_line = 60.0, s1_line = 30.0, s2_line = 15.0;
    std::printf("\n[PERF-07] 1M key 均匀分布 单次中点 split（含迁移）: %.1f s\n", secs);
    std::printf("[PERF-07] S0≤%.0fs %s | S1≤%.0fs %s | S2≤%.0fs %s\n",
                s0_line, secs <= s0_line ? "✔" : "✘",
                s1_line, secs <= s1_line ? "✔" : "✘",
                s2_line, secs <= s2_line ? "✔" : "✘");

    // 结构性断言：拓扑生效 + 区间连续（REL-05）+ 两侧抽查可读
    EXPECT_EQ(meta.region_count(), 2U);
    EXPECT_TRUE(regions_contiguous(meta.all_regions())) << "split 后区间必须首尾相接";
    for (const std::int64_t probe : {std::int64_t{0}, kKeys / 2 - 1, kKeys / 2, kKeys - 1}) {
        auto got = peer->get(tinykv::Cf::Default, make_padded_key(probe));
        ASSERT_TRUE(got.has_value());
        EXPECT_TRUE(got->value.has_value()) << "probe=" << probe;
    }
}

}  // namespace
