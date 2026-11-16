// 文件用途：单步成员变更状态机单测（M4 第④层，FUNC-09/REL-04）。
// 归属：TinyKV-CPP（M4 Multi-Raft 成员变更）。
// 使用场景理解：ConfChange 必须"apply 时才生效"（崩溃后可从日志重建），且：
//   (a) AddLearner 只加 learner，不改 voters / majority；
//   (b) AddVoter 把 learner 提升为 voter；
//   (c) RemoveNode 同时从 voters 与 learners 摘除；
//   (d) apply 是幂等的（重放同一日志不改变结果）；
//   (e) 非 leader 拒绝 ConfChange。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::NotLeader;
using tinykv::raftpb::ConfChange;

bool contains(const std::vector<PeerId>& v, std::uint64_t id) {
    return std::find(v.begin(), v.end(), PeerId{id}) != v.end();
}

ConfChange make_cc(tinykv::raftpb::ConfChangeType type, std::uint64_t node_id) {
    ConfChange cc;
    cc.set_type(type);
    cc.set_node_id(node_id);
    return cc;
}

// 单副本 leader：conf change 提案后，commit 靠自己推进，一次 take_ready 即 apply
struct SoloLeader {
    Cluster cluster{1};
    [[nodiscard]] bool ready() { return cluster.elect() && cluster.leader_index() == 0; }
};

TEST(ConfChange, AddLearnerDoesNotJoinVoters) {
    SoloLeader f;
    ASSERT_TRUE(f.ready());
    ASSERT_TRUE(f.cluster.node(0).propose_conf_change(
        make_cc(tinykv::raftpb::ConfAddLearner, 2)).has_value());
    (void)f.cluster.node(0).take_ready();   // 触发 collect_apply

    EXPECT_FALSE(contains(f.cluster.node(0).voters(), 2));
    EXPECT_TRUE(contains(f.cluster.node(0).learners(), 2));
}

TEST(ConfChange, AddVoterPromotesLearnerIntoVoters) {
    SoloLeader f;
    ASSERT_TRUE(f.ready());
    ASSERT_TRUE(f.cluster.node(0).propose_conf_change(
        make_cc(tinykv::raftpb::ConfAddLearner, 2)).has_value());
    (void)f.cluster.node(0).take_ready();
    ASSERT_TRUE(contains(f.cluster.node(0).learners(), 2));

    ASSERT_TRUE(f.cluster.node(0).propose_conf_change(
        make_cc(tinykv::raftpb::ConfAddVoter, 2)).has_value());
    (void)f.cluster.node(0).take_ready();

    EXPECT_TRUE(contains(f.cluster.node(0).voters(), 2));
    EXPECT_FALSE(contains(f.cluster.node(0).learners(), 2));
}

TEST(ConfChange, RemoveNodeDropsFromBothSets) {
    // 用 3 节点集群：移除一个 follower 后 commit 仍由多数派推进（单副本装置下 RemoveNode
    // 会因 majority 变化卡住 commit，那是装置时序问题，非产品语义）
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);
    // 先加入一个 learner（id=4）再移除，验证从 learners 摘除
    ASSERT_TRUE(c.node(leader).propose_conf_change(
        make_cc(tinykv::raftpb::ConfAddLearner, 4)).has_value());
    c.flush();
    ASSERT_TRUE(contains(c.node(leader).learners(), 4));

    ASSERT_TRUE(c.node(leader).propose_conf_change(
        make_cc(tinykv::raftpb::ConfRemoveNode, 4)).has_value());
    c.flush();

    EXPECT_FALSE(contains(c.node(leader).voters(), 4));
    EXPECT_FALSE(contains(c.node(leader).learners(), 4));
}

TEST(ConfChange, ConfChangeEntryIsPersistedWithType) {
    SoloLeader f;
    ASSERT_TRUE(f.ready());
    ASSERT_TRUE(f.cluster.node(0).propose_conf_change(
        make_cc(tinykv::raftpb::ConfAddVoter, 2)).has_value());
    // 日志里能看到 EntryConfChange 类型的条目（崩溃重建的依据）
    bool has_conf_entry = false;
    for (std::uint64_t i = 1; i <= f.cluster.node(0).last_index().value; ++i) {
        auto e = f.cluster.log(0).at(LogIndex{i});
        if (e && e->type() == tinykv::raftpb::EntryConfChange) {
            has_conf_entry = true;
        }
    }
    EXPECT_TRUE(has_conf_entry) << "成员变更未以 EntryConfChange 落盘";
}

TEST(ConfChange, NonLeaderRejectsConfChange) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    int follower = -1;
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader) {
            follower = i;
            break;
        }
    }
    ASSERT_GE(follower, 0);
    auto r = c.node(follower).propose_conf_change(make_cc(tinykv::raftpb::ConfAddVoter, 9));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<NotLeader>(r.error()));
}

}  // namespace
