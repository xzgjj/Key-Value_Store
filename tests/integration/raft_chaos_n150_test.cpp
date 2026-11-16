// 文件用途：150 轮随机注入长跑——每轮独立种子，丢包 + 提案 + 重启后必须收敛（ENG-05）。
// 归属：TinyKV-CPP（M2 Raft 共识，integration/nightly）。
//
// 与 sim_chaos_test 的分工：那个是 PR 必跑的秒级冒烟（5 种子），本文件是夜跑的 150 轮纪律
// （每轮独立种子、独立集群、独立数据目录）。
// **已知偏差**：重启用"关闭并重建 Peer/Engine"模拟，而不是进程级 kill -9。前者验证
// "内存状态丢弃后从落盘状态恢复"，后者还验证"写到一半被强杀"。真正的 kill -9 需外部
// 进程编排，属 M2 剩余项（见 docs/notes-m2.md §2）。

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <vector>

#include "raft_test_base.h"
#include "raftstore/peer.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

constexpr int kRounds = 150;

// 收敛判据集中一处：唯一 leader + 任期一致 + 已提交位点一致 + 已提交前缀同 term。
// 注意只看**已提交**部分——未提交尾部各节点不同是 Raft 的正常语义。
bool converged(Cluster& c) {
    if (c.leader_count() != 1 || !c.terms_agree()) {
        return false;
    }
    const auto committed = c.log(c.leader_index()).committed();
    for (int i = 0; i < c.size(); ++i) {
        if (c.log(i).committed() != committed) {
            return false;
        }
    }
    for (std::uint64_t idx = 1; idx <= committed.value; ++idx) {
        const auto base = c.log(0).term(tinykv::LogIndex{idx});
        for (int i = 1; i < c.size(); ++i) {
            const auto other = c.log(i).term(tinykv::LogIndex{idx});
            if (base.has_value() && other.has_value() && *base != *other) {
                return false;
            }
        }
    }
    return true;
}

bool run_round(unsigned seed) {
    Cluster c(3);
    if (!c.elect()) {
        return false;
    }
    std::mt19937 rng(seed);
    std::uniform_int_distribution<unsigned> drop(15, 45);

    for (int i = 0; i < 40; ++i) {
        c.tick_all();
        (void)c.deliver_round_dropping(drop(rng), rng);
        const int leader = c.leader_index();
        if (leader >= 0 && i % 4 == 0) {
            (void)c.node(leader).propose("k" + std::to_string(seed) + "-" + std::to_string(i));
        }
    }

    if (!c.elect()) {
        return false;   // 修好网络后必须重新收敛
    }
    for (int k = 0; k < 30; ++k) {
        c.tick_all();
        c.flush();
    }
    return converged(c);
}

// 单副本 Peer：建好后 tick 到当选（两处用例共用，避免复制粘贴）
std::unique_ptr<tinykv::raftstore::Peer> open_single_peer(tinykv::Engine& engine) {
    tinykv::raft::Config cfg{.id = PeerId{1}, .voters = {PeerId{1}},
                             .election_timeout = 4, .heartbeat_interval = 2};
    auto peer = tinykv::raftstore::Peer::open(cfg, engine);
    if (!peer) {
        return nullptr;
    }
    for (int i = 0; i < 20 && !(*peer)->is_leader(); ++i) {
        (*peer)->tick();
    }
    return std::move(*peer);
}

TEST(RaftChaosN150, ConvergesAcrossHundredFiftySeeds) {
    std::vector<unsigned> failed;
    for (int round = 0; round < kRounds; ++round) {
        const unsigned seed = 1000U + static_cast<unsigned>(round);
        if (!run_round(seed)) {
            failed.push_back(seed);
        }
    }
    ASSERT_TRUE(failed.empty()) << "失败种子数 " << failed.size() << "，首个失败种子 "
                                << (failed.empty() ? 0U : failed.front());
}

TEST(RaftChaosN150, CommittedDataSurvivesRestart) {
    TempDir tmp("tinykv_n150_restart");
    const auto dir = tmp.path() / "db";
    const std::string key = "durable-key";
    {
        auto engine = tinykv::Engine::open({.dir = dir});
        ASSERT_TRUE(engine.has_value());
        auto peer = open_single_peer(**engine);
        ASSERT_NE(peer, nullptr);
        ASSERT_TRUE(peer->is_leader());
        ASSERT_TRUE(peer->propose(make_put_request(1, 1, key, "v")).has_value());
        ASSERT_TRUE(peer->on_ready().has_value());
        ASSERT_TRUE((*engine)->close().has_value());
    }
    auto engine = tinykv::Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    auto peer = open_single_peer(**engine);
    ASSERT_NE(peer, nullptr);
    auto got = peer->get(tinykv::Cf::Default, key);
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value()) << "重启后已提交数据丢失";
    EXPECT_EQ(*got->value, "v");
}

}  // namespace
