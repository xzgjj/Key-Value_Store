// 文件用途：快照分块流单测（M4 第⑤层，FUNC-11/REL-03，SPEC-3 D3-3）。
// 归属：TinyKV-CPP（M4 Multi-Raft 快照分块）。
// 使用场景理解：region 变大后快照不能单块全量传输——必须按 4MB 切块、流内序号、逐块 CRC，
//   接收端重组后才安装。两条最必须证明的边界：
//   (a) 多块快照能真正重组并让落后节点追平（不是只移指针）；
//   (b) 坏块（CRC 不符）必须被拒、绝不把脏数据装进存储。

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <variant>

#include "common/compat.h"
#include "common/crc32c.h"
#include "common/error.h"
#include "peer_cluster.h"

namespace {

using namespace tinykv::testlib;  // NOLINT(google-build-using-namespace)

// 用多块路径追平：落后节点在日志压缩后只能靠分块快照追上
TEST(SnapshotChunk, LaggingFollowerCatchesUpViaChunks) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_GE(leader, 0);
    int victim = -1;
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader) {
            victim = i;
            break;
        }
    }
    ASSERT_GE(victim, 0);
    // 强制小块：每条 kv 都跨多块，覆盖 seq 组装与窗口流控
    for (int i = 0; i < c.size(); ++i) {
        c.peer(i).set_snapshot_chunk_bytes(16);
    }

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(c.write(leader, 1, static_cast<std::uint64_t>(i) + 1,
                            "key-" + std::to_string(i), "value-" + std::to_string(i)));
    }
    c.set_partition({victim});
    for (int i = 4; i < 8; ++i) {
        ASSERT_TRUE(c.write(leader, 1, static_cast<std::uint64_t>(i) + 1,
                            "key-" + std::to_string(i), "value-" + std::to_string(i)));
    }
    const auto committed = c.peer(leader).raft().committed();
    ASSERT_GE(committed.value, 8U);
    for (int i = 0; i < c.size(); ++i) {
        if (i != victim) {
            ASSERT_TRUE(c.peer(i).compact_log(c.peer(i).raft().committed()).has_value());
        }
    }

    c.set_partition({});
    for (int round = 0; round < 400; ++round) {
        for (int i = 0; i < c.size(); ++i) {
            c.peer(i).tick();
        }
        c.pump();
        if (c.peer(victim).raft().committed().value >= committed.value) {
            break;
        }
    }
    EXPECT_GE(c.peer(victim).raft().committed().value, committed.value)
        << "落后节点未通过分块快照追平";

    // 快照内容必须真落盘：抽查隔离期间写入的键
    for (int i = 4; i < 8; ++i) {
        auto got = c.peer(victim).read_directly(tinykv::Cf::Default, "key-" + std::to_string(i));
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value()) << "分块快照安装后缺 key-" << i;
        EXPECT_EQ(*got->value, "value-" + std::to_string(i));
    }
}

// 坏块必须被拒：CRC 不符直接返回 Corrupted，不进入安装流程
TEST(SnapshotChunk, CorruptChunkRejected) {
    PeerCluster c(1);
    ASSERT_TRUE(c.elect());
    const int self = c.leader();
    ASSERT_GE(self, 0);

    tinykv::raftpb::SnapshotChunk chunk;
    chunk.set_snapshot_index(10);
    chunk.set_snapshot_term(1);
    chunk.set_chunk_seq(0);
    chunk.set_total_chunks(1);
    chunk.set_data("payload");
    // 写入"正确 CRC + 1"作为坏值：与真实 CRC 必不相等，模拟传输篡改
    const auto good =
        tinykv::crc32c(tinykv::as_bytes(tinykv::span{chunk.data().data(), chunk.data().size()}));
    chunk.set_crc32(good + 1);
    std::string blob;
    ASSERT_TRUE(chunk.SerializeToString(&blob));

    tinykv::raftpb::Message msg;
    msg.set_id(tinykv::raftpb::MSG_SNAPSHOT_CHUNK);
    msg.set_term(1);
    msg.set_from(2);
    msg.set_to(1);
    msg.set_snapshot_data(blob);

    auto r = c.peer(self).step_message(msg);
    EXPECT_FALSE(r.has_value()) << "坏块不应被接受";
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(r.error()));
}

// code-review：半截快照必须有超时回收。旧实现传输中断后接收缓存（最多 512MB/peer）
// 会一直占着，长跑即内存泄漏。
TEST(SnapshotChunk, StalePartialRecvIsReclaimedByTick) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_GE(leader, 0);
    int victim = -1;
    for (int i = 0; i < c.size(); ++i) {
        if (i != leader) {
            victim = i;
            break;
        }
    }
    ASSERT_GE(victim, 0);
    auto& peer = c.peer(victim);
    ASSERT_EQ(peer.snapshot_recv_count(), 0U);

    // 只发第一块（total=3）：接收缓存建立后在途，模拟传输中断
    tinykv::raftpb::SnapshotChunk chunk;
    chunk.set_snapshot_index(100);
    chunk.set_snapshot_term(2);
    chunk.set_total_chunks(3);
    chunk.set_chunk_seq(0);
    chunk.set_data("partial");
    const auto crc = tinykv::crc32c(
        tinykv::as_bytes(tinykv::span{chunk.data().data(), chunk.data().size()}));
    chunk.set_crc32(crc);
    tinykv::raftpb::Message msg;
    msg.set_id(tinykv::raftpb::MSG_SNAPSHOT_CHUNK);
    msg.set_term(peer.raft().term().value);
    msg.set_from(tinykv::PeerId{99}.value);
    msg.set_to(peer.id().value);
    msg.set_snapshot_data(chunk.SerializeAsString());
    ASSERT_TRUE(peer.step_message(msg).has_value());
    ASSERT_EQ(peer.snapshot_recv_count(), 1U);

    // 300 tick 内不回收（避免误伤"Just slow"的正常传输），301+ 后必须清掉
    for (int i = 0; i < 300; ++i) {
        peer.tick();
    }
    EXPECT_EQ(peer.snapshot_recv_count(), 1U);
    for (int i = 0; i < 2; ++i) {
        peer.tick();
    }
    EXPECT_EQ(peer.snapshot_recv_count(), 0U) << "半截快照必须在超时后释放";
}

}  // namespace
