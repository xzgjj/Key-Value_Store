// 文件用途：TCP 传输集成测试——真实 socket 上的分帧收发与三节点选主（FUNC-03/04 网络侧）。
// 归属：TinyKV-CPP（M2 RaftKV）。
// 使用场景理解：内存总线证明"状态机对"，真实 socket 证明"封装对"。这里盯三件事：
//   帧边界（多帧不会粘在一起）、跨线程投递（io 线程 → 收件箱 → 状态机）、
//   以及三节点在真 TCP 上仍能选出唯一 leader、复制条目并各自 apply。
//
// 两条网络测试的硬经验（都踩过）：
//   * tick 必须与自旋解耦：一次自旋约 0.1ms，而一次 TCP 往返要毫秒级；每自旋都 tick 会让
//     选举超时比投票往返还短，集群永远在重选；
//   * 不用 sleep 对齐：等消息靠自旋探测（yield），因为本项目把"sleep 对齐启动"列为违禁模式。

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "common/compat.h"
#include "raft_test_base.h"
#include "raftstore/peer.h"
#include "tcp_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
constexpr int kSpinPerTick = 300;  // 每 300 次自旋推进一拍（约 30ms/tick）

// 自旋等待条件成立（不 sleep；上限内未满足即判失败，避免挂死）
template <typename Pred>
bool wait_until(Pred pred, int max_spins = 100000) {
    for (int i = 0; i < max_spins; ++i) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// 三节点真实 TCP 集群夹具：起端口、建 Peer、泵消息，把样板从用例里挪出来
class TcpCluster {
public:
    void start() {
        (void)start_transports(kNodes, transports_);  // 端口自动分配并互相 set_peers

        std::vector<PeerId> voters;
        for (int i = 0; i < kNodes; ++i) {
            voters.push_back(PeerId{static_cast<std::uint64_t>(i) + 1});
        }
        for (int i = 0; i < kNodes; ++i) {
            dirs_.push_back(std::make_unique<TempDir>("tinykv_tcp_node"));
            auto engine = tinykv::Engine::open({.dir = dirs_.back()->path() / "db"});
            EXPECT_TRUE(engine.has_value());
            engines_.push_back(std::move(*engine));
            tinykv::raft::Config cfg{.id = voters[static_cast<std::size_t>(i)],
                                     .voters = voters,
                                     .election_timeout = 10,
                                     .heartbeat_interval = 3};
            auto peer = Peer::open(cfg, *engines_.back());
            EXPECT_TRUE(peer.has_value());
            peers_.push_back(std::move(*peer));
        }
    }

    // 收件 → 步进 → apply → 发件；advance_tick 时按自旋比例推进时钟
    void pump(bool advance_tick) {
        if (advance_tick && ++spin_ % kSpinPerTick == 0) {
            for (auto& p : peers_) {
                p->tick();
            }
        }
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            for (auto& msg : transports_[i]->poll()) {
                (void)peers_[i]->step_message(msg);
            }
            auto applied = peers_[i]->on_ready();
            if (!applied.has_value()) {
                std::string why = "?";
                if (const auto* inv = std::get_if<tinykv::InvalidArgument>(&applied.error())) {
                    why = inv->what;
                }
                ADD_FAILURE() << "节点 " << i << " apply 失败：" << why;
            }
            for (auto& out : peers_[i]->take_messages()) {
                (void)transports_[i]->send(PeerId{out.to()}, out);
            }
        }
    }

    [[nodiscard]] int leader_index() const {
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            if (peers_[i]->is_leader()) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] bool all_committed(std::uint64_t need) const {
        for (const auto& p : peers_) {
            if (p->raft().committed().value < need) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] Peer& peer(int i) { return *peers_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] tinykv::Engine& engine(int i) { return *engines_[static_cast<std::size_t>(i)]; }

private:
    std::vector<std::unique_ptr<TcpTransport>> transports_;
    std::vector<std::unique_ptr<TempDir>> dirs_;
    std::vector<std::unique_ptr<tinykv::Engine>> engines_;
    std::vector<std::unique_ptr<Peer>> peers_;
    int spin_{0};
};

TEST(TcpTransport, RoundTripBetweenTwoPeers) {
    auto a = TcpTransport::start(TcpOptions{.self = PeerId{1}, .listen_port = 0});
    auto b = TcpTransport::start(TcpOptions{.self = PeerId{2}, .listen_port = 0});
    ASSERT_TRUE(a.has_value()) << "A 启动失败（端口不可用？）";
    ASSERT_TRUE(b.has_value());
    ASSERT_GT((*a)->port(), 0);
    ASSERT_GT((*b)->port(), 0);

    (*a)->set_peers({{2, (*b)->port()}});
    (*b)->set_peers({{1, (*a)->port()}});

    tinykv::raftpb::Message msg;
    msg.set_id(tinykv::raftpb::MSG_HEARTBEAT);
    msg.set_term(7);
    msg.set_from(1);
    msg.set_to(2);
    msg.set_commit(3);
    ASSERT_TRUE((*a)->send(PeerId{2}, msg).has_value());

    auto received = std::make_shared<std::vector<tinykv::raftpb::Message>>();
    ASSERT_TRUE(wait_until([&] {
        for (auto& m : (*b)->poll()) {
            received->push_back(std::move(m));
        }
        return !received->empty();
    })) << "对端未在自旋上限内收到消息";

    ASSERT_EQ(received->size(), 1U);
    EXPECT_EQ((*received)[0].id(), tinykv::raftpb::MSG_HEARTBEAT);
    EXPECT_EQ((*received)[0].term(), 7U);
    EXPECT_EQ((*received)[0].commit(), 3U);
}

TEST(TcpTransport, MultipleFramesDoNotCoalesce) {
    auto a = TcpTransport::start(TcpOptions{.self = PeerId{1}, .listen_port = 0});
    auto b = TcpTransport::start(TcpOptions{.self = PeerId{2}, .listen_port = 0});
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    (*a)->set_peers({{2, (*b)->port()}});
    (*b)->set_peers({{1, (*a)->port()}});

    for (std::uint64_t i = 1; i <= 3; ++i) {
        tinykv::raftpb::Message msg;
        msg.set_id(tinykv::raftpb::MSG_HEARTBEAT);
        msg.set_term(i);
        msg.set_from(1);
        msg.set_to(2);
        ASSERT_TRUE((*a)->send(PeerId{2}, msg).has_value());
    }

    auto seen = std::make_shared<std::vector<std::uint64_t>>();
    ASSERT_TRUE(wait_until([&] {
        for (auto& m : (*b)->poll()) {
            seen->push_back(m.term());
        }
        return seen->size() >= 3;
    }));
    EXPECT_EQ(seen->size(), 3U);  // 三条各自成帧，没有被粘成一条
}

TEST(TcpCluster, ElectsLeaderAndReplicatesOverRealSockets) {
    TcpCluster cluster;
    cluster.start();
    ASSERT_EQ(cluster.leader_index(), -1);  // 起手无 leader

    ASSERT_TRUE(wait_until([&] {
        cluster.pump(true);
        return cluster.leader_index() >= 0;
    })) << "真 TCP 集群在自旋上限内未选出 leader";

    const int leader = cluster.leader_index();
    ASSERT_GE(leader, 0);

    cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(1);
    req.mutable_header()->set_sequence_id(1);
    auto* one = req.add_requests();  // 一条命令设全字段；调两次会造出"只有 value 的空 key"
    one->mutable_put()->set_key("net-key");
    one->mutable_put()->set_value("net-value");
    ASSERT_TRUE(cluster.peer(leader).propose(req).has_value());

    // 仍需低频 tick：follower 的 commit 位点靠 leader 后续的 AppendEntries/心跳带过去
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !cluster.all_committed(1)) {
        cluster.pump(true);
        std::this_thread::yield();
    }
    ASSERT_TRUE(cluster.all_committed(1)) << "条目未在 10s 内复制到全部节点";

    // 直接查各节点存储：apply 在每个节点都会发生，绕开"读必须找当前 leader"的约束，
    // 断言也更硬——三个节点的状态机都得有这条数据。
    const std::string key = "net-key";
    const auto key_bytes = tinykv::as_bytes(tinykv::span{key.data(), key.size()});
    for (int i = 0; i < kNodes; ++i) {
        auto got = cluster.engine(i).get(tinykv::Cf::Default, key_bytes);
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value()) << "节点 " << i << " 未 apply 该写入";
        EXPECT_EQ(*got->value, "net-value") << "节点 " << i;
    }
}

}  // namespace
