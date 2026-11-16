// 文件用途：RaftKV 冒烟——提案走共识、apply 落存储、重试幂等（SPEC-2 §2.7.1 FUNC-04）。
// 归属：TinyKV-CPP（M2 RaftKV）。
// 使用场景理解：这一层是"写走共识"的落点。最重要的断言不是"能写进去"，
// 而是"同一 (client, seq) 重试不会二次 apply"——真实故障里客户端重试是常态，
// 若重试被当成新写，删了的数据会被重新写回，且这类问题只在网络抖动时出现。

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "raft_test_base.h"
#include "raftstore/peer.h"

namespace {

namespace cmdpb = tinykv::cmdpb;
using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::raftstore::Peer;

cmdpb::RaftCmdRequest make_delete(std::uint64_t client, std::uint64_t seq,
                                  const std::string& key) {
    cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    req.add_requests()->mutable_delete_()->set_key(key);
    return req;
}

// 单副本集群：自己一票即多数派，便于在进程内验证提案→apply 全链路
struct SinglePeer {
    TempDir tmp{"tinykv_peer"};
    std::unique_ptr<tinykv::Engine> engine;
    std::unique_ptr<Peer> peer;

    explicit SinglePeer(int voters = 1) {
        auto opened = tinykv::Engine::open({.dir = tmp.path() / "db"});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
        std::vector<PeerId> members;
        for (int i = 0; i < voters; ++i) {
            members.push_back(PeerId{static_cast<std::uint64_t>(i) + 1});
        }
        tinykv::raft::Config cfg{.id = PeerId{1},
                                 .voters = members,
                                 .election_timeout = 4,
                                 .heartbeat_interval = 2};
        auto p = Peer::open(cfg, *engine);
        EXPECT_TRUE(p.has_value());
        peer = std::move(*p);
        for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
            peer->tick();
        }
    }
};

TEST(RaftKv, SingleVoterBecomesLeaderOnFirstTick) {
    SinglePeer f;
    EXPECT_TRUE(f.peer->is_leader());
}

TEST(RaftKv, PutGoesThroughLogThenApplies) {
    SinglePeer f;
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 1, "k1", "v1")).has_value());
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 1U);

    auto got = f.peer->get(tinykv::Cf::Default, "k1");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "v1");
}

TEST(RaftKv, DuplicateSequenceIsNotReapplied) {
    SinglePeer f;
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 1, "k1", "v1")).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());

    // 同 client 同 seq 的重试：不得产生新提案，也不得覆盖已提交的值
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 1, "k1", "v2")).has_value());
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 0U);

    auto got = f.peer->get(tinykv::Cf::Default, "k1");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "v1");
}

TEST(RaftKv, DeleteAppliesAndLeavesNoValue) {
    SinglePeer f;
    ASSERT_TRUE(f.peer->propose(make_put_request(9, 1, "k2", "v2")).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());
    ASSERT_TRUE(f.peer->propose(make_delete(9, 2, "k2")).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());

    auto got = f.peer->get(tinykv::Cf::Default, "k2");
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->value.has_value());
}

TEST(RaftKv, NonLeaderRejectsReadAndProposal) {
    // 3 副本但只有本节点在跑 → 拿不到多数派，永远选不出 leader
    SinglePeer f(3);
    EXPECT_FALSE(f.peer->is_leader());

    auto got = f.peer->get(tinykv::Cf::Default, "k");
    ASSERT_FALSE(got.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::NotLeader>(got.error()));

    auto r = f.peer->propose(make_put_request(1, 1, "k", "v"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::NotLeader>(r.error()));
}

TEST(RaftKv, ReadIndexCommandInLogIsRejected) {
    // ReadIndex 标记不应进日志（它只是读协调器）；Get 可以进日志——那是 log 模式读
    SinglePeer f;
    cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(3);
    req.mutable_header()->set_sequence_id(1);
    req.add_requests()->mutable_read_index();
    ASSERT_TRUE(f.peer->propose(req).has_value());

    auto applied = f.peer->on_ready();
    ASSERT_FALSE(applied.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(applied.error()));
}

}  // namespace
