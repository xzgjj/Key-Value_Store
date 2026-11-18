// 文件用途：RegionPeerRegistry 单测（M6 挂账收口）：动态建 peer、按 key 路由、数据迁移。
// 归属：TinyKV-CPP（M6 运行时）。

#include "server/peer_registry.h"

#include <gtest/gtest.h>

#include <filesystem>

#include "common/error_format.h"
#include "raft_test_base.h"

namespace {

using tinykv::RegionId;
using tinykv::server::RegionPeerRegistry;
using tinykv::testlib::TempDir;

tinykv::raftstore::Region full_region() {
    return {.id = RegionId{1}, .start_key = "", .end_key = "", .peers = {},
            .epoch = {1, 0}, .size_hint = 0};
}

TEST(PeerRegistryTest, BootstrapCreatesOnePeerAndRoutesEveryKey) {
    TempDir tmp{"peer_registry"};
    tinykv::raftstore::StoreMeta meta;
    RegionPeerRegistry reg(tmp.path(), meta);
    auto region = full_region();
    ASSERT_TRUE(meta.add_region(region).has_value());   // 路由依赖 StoreMeta 的区间索引
    ASSERT_TRUE(reg.bootstrap(region).has_value());
    EXPECT_EQ(reg.size(), 1U);
    reg.campaign_all();
    EXPECT_NE(reg.peer_for("a"), nullptr);
    EXPECT_NE(reg.peer_for("z"), nullptr);
    EXPECT_NE(reg.peer_by_id(RegionId{1}), nullptr);
    EXPECT_EQ(reg.peer_by_id(RegionId{2}), nullptr);
    EXPECT_TRUE(reg.close_all().has_value());
}

TEST(PeerRegistryTest, OnSplitCreatesPeerAndMigratesRightHalf) {
    TempDir tmp{"peer_registry_split"};
    tinykv::raftstore::StoreMeta meta;
    RegionPeerRegistry reg(tmp.path(), meta);
    auto region = full_region();
    ASSERT_TRUE(meta.add_region(region).has_value());
    ASSERT_TRUE(reg.bootstrap(region).has_value());
    reg.campaign_all();
    auto* left_peer = reg.peer_by_id(RegionId{1});
    ASSERT_NE(left_peer, nullptr);
    ASSERT_TRUE(left_peer->is_leader());

    // 写左右两条（直接经 peer 提案走共识）
    std::uint64_t seq = 0;
    auto put = [&](const std::string& key, const std::string& value) {
        tinykv::cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(7);
        req.mutable_header()->set_sequence_id(++seq);
        auto* one = req.add_requests();
        one->mutable_put()->set_key(key);
        one->mutable_put()->set_value(value);
        const auto expect = left_peer->raft().last_index().value + 1;
        if (!left_peer->propose(req).has_value()) {
            return false;
        }
        for (int i = 0; i < 200 && !left_peer->applied_at_least(expect); ++i) {
            left_peer->tick();
            (void)left_peer->on_ready();
        }
        return left_peer->applied_at_least(expect);
    };
    ASSERT_TRUE(put("a1", "left"));
    ASSERT_TRUE(put("z1", "right"));

    // split at "m"：更新 StoreMeta（与 Peer::apply_split_admin 同款），再触发 on_split
    auto parts = tinykv::raftstore::split_region(region, "m", RegionId{2});
    ASSERT_TRUE(parts.has_value());
    ASSERT_TRUE(meta.replace_region(RegionId{1}, parts->first, parts->second).has_value());
    ASSERT_TRUE(reg.on_split(parts->first, parts->second).has_value());

    EXPECT_EQ(reg.size(), 2U);
    EXPECT_NE(reg.peer_by_id(RegionId{2}), nullptr);
    // 路由：左 key 仍归 peer 1，右 key 归新 peer 2
    EXPECT_EQ(reg.peer_for("a1"), reg.peer_by_id(RegionId{1}));
    EXPECT_EQ(reg.peer_for("z1"), reg.peer_by_id(RegionId{2}));
    // BUG-0022（N-4）：on_split 只注册不泵，读之前先按持闸方职责补泵推举
    auto* right = reg.peer_by_id(RegionId{2});
    for (int i = 0; i < 20 && !right->is_leader(); ++i) {
        right->tick();
    }
    (void)right->on_ready();
    ASSERT_TRUE(right->is_leader());
    // 数据迁移：新 peer 的引擎里能读到右半数据
    auto got = right->get(tinykv::Cf::Default, "z1");
    ASSERT_TRUE(got.has_value()) << tinykv::describe(got.error());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "right");
    EXPECT_TRUE(reg.close_all().has_value());
}

// BUG-0022（code-review r2 N-4）：on_split 只注册不泵——observer 回调可能发生在分区闸
// 泵路径（admin_split 超时后残留 split 在 pump_peer_only 里 apply），此时只持旧 region
// 的桶；若在此推举新 peer，另一线程可同时持新 region 桶触碰同一个 Peer 对象（registry
// 的 mu_ 只保护表结构，不保护 Peer 本体）。新 peer 注册后不得已是 leader；推举是持闸方
// （admin_split 成功路径的全局闸补泵 / Worker）的职责。
TEST(PeerRegistryTest, OnSplitOnlyRegistersWithoutPumping) {
    TempDir tmp{"peer_registry_n4"};
    tinykv::raftstore::StoreMeta meta;
    RegionPeerRegistry reg(tmp.path(), meta);
    auto region = full_region();
    ASSERT_TRUE(meta.add_region(region).has_value());
    ASSERT_TRUE(reg.bootstrap(region).has_value());
    reg.campaign_all();

    auto parts = tinykv::raftstore::split_region(region, "m", RegionId{2});
    ASSERT_TRUE(parts.has_value());
    ASSERT_TRUE(meta.replace_region(RegionId{1}, parts->first, parts->second).has_value());
    ASSERT_TRUE(reg.on_split(parts->first, parts->second).has_value());

    auto* right = reg.peer_by_id(RegionId{2});
    ASSERT_NE(right, nullptr);
    EXPECT_FALSE(right->is_leader()) << "on_split 内推举 = N-4 竞态窗口（回归：只注册不泵）";

    // 持闸方补泵（与 admin_split 成功路径/Worker 同款动作）：单节点组 tick 即成 leader
    for (int i = 0; i < 20 && !right->is_leader(); ++i) {
        right->tick();
    }
    (void)right->on_ready();
    ASSERT_TRUE(right->is_leader()) << "补泵后必须可服务（推举职责转移到持闸方，不丢功能）";

    // 推举后右半可写（契约：split 返回后右半立即可服务）
    tinykv::cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(9);
    req.mutable_header()->set_sequence_id(1);
    auto* one = req.add_requests();
    one->mutable_put()->set_key("z2");
    one->mutable_put()->set_value("fresh");
    const auto expect = right->raft().last_index().value + 1;
    ASSERT_TRUE(right->propose(req).has_value());
    bool applied = false;
    for (int i = 0; i < 200 && !applied; ++i) {
        right->tick();
        (void)right->on_ready();
        applied = right->applied_at_least(expect);
    }
    ASSERT_TRUE(applied) << "新 peer 补泵后必须能走完一次写提案（含 split 前迁移的初始条目）";
    EXPECT_TRUE(reg.close_all().has_value());
}

}  // namespace
