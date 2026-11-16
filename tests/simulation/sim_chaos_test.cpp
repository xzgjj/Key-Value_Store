// 文件用途：随机注入冒烟——丢包下仍能收敛到唯一 leader 且日志一致（ENG-05 的轻量版）。
// 归属：TinyKV-CPP（M2 Raft 共识，simulation 层）。
//
// 与 150 轮夜跑的关系：这里是**每轮几十个 tick 的快速版**，进 PR 必跑；
// 150 轮长跑要真实网络与进程级 kill -9，属 M2 剩余的集成台工作（见 notes-m2 §7）。
// 快速版的价值：把"丢包下不收敛"这类结构性错误在秒级暴露，而不是等夜跑。

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 丢包 → 修好 → 必须重新收敛：这是 Raft 的活性（liveness）断言
TEST(SimChaos, ConvergesAfterPacketLoss) {
    for (unsigned seed : {1U, 7U, 42U, 99U, 1234U}) {
        Cluster c(3);
        ASSERT_TRUE(c.elect()) << "seed=" << seed;
        std::mt19937 rng(seed);

        // 注入：30% 丢包 + 随机 tick + leader 提案，持续 60 轮
        for (int i = 0; i < 60; ++i) {
            c.tick_all();
            (void)c.deliver_round_dropping(30, rng);
            const int leader = c.leader_index();
            if (leader >= 0 && i % 5 == 0) {
                (void)c.node(leader).propose("chaos-" + std::to_string(seed) + "-" + std::to_string(i));
            }
        }

        // 修好网络：必须重新选出唯一 leader 且任期收敛
        ASSERT_TRUE(c.elect()) << "seed=" << seed << " 丢包恢复后未收敛";
        EXPECT_EQ(c.leader_count(), 1) << "seed=" << seed;
        EXPECT_TRUE(c.terms_agree()) << "seed=" << seed;

        // 让新 leader 把尾巴复制出去：elect() 一选出即返回，此时 follower 可能还落后
        for (int k = 0; k < 30; ++k) {
            c.tick_all();
            c.flush();
        }

        // 一致性断言只看**已提交**部分：未提交的尾部条目允许各节点不同（这正是 Raft 的语义）
        const auto committed = c.log(c.leader_index()).committed();
        for (int i = 0; i < c.size(); ++i) {
            EXPECT_EQ(c.log(i).committed(), committed)
                << "seed=" << seed << " 节点 " << i << " committed 不一致";
        }
        // 已提交前缀逐条比对：term 必须完全一致（同一 index 同一 term）
        for (std::uint64_t idx = 1; idx <= committed.value; ++idx) {
            const auto base = c.log(0).term(tinykv::LogIndex{idx});
            for (int i = 1; i < c.size(); ++i) {
                const auto other = c.log(i).term(tinykv::LogIndex{idx});
                if (base.has_value() && other.has_value()) {
                    EXPECT_EQ(*other, *base) << "seed=" << seed << " index " << idx << " term 不一致";
                }
            }
        }
    }
}

}  // namespace
