// 文件用途：快照追赶——落后节点在日志被压缩后必须靠快照追上（FUNC-03 快照安装）。
// 归属：TinyKV-CPP（M2 快照，integration）。
// 使用场景理解：这条路径只在"follower 落后 + leader 已压缩日志"时触发，是最容易漏做的一条：
// 逐条退格永远退不到已删除的条目，必须显式识别 next_index < first_index 并改走快照。

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "peer_cluster.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

TEST(Snapshot, LaggingFollowerCatchesUpAfterCompaction) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_GE(leader, 0);

    // 1) 先写 5 条，三个节点同步到同一位置
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(c.write(leader, 1, static_cast<std::uint64_t>(i) + 1, "k" + std::to_string(i),
                            "v" + std::to_string(i)));
    }

    // 2) 隔离一个非 leader 节点，再写 5 条——它必然落后
    int victim = -1;
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader) {
            victim = i;
            break;
        }
    }
    ASSERT_GE(victim, 0);
    c.set_partition({victim});
    for (int i = 5; i < 10; ++i) {
        ASSERT_TRUE(c.write(leader, 1, static_cast<std::uint64_t>(i) + 1, "k" + std::to_string(i),
                            "v" + std::to_string(i)));
    }

    // 3) 把未隔离节点的日志压缩到已提交位点：victim 需要的条目就此消失
    const auto committed = c.peer(leader).raft().committed();
    ASSERT_GE(committed.value, 10U);
    for (int i = 0; i < c.size(); ++i) {
        if (i == victim) {
            continue;
        }
        // 各节点按自己的 committed 压缩：follower 的提交位点可能落后于 leader，
        // 用 leader 的位点压它会越界（compact 不得越过 committed，红线 9）
        const auto own = c.peer(i).raft().committed();
        auto r = c.peer(i).compact_log(own);
        ASSERT_TRUE(r.has_value()) << "节点 " << i << " 压缩失败";
    }

    // 4) 解除隔离：victim 只能靠快照追平
    c.set_partition({});
    for (int round = 0; round < 300; ++round) {
        for (int i = 0; i < c.size(); ++i) {
            c.peer(i).tick();
        }
        c.pump();
        if (c.peer(victim).raft().committed().value >= committed.value) {
            break;
        }
    }

    EXPECT_GE(c.peer(victim).raft().committed().value, committed.value)
        << "落后节点未通过快照追上已提交位点";

    // 5) 快照内容必须真的落到 victim 的存储里（不只是指针前移）
    for (int i = 5; i < 10; ++i) {
        auto got = c.peer(victim).read_directly(tinykv::Cf::Default, "k" + std::to_string(i));
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value()) << "快照安装后缺少 k" << i;
        EXPECT_EQ(*got->value, "v" + std::to_string(i));
    }
}

TEST(Snapshot, InstallRemovesKeysMissingFromSnapshot) {
    // 只覆盖不删除会让分叉日志留下的脏键永久残留，这里经 step_message 走真实安装路径
    PeerCluster c(1);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_TRUE(c.write(leader, 1, 1, "gone", "stale"));
    ASSERT_TRUE(c.write(leader, 1, 2, "keep", "yes"));

    tinykv::raftpb::SnapshotData data;
    auto* kv = data.add_kvs();
    kv->set_cf(static_cast<std::uint32_t>(tinykv::Cf::Default));
    kv->set_key("keep");
    kv->set_value("yes");
    std::string blob;
    ASSERT_TRUE(data.SerializeToString(&blob));

    const auto term = c.peer(leader).raft().term().value;
    const auto last = c.peer(leader).raft().last_index().value;
    tinykv::raftpb::Message snap;
    snap.set_id(tinykv::raftpb::MSG_SNAPSHOT);
    snap.set_term(term);
    snap.set_from(2);
    snap.set_to(1);
    snap.set_index(last);
    snap.set_log_term(term);
    snap.set_snapshot_data(blob);
    ASSERT_TRUE(c.peer(leader).step_message(snap).has_value());

    auto stale = c.peer(leader).read_directly(tinykv::Cf::Default, "gone");
    ASSERT_TRUE(stale.has_value());
    EXPECT_FALSE(stale->value.has_value()) << "快照安装未清除快照外的键";
    auto kept = c.peer(leader).read_directly(tinykv::Cf::Default, "keep");
    ASSERT_TRUE(kept.has_value());
    ASSERT_TRUE(kept->value.has_value());
    EXPECT_EQ(*kept->value, "yes");
}

}  // namespace
