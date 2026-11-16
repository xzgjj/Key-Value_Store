// 文件用途：leadership 转移用例（M4 第⑦a层，红线6，SPEC-3 §3.7 transfer_leader_test）。
// 归属：TinyKV-CPP（M4 集成层）。
// 使用场景理解：转移不是"改个字段"，而是一次"让位 + 立即竞选"的协商。必须证明四件事：
// (a) 目标追平后送 TimeoutNow，目标真的当选、原 leader 降级；
// (b) 转移期拒新提案（否则转移会带着未提交条目走，结果不可预期）；
// (c) 一个 election timeout 没成即放弃，且放弃后原 leader 仍稳（不会自己把自己搞下台）；
// (d) 非法目标（自己/learner/不存在）与非 leader 发起都被明确拒绝。

#include <gtest/gtest.h>

#include <variant>

#include "raft_cluster.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::InvalidArgument;
using tinykv::NotLeader;
using tinykv::PeerId;

// 选一个不是 leader 的 voter 作为转移目标（返回 peer id）
std::uint64_t pick_target(Cluster& c, int leader_idx) {
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader_idx) {
            return static_cast<std::uint64_t>(i) + 1;   // make_voters: id = index+1
        }
    }
    return 0;
}

// 转移成功：TimeoutNow 让目标立即竞选，原 leader 投票并降级
TEST(TransferLeader, MovesLeadershipToTarget) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);

    // 先写几条，让转移目标需要"追平后才被通知"这条路径也走一遍
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(c.node(leader).propose("sync-" + std::to_string(i)).has_value());
        c.flush();
    }

    const std::uint64_t target = pick_target(c, leader);
    ASSERT_NE(target, 0U);
    const auto term_before = c.node(leader).term();

    ASSERT_TRUE(c.node(leader).transfer_leader(PeerId{target}).has_value());
    EXPECT_TRUE(c.node(leader).is_transferring());
    EXPECT_EQ(c.node(leader).transfer_target()->value, target);

    c.flush();

    EXPECT_TRUE(c.node(static_cast<int>(target) - 1).is_leader()) << "目标应成为新 leader";
    EXPECT_FALSE(c.node(leader).is_leader()) << "原 leader 应降级为 follower";
    EXPECT_GT(c.node(static_cast<int>(target) - 1).term().value, term_before.value)
        << "转移必然伴随一次 term+1 的选举";
    EXPECT_FALSE(c.node(leader).is_transferring()) << "转移完成后意图应清除";
    EXPECT_TRUE(c.terms_agree());
}

// 转移期拒提案：期间返回 InvalidArgument，转移完成后原 leader 已非 leader
TEST(TransferLeader, RejectsProposalsDuringTransfer) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    const std::uint64_t target = pick_target(c, leader);

    ASSERT_TRUE(c.node(leader).transfer_leader(PeerId{target}).has_value());
    auto r = c.node(leader).propose("during-transfer");
    ASSERT_FALSE(r.has_value()) << "转移期必须拒绝新提案";
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(r.error()));

    c.flush();
    // 转移完成后原 leader 已降级，再提案得到 NotLeader（而不是"假成功"）
    auto after = c.node(leader).propose("after-transfer");
    ASSERT_FALSE(after.has_value());
    EXPECT_TRUE(std::holds_alternative<NotLeader>(after.error()));
}

// 一个 election timeout 未完成即放弃：不投递消息（目标永不竞选），tick 到点清除意图
TEST(TransferLeader, AbandonsAfterOneElectionTimeout) {
    Cluster c(3, /*election_timeout=*/8, /*heartbeat=*/3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    const std::uint64_t target = pick_target(c, leader);

    ASSERT_TRUE(c.node(leader).transfer_leader(PeerId{target}).has_value());
    EXPECT_TRUE(c.node(leader).is_transferring());

    // 不 flush：TimeoutNow 永远送不到，目标不会竞选
    for (int i = 0; i < 7; ++i) {
        c.tick_one(leader);
    }
    EXPECT_TRUE(c.node(leader).is_transferring()) << "未到 deadline 前不该放弃";

    c.tick_one(leader);   // 第 8 拍：到达 transfer_deadline
    EXPECT_FALSE(c.node(leader).is_transferring()) << "一个 election timeout 后必须放弃";
    EXPECT_TRUE(c.node(leader).is_leader()) << "放弃后原 leader 仍应在位（不能自己下台）";
    EXPECT_TRUE(c.node(leader).propose("after-abandon").has_value())
        << "放弃后应恢复接受提案";
}

// 非法目标与非 leader 发起：明确拒绝而非静默
TEST(TransferLeader, RejectsInvalidTarget) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    const int other = (leader + 1) % 3;

    // 目标是自己
    auto self = c.node(leader).transfer_leader(PeerId{static_cast<std::uint64_t>(leader) + 1});
    ASSERT_FALSE(self.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(self.error()));

    // 目标不在集群里（不是 voter）
    auto ghost = c.node(leader).transfer_leader(PeerId{99});
    ASSERT_FALSE(ghost.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(ghost.error()));

    // follower 发起转移 → NotLeader
    auto not_leader = c.node(other).transfer_leader(PeerId{static_cast<std::uint64_t>(leader) + 1});
    ASSERT_FALSE(not_leader.has_value());
    EXPECT_TRUE(std::holds_alternative<NotLeader>(not_leader.error()));
}

}  // namespace
