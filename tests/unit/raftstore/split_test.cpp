// 文件用途：Split 端到端——admin 提案走共识、apply 切分元数据与路由（M4 第③层，FUNC-08）。
// 归属：TinyKV-CPP（M4 Multi-Raft Split）。
// 使用场景理解：这是 SPEC-3 红线1/2 的证据物——split_key 必须在开区间、apply 后立即更新路由，
// 否则客户端会命中"no region"路由空洞。用单副本装置先证明提案→apply→路由更新的全链路。

#include "raftstore/peer.h"
#include "raftstore/region.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "raft_test_base.h"
#include "temp_dir.h"

namespace {

using namespace tinykv;   // NOLINT(google-build-using-namespace)
using tinykv::raftstore::Peer;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;
using tinykv::testlib::TempDir;

cmdpb::RaftCmdRequest make_split(std::uint64_t client, std::uint64_t seq,
                                 const std::string& key, std::uint64_t new_region_id) {
    cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    auto* admin = req.mutable_admin();
    admin->mutable_split()->set_split_key(key);
    admin->mutable_split()->set_new_region_id(new_region_id);
    return req;
}

struct SplitPeer {
    TempDir tmp{"tinykv_split"};
    std::unique_ptr<Engine> engine;
    std::unique_ptr<Peer> peer;
    std::unique_ptr<StoreMeta> meta;

    explicit SplitPeer(std::string start = "", std::string end = "") {
        auto opened = Engine::open({.dir = tmp.path() / "db"});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
        const std::vector<PeerId> members{PeerId{1}};
        raft::Config cfg{.id = PeerId{1}, .voters = members, .election_timeout = 4,
                         .heartbeat_interval = 2};
        auto p = Peer::open(cfg, *engine);
        EXPECT_TRUE(p.has_value());
        peer = std::move(*p);
        meta = std::make_unique<StoreMeta>();
        Region region{.id = RegionId{1}, .start_key = std::move(start), .end_key = std::move(end),
                      .peers = {}, .epoch = {1, 0}, .size_hint = 0};
        EXPECT_TRUE(meta->add_region(region).has_value());
        peer->set_region(region);
        peer->set_store_meta(meta.get());
        for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
            peer->tick();
        }
    }
};

TEST(Split, AdminProposalCutsRegionAndUpdatesRouter) {
    SplitPeer f;
    ASSERT_TRUE(f.peer->is_leader());
    ASSERT_TRUE(f.peer->propose(make_split(7, 1, "m", 2)).has_value());
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 1U);

    // 路由：左半与右半各归一个 region，边界 m 归右
    auto a = f.meta->find_region("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, RegionId{1});
    EXPECT_EQ(a->end_key, "m");
    auto m = f.meta->find_region("m");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->id, RegionId{2});
    EXPECT_EQ(m->start_key, "m");
    EXPECT_EQ(f.meta->region_count(), 2U);
}

TEST(Split, InvalidSplitKeyIsRejectedAtApply) {
    SplitPeer f("", "z");   // [,"z")
    // split_key 在边界外 → apply 阶段拒绝（红线1）
    ASSERT_TRUE(f.peer->propose(make_split(7, 1, "zz", 2)).has_value());
    auto applied = f.peer->on_ready();
    // apply 返回错误（split 被拒），region 不增
    EXPECT_FALSE(applied.has_value());
    EXPECT_EQ(f.meta->region_count(), 1U);
}

TEST(Split, DataWrittenBeforeSplitRemainsInLeftRegion) {
    SplitPeer f;
    // 先在 [,"") 写一条
    cmdpb::RaftCmdRequest put;
    put.mutable_header()->set_client_id(7);
    put.mutable_header()->set_sequence_id(1);
    auto* put_req = put.add_requests()->mutable_put();
    put_req->set_key("k");
    put_req->set_value("v");
    ASSERT_TRUE(f.peer->propose(put).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());

    // split at "m"：k < m 归左 region
    ASSERT_TRUE(f.peer->propose(make_split(7, 2, "m", 2)).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());

    auto got = f.peer->get(Cf::Default, "k");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "v");
}

}  // namespace
