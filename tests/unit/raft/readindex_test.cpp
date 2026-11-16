// 文件用途：ReadIndex 与 LeaseRead——quorum 确认、分区下不可确认、租约有效期（FUNC-05/06）。
// 归属：TinyKV-CPP（M3 一致读）。
// 使用场景理解：ReadIndex 的安全前提是"多数派确认过这个位点"——只要确认过，任何后来当选者
// 的日志都必然包含它，读才不会读到回退的数据。所以三条断言缺一不可：
//   (a) leader 能拿到 quorum 确认；
//   (b) 分区中的 leader **拿不到**确认（否则就是脏读）；
//   (c) 租约默认关，开启后只在 quorum 心跳刷新后的窗口内有效。

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 选主 + 提交一条真实条目，保证 commit ≥ 1（read_index 为 0 时断言没有区分度）
void elect_and_commit(Cluster& c) {
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    ASSERT_GE(leader, 0);
    ASSERT_TRUE(c.node(leader).propose("warmup").has_value());
    c.flush();
    ASSERT_GE(c.log(leader).committed().value, 1U);
}

TEST(ReadIndex, QuorumConfirmsReadIndex) {
    Cluster c(3);
    elect_and_commit(c);
    const int leader = c.leader_index();
    const auto commit = c.log(leader).committed();

    ASSERT_TRUE(c.node(leader).read_index().has_value());
    // 手动泵一轮：不能走 Cluster::flush——它取 Ready 时只留 messages，会把 confirmed_reads 丢掉
    auto hb = c.node(leader).take_ready();
    for (const auto& m : hb.messages) {
        (void)c.node(static_cast<int>(m.to()) - 1).step(m);
    }
    std::vector<tinykv::raftpb::Message> replies;
    for (int i = 0; i < c.size(); ++i) {
        for (auto& m : c.node(i).take_ready().messages) {
            replies.push_back(m);
        }
    }
    for (const auto& m : replies) {
        (void)c.node(static_cast<int>(m.to()) - 1).step(m);
    }

    auto ready = c.node(leader).take_ready();
    ASSERT_FALSE(ready.confirmed_reads.empty()) << "多数派回执后仍未确认读位点";
    EXPECT_GE(ready.confirmed_reads.front(), commit.value)
        << "确认位点必须 ≥ 发起时的 commit（否则读可能回退）";
}

TEST(ReadIndex, PartitionedLeaderCannotConfirm) {
    Cluster c(3);
    elect_and_commit(c);
    const int leader = c.leader_index();

    // 全丢包：leader 仍在任期内，但拿不到任何回执
    ASSERT_TRUE(c.node(leader).read_index().has_value());
    std::mt19937 rng(7);
    for (int i = 0; i < 10; ++i) {
        c.tick_all();
        (void)c.deliver_round_dropping(100, rng);
    }
    auto ready = c.node(leader).take_ready();
    EXPECT_TRUE(ready.confirmed_reads.empty())
        << "分区中的 leader 竟然确认了读位点——这会直接导致脏读";
}

TEST(ReadIndex, LeaseDisabledByDefault) {
    Cluster c(3);
    elect_and_commit(c);
    EXPECT_FALSE(c.node(c.leader_index()).lease_valid()) << "LeaseRead 必须默认关（D2-4）";
}

TEST(ReadIndex, LeaseValidAfterQuorumHeartbeatThenExpires) {
    Cluster c(3, /*election_timeout=*/8, /*heartbeat=*/3);
    elect_and_commit(c);
    const int leader = c.leader_index();
    c.node(leader).enable_lease(true);

    for (int i = 0; i < 3; ++i) {   // 心跳间隔 3 拍：必须 tick 满一轮才会发出心跳
        c.tick_all();
    }
    c.flush();   // 心跳 + 回执 → 刷新租约
    EXPECT_TRUE(c.node(leader).lease_valid()) << "quorum 心跳后租约应有效";

    // 之后只推进时钟、不投递消息：租约应在 0.7×election_timeout 后失效
    for (int i = 0; i < 10; ++i) {
        c.node(leader).tick();
    }
    EXPECT_FALSE(c.node(leader).lease_valid()) << "租约过期后必须回到 ReadIndex 路径";
}

TEST(ReadIndex, NonLeaderRejectsReadIndex) {
    Cluster c(3);
    elect_and_commit(c);
    const int leader = c.leader_index();
    for (int i = 0; i < c.size(); ++i) {
        if (i == leader) {
            continue;
        }
        auto r = c.node(i).read_index();
        ASSERT_FALSE(r.has_value());
        EXPECT_TRUE(std::holds_alternative<tinykv::NotLeader>(r.error()));
    }
}

}  // namespace
