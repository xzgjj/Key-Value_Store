// 文件用途：单步成员变更模拟用例（M4 ④b，FUNC-09/REL-04，SPEC-3 红线5 + D3-5）。
// 归属：TinyKV-CPP（M4 Multi-Raft 成员变更追平门槛与移除边界）。
// 使用场景理解：成员变更以"apply 时才生效"为前提，因此引入两道边界——
//   (a) Learner 未追平（日志差 >=100 或未持续 5s）时 AddVoter 必须被拒，防止把落后节点提升为 voter
//       拉低可用性；追平后才允许提升。
//   (b) 两节点集群移除 leader 自己会让剩余单节点无法形成多数派、写永久卡死（SPEC-3 红线5），
//       在 TransferLeader 落地前（拆到第⑦层）必须先守卫拒绝。
//   (c) 三节点移除 follower 不受该守卫影响，仍须放行（防止守卫过宽）。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::PeerId;
using tinykv::raftpb::ConfAddLearner;
using tinykv::raftpb::ConfAddVoter;
using tinykv::raftpb::ConfRemoveNode;
using tinykv::raftpb::ConfChange;
using tinykv::raftpb::ConfChangeType;

// 构造单步成员变更提案（本文件局部使用，不污染共享 testlib 基建）
ConfChange make_cc(ConfChangeType type, std::uint64_t node_id) {
    ConfChange cc;
    cc.set_type(type);
    cc.set_node_id(node_id);
    return cc;
}

bool has_peer(const std::vector<PeerId>& v, std::uint64_t id) {
    return std::find(v.begin(), v.end(), PeerId{id}) != v.end();
}

// 追加普通日志并 flush，制造 leader 与未复制 learner 之间的日志差
void append_entries(Cluster& c, int leader, int n) {
    for (int i = 0; i < n; ++i) {
        (void)c.node(leader).propose("entry");
    }
    c.flush();
}

// 红用例：learner 日志差 >=100 时 AddVoter 必须被拒。
// 当前实现（④a）无门槛 → AddVoter 直接成功 → EXPECT_FALSE 失败（红）。
TEST(SimConfChange, AddVoterRejectedBeforeLearnerCatchUp) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);

    // 加新 learner（id=4）：无对应实例、无任何日志，match=0
    ASSERT_TRUE(c.node(leader).propose_conf_change(make_cc(ConfAddLearner, 4)).has_value());
    c.flush();
    ASSERT_TRUE(has_peer(c.node(leader).learners(), 4));

    append_entries(c, leader, 120);   // 日志差 120 > 100

    auto r = c.node(leader).propose_conf_change(make_cc(ConfAddVoter, 4));
    EXPECT_FALSE(r.has_value()) << "日志差 >=100 时 AddVoter 应被拒";
    EXPECT_FALSE(has_peer(c.node(leader).voters(), 4));
}

// 红用例：两节点集群移除 leader 自己必须被拒（红线5）。
// 当前实现（④a）无守卫 → 提案成功 → EXPECT_FALSE 失败（红）。
TEST(SimConfChange, RemoveLeaderRejectedInTwoNodeCluster) {
    Cluster c(2);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);
    const auto self_id = c.node(leader).voters().at(0).value;

    auto r = c.node(leader).propose_conf_change(make_cc(ConfRemoveNode, self_id));
    EXPECT_FALSE(r.has_value()) << "两节点集群移除 leader 自己应被守卫拒绝";
}

// 回归用例：三节点集群移除一个节点仍应成功（守卫只针对"移除自己且两节点"）。
TEST(SimConfChange, RemoveNodeAllowedInThreeNodeCluster) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);
    const auto victim = c.node(leader).voters().at(0).value;

    auto r = c.node(leader).propose_conf_change(make_cc(ConfRemoveNode, victim));
    EXPECT_TRUE(r.has_value()) << "三节点集群移除一个节点应被允许";
}

}  // namespace
