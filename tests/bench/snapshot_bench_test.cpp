// 文件用途：快照传输吞吐基准（PERF-09，SPEC-3 §3.7：≥10MB/s (S0)）。
// 归属：TinyKV-CPP（M4 Multi-Raft，LEVEL=bench → nightly）。
// 使用场景理解：S0 要证明分块快照管线的吞吐 ≥10MB/s（dump→4MB 切块→逐块 CRC→流控→
// 安装）。测法：3 节点集群先隔离 follower → leader 写入 ~13MB 状态并压缩日志 →
// 解除隔离触发快照路径（send_append 发现 next < first_index → 分块流）→
// 从解除隔离到 follower 落盘哨兵键计时。进程内总线传输很快，S0 是"下限证明"，
// 实际读数由人工判定与目标环境对比。

#include "peer_cluster.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::cmdpb::RaftCmdRequest;

constexpr int kRecords = 100'000;
constexpr std::size_t kValueBytes = 128;   // 100k × (key ~9B + 128B) ≈ 13MB → 4 个 4MB 块

// 批量写入一批（batch=256），泵到回执出现
bool write_batch(PeerCluster& c, int leader, std::uint64_t client, std::uint64_t seq,
                 std::int64_t begin, std::int64_t end) {
    RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    for (std::int64_t i = begin; i < end; ++i) {
        auto* one = req.add_requests();
        one->mutable_put()->set_key("s:" + std::to_string(i));
        one->mutable_put()->set_value(std::string(kValueBytes, 'x'));
    }
    if (!c.peer(leader).propose(req).has_value()) {
        return false;
    }
    for (int round = 0; round < 400; ++round) {
        c.pump();
        if (c.peer(leader).take_reply(client, seq).has_value()) {
            return true;
        }
    }
    return false;
}

TEST(SnapshotBench, ChunkedSnapshotThroughput) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    const int lagging = (leader + 1) % c.size();   // 待追赶节点

    // 隔离 lagging：之后的写入它全部错过（tick 只给其余节点，避免它超时竞选）
    c.set_partition({lagging});
    std::uint64_t seq = 0;
    for (std::int64_t begin = 0; begin < kRecords; begin += 256) {
        ++seq;
        const std::int64_t end = std::min(begin + 256, static_cast<std::int64_t>(kRecords));
        ASSERT_TRUE(write_batch(c, leader, 7, seq, begin, end)) << "batch@" << begin;
        c.peer(leader).tick();
        c.peer((leader + 2) % c.size()).tick();   // 另一个 follower 也要活着
        c.pump();
    }
    const std::string sentinel = "s:" + std::to_string(kRecords - 1);

    // 压缩日志：lagging 需要的条目已不在 → 解除隔离必然走快照路径
    const auto committed = c.peer(leader).raft().committed();
    ASSERT_TRUE(c.peer(leader).compact_log(committed).has_value());
    c.peer(leader).tick();
    c.peer((leader + 2) % c.size()).tick();
    c.pump();

    // 解除隔离：计时从解除到 lagging 落盘哨兵键（dump→切块→CRC→流控→安装全链路）
    c.set_partition({});
    const auto t0 = std::chrono::steady_clock::now();
    bool found = false;
    for (int round = 0; round < 4000 && !found; ++round) {
        for (int n = 0; n < c.size(); ++n) {
            c.peer(n).tick();   // 无 tick 则 leader 永不发心跳，next<first 的发现路径不触发
        }
        c.pump();
        auto got = c.peer(lagging).read_directly(tinykv::Cf::Default, sentinel);
        found = got.has_value() && got->value.has_value();
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(found) << "快照追赶未完成（哨兵键不可读）";

    // 状态体积（按 key+value 近似）与吞吐
    const double mb = static_cast<double>(kRecords) *
                      (static_cast<double>(kValueBytes) + 16.0) / (1024.0 * 1024.0);
    const double mbps = mb / secs;
    std::printf("[bench] 快照 %.1f MB / %.2f s = %.0f MB/s（%d 条 → 4MB 分块）\n", mb, secs,
                mbps, kRecords);

    // 结构断言：S0 下限 + 追上后哨兵值正确
    EXPECT_GE(mbps, 10.0) << "快照吞吐低于 S0 下限 10MB/s";
    auto check = c.peer(lagging).read_directly(tinykv::Cf::Default, "s:0");
    ASSERT_TRUE(check.has_value());
    ASSERT_TRUE(check->value.has_value());
    EXPECT_EQ(*check->value, std::string(kValueBytes, 'x'));
}

}  // namespace
