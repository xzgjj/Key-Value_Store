// 文件用途：leadership 转移的集群级模拟（M4 第⑦a层，FUNC-10，SPEC-3 §3.7 sim_transfer_test）。
// 归属：TinyKV-CPP（M4 集成层）。
// 使用场景理解：单测证明的是"机制对不对"（发不发 TimeoutNow、到不到点放弃），
// 这里证明的是"业务结果对不对"——转移过程不能让已提交的提案消失，也不能让客户端
// 以为写成功其实没写。三条边界对应 SPEC-3 FUNC-10 一句话：
//   (a) 转移不丢提案：转移前已提交的条目，在新 leader 上必须仍然在、且仍已提交；
//   (b) 转移期被拒的提案有明确错误码（可重试），而不是静默丢弃；
//   (c) 转移失败放弃后，原 leader 继续服务（写入仍能提交），不是"卡住等死"。

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::InvalidArgument;
using tinykv::PeerId;

// 选一个不是 leader 的 voter 作为转移目标
std::uint64_t pick_target(Cluster& c, int leader_idx) {
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader_idx) {
            return static_cast<std::uint64_t>(i) + 1;
        }
    }
    return 0;
}

// 转移前已提交的条目，转移后在新 leader 上依然存在且已提交（不丢提案）
TEST(SimTransfer, NoCommittedProposalLostAcrossTransfer) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int old_leader = c.leader_index();
    ASSERT_GE(old_leader, 0);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(c.node(old_leader).propose("k" + std::to_string(i)).has_value());
        c.flush();
    }
    const std::uint64_t committed_before = c.log(old_leader).committed().value;
    ASSERT_GE(committed_before, 5U) << "前提：5 条提案都应已提交";
    const std::uint64_t last_before = c.log(old_leader).last_index().value;

    const std::uint64_t target = pick_target(c, old_leader);
    ASSERT_TRUE(c.node(old_leader).transfer_leader(PeerId{target}).has_value());
    c.flush();

    const int new_leader = c.leader_index();
    ASSERT_EQ(new_leader, static_cast<int>(target) - 1) << "转移目标应成为新 leader";
    // 新 leader（term+1）按红线4只能提交自己任期的条目：补一条后旧条目随之重新提交
    ASSERT_TRUE(c.node(new_leader).propose("after-transfer").has_value());
    c.flush();

    // 不丢：旧 committed 位点全部恢复，日志末尾不缩短
    EXPECT_GE(c.log(new_leader).committed().value, committed_before)
        << "转移后旧提案必须仍然已提交（一条都不能少）";
    EXPECT_GE(c.log(new_leader).last_index().value, last_before);
    // 全集群日志在末尾对齐（转移后新 leader 补一轮 append）
    for (int i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c.log(i).last_index().value, c.log(new_leader).last_index().value)
            << "转移后各副本日志长度应收敛到一致";
    }
}

// 转移期被拒的提案拿到明确错误（可重试），不是静默丢弃
TEST(SimTransfer, RejectedProposalHasExplicitError) {
    Cluster c(3);
    ASSERT_TRUE(c.elect());
    const int old_leader = c.leader_index();
    const std::uint64_t target = pick_target(c, old_leader);

    ASSERT_TRUE(c.node(old_leader).transfer_leader(PeerId{target}).has_value());
    auto during = c.node(old_leader).propose("during");
    ASSERT_FALSE(during.has_value()) << "转移期必须拒绝提案，不能静默接收后丢失";
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(during.error()))
        << "拒绝理由须可判定（InvalidArgument 表示可重试）";

    c.flush();
    // 重试落在新 leader 上：同一份载荷可以成功提交（证明"拒绝"不等于"丢失"）
    const int new_leader = c.leader_index();
    ASSERT_GE(new_leader, 0);
    ASSERT_TRUE(c.node(new_leader).propose("during").has_value());
    c.flush();
    EXPECT_GE(c.log(new_leader).committed().value, 1U);
}

// 转移放弃后原 leader 继续服务（能继续提交，不是卡死）
TEST(SimTransfer, AbandonedTransferKeepsLeaderServing) {
    Cluster c(3, /*election_timeout=*/8, /*heartbeat=*/3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader_index();
    const std::uint64_t target = pick_target(c, leader);

    ASSERT_TRUE(c.node(leader).transfer_leader(PeerId{target}).has_value());
    // 模拟"到目标的网络分区"：立即丢弃 leader 的 outbox，TimeoutNow 永远送不到，
    // 转移注定完不成（否则 flush 时 TimeoutNow 仍会送达并触发目标竞选）
    auto dropped = c.node(leader).take_ready();   // (void)：只丢弃，内容无关
    for (int i = 0; i < 8; ++i) {
        c.tick_one(leader);
    }
    ASSERT_FALSE(c.node(leader).is_transferring()) << "一个 election timeout 后应放弃";

    // 放弃后恢复服务：新提案能提交，且多数派仍可达
    ASSERT_TRUE(c.node(leader).propose("after-abandon").has_value());
    c.flush();
    EXPECT_GE(c.log(leader).committed().value, 1U);
    EXPECT_TRUE(c.terms_agree());
}

}  // namespace
