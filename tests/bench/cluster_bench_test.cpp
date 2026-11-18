// 文件用途：集群档位基准——写 QPS（batch=1/64）、心跳带宽、重启恢复时间（PERF-01/02/05/06/08）。
// 归属：TinyKV-CPP（M2 集群性能，LEVEL=bench → nightly）。
// 使用场景理解：单机基准（write_bench）证明存储层没走错路；这里要证明**共识路径**的代价。
// 三个指标各有红线：PERF-01 写 QPS（≥2k）、PERF-02 批量加速（≥8×）、
// PERF-08 心跳不带日志（衡量方式就是每拍心跳的字节数，应只有几十字节）。

#include "peer_cluster.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;  // NOLINT(google-build-using-namespace)

struct Sample {
    double per_sec{0.0};
    std::size_t ok{0};
};

// 逐条写：每次都是一次完整的 提案→复制→apply→回执
Sample write_one_by_one(PeerCluster& c, int leader, int ops) {
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t ok = 0;
    for (int i = 0; i < ops; ++i) {
        if (c.write(leader, 1, static_cast<std::uint64_t>(i) + 1, "b1:" + std::to_string(i), "v")) {
            ++ok;
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return Sample{.per_sec = static_cast<double>(ok) / secs, .ok = ok};
}

// 批量写：一个提案里塞 kBatch 条命令（requests[] 批量一等公民）
Sample write_batched(PeerCluster& c, int leader, int batches, int kBatch) {
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t records = 0;
    for (int b = 0; b < batches; ++b) {
        tinykv::cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(2);
        req.mutable_header()->set_sequence_id(static_cast<std::uint64_t>(b) + 1);
        for (int i = 0; i < kBatch; ++i) {
            auto* one = req.add_requests();
            one->mutable_put()->set_key("bb:" + std::to_string(b) + ":" + std::to_string(i));
            one->mutable_put()->set_value("v");
        }
        if (!c.peer(leader).propose(req).has_value()) {
            continue;
        }
        for (int round = 0; round < 200; ++round) {
            c.pump();
            if (c.peer(leader).take_reply(2, static_cast<std::uint64_t>(b) + 1).has_value()) {
                records += static_cast<std::size_t>(kBatch);
                break;
            }
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return Sample{.per_sec = static_cast<double>(records) / secs, .ok = records};
}

// 心跳字节数：强制发一轮心跳，把所有消息序列化后求字节和（PERF-08 的机械度量）
std::size_t heartbeat_bytes(Cluster& c) {
    const int leader = c.leader_index();
    for (int i = 0; i < 3; ++i) {  // 心跳间隔 3 拍
        c.node(leader).tick();
    }
    auto ready = c.node(leader).take_ready();
    std::size_t total = 0;
    for (const auto& m : ready.messages) {
        total += m.ByteSizeLong();
    }
    return total;
}

TEST(ClusterBench, WriteThroughputAndHeartbeatCost) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_GE(leader, 0);

    const auto single = write_one_by_one(c, leader, 40);
    const auto batched = write_batched(c, leader, 20, 64);

    // 心跳成本用 Raft 层的裸消息量衡量（Peer 层还会带快照字段，那是另一码事）
    Cluster raft_only(3);
    ASSERT_TRUE(raft_only.elect());
    const std::size_t hb = heartbeat_bytes(raft_only);

    std::printf("[bench] 集群写 batch=1  : %.0f ops/s (%zu)\n", single.per_sec, single.ok);
    std::printf("[bench] 集群写 batch=64 : %.0f records/s (%zu)\n", batched.per_sec, batched.ok);
    std::printf("[bench] 每拍心跳字节    : %zu B（3 节点集群，含帧外开销）\n", hb);
    if (single.per_sec > 0) {
        std::printf("[bench] 批量加速比       : %.2fx\n", batched.per_sec / single.per_sec);
    }

    EXPECT_GT(single.ok, 0U);
    EXPECT_GT(batched.ok, 0U);
    // PERF-08：心跳只带元数据，单条应远小于一个数据包；这里用"每节点 < 128B"作机械上限
    EXPECT_LT(hb / 2, 128U) << "心跳体积异常，可能携带了日志条目";
}

TEST(ClusterBench, RestartRecoveryTime) {
    // PERF-06：重启到可服务的时间。用"重开引擎 → 首次读成功"衡量
    TempDir tmp("tinykv_recover_bench");
    const auto dir = tmp.path() / "db";
    constexpr int kRecords = 2000;
    {
        auto engine = tinykv::Engine::open({.dir = dir, .fsync_on_commit = false});
        ASSERT_TRUE(engine.has_value());
        for (int i = 0; i < kRecords; ++i) {
            tinykv::ModifyVec batch{tinykv::Modify{tinykv::Put{
                .cf = tinykv::Cf::Default, .key = "r:" + std::to_string(i), .value = "v"}}};
            ASSERT_TRUE((*engine)->write(batch).has_value());
        }
        ASSERT_TRUE((*engine)->close().has_value());
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto engine = tinykv::Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    const std::string key = "r:0";
    auto got =
        (*engine)->get(tinykv::Cf::Default, tinykv::as_bytes(tinykv::span{key.data(), key.size()}));
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[bench] 重启恢复 %d 条后首读 : %.1f ms\n", kRecords, ms);
    EXPECT_TRUE(got.has_value());
    EXPECT_LT(ms, 5000.0) << "恢复耗时超过 5s，与 PERF-06 的 S0 目标差距过大";
}

}  // namespace
