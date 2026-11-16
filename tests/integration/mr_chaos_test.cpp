// 文件用途：Multi-Raft 150 轮 chaos——split + confchange 叠加注入，修复后必须收敛
//          （M4 第⑦c层，ENG-05，SPEC-3 §3.7 mr_chaos_n150）。
// 归属：TinyKV-CPP（M4 集成层，integration/nightly）。
//
// 与 M2 raft_chaos_n150 的分工：那个只注入丢包+提案；本文件在此之上叠加
// **split（③层）+ 成员变更（④层）+ 随机分区**，并断言 Multi-Raft 特有的不变量：
//   (a) 每个 peer 的 StoreMeta 区间首尾相接、无重叠无空洞（REL-05：每 key 恰属一 region）；
//   (b) 修复后三副本的 region 视图一致（split 是共识条目，各副本各自 apply 各自视图）；
//   (c) 混沌期间写入的已提交数据在修复后仍然可读（不丢已提交）；
//   (d) conf change 走完 AddLearner→RemoveNode 全程后 voters/learners 回到原状。

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "peer_cluster.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"
#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::RegionEpoch;
using tinykv::cmdpb::RaftCmdRequest;
using tinykv::raftstore::Region;

constexpr int kRounds = 150;

RaftCmdRequest make_split_request(std::uint64_t client, std::uint64_t seq,
                                         const std::string& key, std::uint64_t new_id) {
    RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    auto* admin = req.mutable_admin();
    admin->mutable_split()->set_split_key(key);
    admin->mutable_split()->set_new_region_id(new_id);
    return req;
}

// REL-05 的可判定形式：区间排序后首尾相接（首 start=""、尾 end=""、相邻共享边界）
bool contiguous(const std::vector<Region>& regions) {
    std::vector<Region> sorted = regions;
    std::sort(sorted.begin(), sorted.end(),
              [](const Region& a, const Region& b) { return a.start_key < b.start_key; });
    if (sorted.empty() || !sorted.front().start_key.empty() || !sorted.back().end_key.empty()) {
        return false;
    }
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1].end_key != sorted[i].start_key) {
            return false;
        }
    }
    return true;
}

// region 视图的指纹（跨副本一致性比对用）
std::string regions_fingerprint(const std::vector<Region>& regions) {
    std::vector<Region> sorted = regions;
    std::sort(sorted.begin(), sorted.end(),
              [](const Region& a, const Region& b) { return a.start_key < b.start_key; });
    std::string out;
    for (const auto& r : sorted) {
        out += std::to_string(r.id.value) + ":" + r.start_key + ".." + r.end_key + ";";
    }
    return out;
}

int count_leaders(PeerCluster& c) {
    int n = 0;
    for (int i = 0; i < c.size(); ++i) {
        n += c.peer(i).is_leader() ? 1 : 0;
    }
    return n;
}

// 单拍注入：分区抖动 + 提案 + split + confchange（i==20 无分区时 split，三副本各 apply 各视图）
void inject(PeerCluster& c, int i, std::mt19937& rng, std::vector<std::string>& written) {
    for (int n = 0; n < c.size(); ++n) {
        c.peer(n).tick();
    }
    c.pump();
    // 随机分区：每轮 20% 概率隔离一个节点（比固定分区更接近真实抖动）
    if (std::uniform_int_distribution<unsigned>(0, 4)(rng) == 0) {
        const int victim =
            static_cast<int>(std::uniform_int_distribution<unsigned>(0, 2)(rng));
        c.set_partition({victim});
    } else {
        c.set_partition({});
    }
    const int leader = c.leader();
    if (leader < 0) {
        return;   // 无 leader 的拍：注入自然失败，属混沌的一部分
    }
    if (i % 4 == 0 && c.write(leader, 9, static_cast<std::uint64_t>(i),
                              "k" + std::to_string(i), "v" + std::to_string(i))) {
        written.push_back("k" + std::to_string(i));
    }
    if (i == 20) {
        (void)c.peer(leader).propose(make_split_request(88, 1, "m", 2));
        for (int k = 0; k < 50; ++k) {
            c.pump();
        }
    }
    // confchange 注入：加 learner（无实例，消息自然丢弃）再移除，全程 voters 不动
    if (i == 30) {
        tinykv::raftpb::ConfChange cc;
        cc.set_type(tinykv::raftpb::ConfAddLearner);
        cc.set_node_id(4);
        (void)c.peer(leader).raft().propose_conf_change(cc);
    }
    if (i == 36) {
        tinykv::raftpb::ConfChange cc;
        cc.set_type(tinykv::raftpb::ConfRemoveNode);
        cc.set_node_id(4);
        (void)c.peer(leader).raft().propose_conf_change(cc);
    }
}

// 修复后收敛判据：唯一 leader + 任期一致（与 M2 chaos 同口径）
bool converged(PeerCluster& c) {
    if (count_leaders(c) != 1) {
        return false;
    }
    for (int n = 1; n < c.size(); ++n) {
        if (c.peer(n).raft().term() != c.peer(0).raft().term()) {
            return false;
        }
    }
    return true;
}

// Multi-Raft 特有不变量：region 连续覆盖、三副本视图一致、split 生效、membership 复原、数据不丢
bool mr_invariants_hold(PeerCluster& c, const std::vector<std::string>& written) {
    const auto fp = regions_fingerprint(c.meta(0).all_regions());
    for (int n = 0; n < c.size(); ++n) {
        if (!contiguous(c.meta(n).all_regions())) {
            return false;
        }
        if (regions_fingerprint(c.meta(n).all_regions()) != fp) {
            return false;
        }
    }
    if (c.meta(0).region_count() != 2) {
        return false;   // split 必须已生效
    }
    const int leader = c.leader();
    for (const auto& key : written) {
        auto got = c.peer(leader).read_directly(tinykv::Cf::Default, key);
        if (!got.has_value() || !got->value.has_value()) {
            return false;   // 已提交数据丢失
        }
    }
    return c.peer(leader).raft().learners().empty();   // conf change 全程后无残留
}

bool run_round(unsigned seed) {
    PeerCluster c(3);
    Region initial{.id = tinykv::RegionId{1}, .start_key = "", .end_key = "",
                   .peers = {}, .epoch = RegionEpoch{1, 0}, .size_hint = 0};
    c.enable_split(initial);
    if (!c.elect()) {
        return false;
    }
    std::mt19937 rng(seed);
    std::vector<std::string> written;
    for (int i = 0; i < 40; ++i) {
        inject(c, i, rng, written);
    }
    // 修复网络：必须重新收敛
    c.set_partition({});
    if (!c.elect()) {
        return false;
    }
    for (int k = 0; k < 60; ++k) {
        for (int n = 0; n < c.size(); ++n) {
            c.peer(n).tick();
        }
        c.pump();
    }
    return converged(c) && mr_invariants_hold(c, written);
}

TEST(MrChaosN150, SplitAndConfChangeUnderChaosConverges) {
    std::vector<unsigned> failed;
    for (int round = 0; round < kRounds; ++round) {
        const unsigned seed = 5000U + static_cast<unsigned>(round);
        if (!run_round(seed)) {
            failed.push_back(seed);
        }
    }
    ASSERT_TRUE(failed.empty()) << "失败种子数 " << failed.size() << "，首个失败种子 "
                                << (failed.empty() ? 0U : failed.front());
}

}  // namespace
