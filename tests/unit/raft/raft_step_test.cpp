// 文件用途：Raft 状态机单测——选举收敛、复制、四条红线（SPEC-2 §2.5、§2.6）。
// 归属：TinyKV-CPP（M2 Raft 共识）。
// 使用场景理解：这些用例直接对着 SPEC-2 的红线写，每条红线至少一个断言。
// 走的是"构造消息序列 → 断言状态"，不碰网络，失败时能一眼看出是哪条规则被违反。

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::testlib::make_entry;
using tinykv::testlib::make_msg;

TEST(RaftStep, ElectionConvergesToSingleLeaderOnThreeNodes) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    EXPECT_TRUE(c.terms_agree());
    EXPECT_GE(c.node(c.leader_index()).term().value, 1U);
}

TEST(RaftStep, ElectionConvergesOnFiveNodes) {
    Cluster c(5);
    ASSERT_TRUE(c.elect());
    EXPECT_TRUE(c.terms_agree());
}

TEST(RaftStep, LeaderProposalReplicatesToAllFollowers) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);

    ASSERT_TRUE(c.node(leader).propose("cmd-1").has_value());
    c.flush();

    for (int i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c.log(i).last_index(), LogIndex{1}) << "节点 " << i << " 未复制到该条目";
    }
    EXPECT_GE(c.log(leader).committed().value, 1U);
}

TEST(RaftStep, NonLeaderProposalRejected) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    for (int i = 0; i < c.size(); ++i) {
        if (i == leader) {
            continue;
        }
        auto r = c.node(i).propose("nope");
        ASSERT_FALSE(r.has_value());
        EXPECT_TRUE(std::holds_alternative<tinykv::NotLeader>(r.error()));
    }
}

TEST(RaftStep, RejectVoteDoesNotResetElectionTimer) {
    // 红线 1：拒票不得重置自己的选举计时器（重置会让集群反复打散，白皮书实测）
    Cluster c(2);
    // 统一推到任期 5，清掉 voted_for
    for (int i = 0; i < c.size(); ++i) {
        (void)c.node(i).step(
            make_msg(tinykv::raftpb::MSG_HEARTBEAT, 5, 2, static_cast<std::uint64_t>(i) + 1));
    }
    c.flush();
    // 让节点 1 的日志比候选者新：候选者 index=0 的日志必然过不了"日志足够新"判定
    ASSERT_TRUE(c.log(1).append(std::vector<Entry>{make_entry(1, 1, "newer")}).has_value());

    for (int i = 0; i < 3; ++i) {
        c.tick_one(1);
    }
    const auto before = c.node(1).election_elapsed();
    ASSERT_GT(before, 0U);

    auto vote_req = make_msg(tinykv::raftpb::MSG_REQUEST_VOTE, 5, 2, 1);
    vote_req.set_index(0);       // 候选者日志更旧
    vote_req.set_log_term(0);
    (void)c.node(1).step(vote_req);

    EXPECT_EQ(c.node(1).election_elapsed(), before);   // 拒绝后计时器一动不动
}

TEST(RaftStep, OldTermEntryDoesNotCommitByMajorityAlone) {
    // 红线 4：只有当前 term 的条目被多数派复制才推进 commit
    Cluster c(3);
    // 把三个节点推到任期 5
    for (int i = 0; i < c.size(); ++i) {
        (void)c.node(i).step(
            make_msg(tinykv::raftpb::MSG_HEARTBEAT, 5, 1, static_cast<std::uint64_t>(i) + 1));
    }
    c.flush();
    // 三份日志各写入一条 term=1 的旧条目（属于前任期的尾巴）
    for (int i = 0; i < c.size(); ++i) {
        ASSERT_TRUE(c.log(i).append(std::vector<Entry>{make_entry(1, 1, "old-term")}).has_value());
    }

    ASSERT_TRUE(c.elect());
    c.flush();
    // 旧 term 条目已被多数派"复制"（三份都有），但 commit 不得因此推进
    EXPECT_EQ(c.log(c.leader_index()).committed(), LogIndex{0});

    // 一旦当前 term 有新条目提交，旧条目随之被间接提交（Raft 的正确语义）
    ASSERT_TRUE(c.node(c.leader_index()).propose("new-term").has_value());
    c.flush();
    EXPECT_GE(c.log(c.leader_index()).committed().value, 2U);
}

TEST(RaftStep, CompactedPrevIndexRepliesCommittedInsteadOfReject) {
    // 红线 3：follower 对已压缩的 prevLogIndex 回 committed，而不是 reject
    Cluster c(2);
    for (int i = 0; i < c.size(); ++i) {
        (void)c.node(i).step(
            make_msg(tinykv::raftpb::MSG_HEARTBEAT, 3, 1, static_cast<std::uint64_t>(i) + 1));
    }
    c.flush();
    // 节点 1：写入 5 条并提交到 5，再压缩到 3
    std::vector<Entry> entries;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        entries.push_back(make_entry(i, 3, "e"));
    }
    ASSERT_TRUE(c.log(1).append(entries).has_value());
    ASSERT_TRUE(c.log(1).set_committed(LogIndex{5}).has_value());
    // 压缩红线收紧到 applied（M7 ②）：先推进 applied 再压缩，否则会被拒绝
    ASSERT_TRUE(c.log(1).set_applied(LogIndex{5}).has_value());
    ASSERT_TRUE(c.log(1).compact(LogIndex{3}).has_value());

    // leader 发来 prev=1（已被压缩）的 AppendEntries
    auto append = make_msg(tinykv::raftpb::MSG_APPEND, 3, 2, 1);
    append.set_index(1);
    append.set_log_term(3);
    append.set_commit(5);
    (void)c.node(1).step(append);

    auto ready = c.node(1).take_ready();
    ASSERT_FALSE(ready.messages.empty());
    const auto& reply = ready.messages.front();
    EXPECT_FALSE(reply.reject());
    EXPECT_GE(reply.index(), 5U);   // 回 committed 而非 reject
}

TEST(RaftStep, HeartbeatCarriesNoEntries) {
    // 红线 6：心跳只带元数据（PERF-08 的直接约束）
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    // 清空残留消息后推进到下一次心跳
    c.flush();
    for (std::uint64_t i = 0; i < 3; ++i) {
        c.node(leader).tick();
    }
    auto ready = c.node(leader).take_ready();
    bool saw_heartbeat = false;
    for (const auto& m : ready.messages) {
        if (m.id() == tinykv::raftpb::MSG_HEARTBEAT) {
            saw_heartbeat = true;
            EXPECT_EQ(m.entries_size(), 0) << "心跳携带了日志条目";
        }
    }
    EXPECT_TRUE(saw_heartbeat);
}

}  // namespace
