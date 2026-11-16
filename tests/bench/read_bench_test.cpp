// 文件用途：三模式读基准——ReadIndex / Lease / Log 的 ops/s 与倍率（PERF-03/04）。
// 归属：TinyKV-CPP（M3 一致读，LEVEL=bench → nightly）。
// 使用场景理解：PERF-03 要证明"ReadIndex 读 ≥3× 写共识路径"。基线是**把读也走一遍共识日志**
// （Log 模式），不是"直接读存储"——只有同语义对比才有意义：三者都保证线性一致，
// 差别只在要付多少代价。

#include "peer_cluster.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

struct Sample {
    double ops_per_sec{0.0};
    std::size_t ok{0};
};

Sample measure(PeerCluster& c, int leader, ReadMode mode, int ops) {
    const auto started = std::chrono::steady_clock::now();
    std::size_t ok = 0;
    for (int i = 0; i < ops; ++i) {
        auto got = c.read(leader, "bench-key", mode, 100, static_cast<std::uint64_t>(i + 1));
        if (got.has_value()) {
            ++ok;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return Sample{.ops_per_sec = static_cast<double>(ok) / secs, .ok = ok};
}

TEST(ReadBench, ReadIndexVersusLogBaseline) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    const int leader = c.leader();
    ASSERT_TRUE(c.write(leader, 1, 1, "bench-key", "v"));

    // 租约预热：先让 leader 收到一轮 quorum 心跳回执
    c.peer(leader).enable_lease(true);
    for (int i = 0; i < 3; ++i) {
        for (int n = 0; n < c.size(); ++n) {
            c.peer(n).tick();
        }
        c.pump();
    }

    constexpr int kOps = 60;   // 次数适中：log 模式每次要走一遍共识，太多会拖长夜跑
    const auto read_index = measure(c, leader, ReadMode::ReadIndex, kOps);
    const auto lease = measure(c, leader, ReadMode::Lease, kOps);
    const auto log = measure(c, leader, ReadMode::Log, kOps);

    std::printf("[bench] read_index : %.0f ops/s (%zu/%d)\n", read_index.ops_per_sec, read_index.ok, kOps);
    std::printf("[bench] lease      : %.0f ops/s (%zu/%d)\n", lease.ops_per_sec, lease.ok, kOps);
    std::printf("[bench] log(基线)  : %.0f ops/s (%zu/%d)\n", log.ops_per_sec, log.ok, kOps);
    if (log.ops_per_sec > 0) {
        std::printf("[bench] 倍率 read_index/log = %.2fx, lease/log = %.2fx\n",
                    read_index.ops_per_sec / log.ops_per_sec,
                    lease.ops_per_sec / log.ops_per_sec);
    }

    // 全部读都必须成功——基准里出现失败说明协议路径有问题，不能当噪声放过
    EXPECT_EQ(read_index.ok, static_cast<std::size_t>(kOps));
    EXPECT_EQ(lease.ok, static_cast<std::size_t>(kOps));
    EXPECT_EQ(log.ok, static_cast<std::size_t>(kOps));
    // 只断言"不慢于走共识"这条底线；S0/S1 的 3×/5× 由人工读数判定（机器差异太大）
    EXPECT_GT(read_index.ops_per_sec, log.ops_per_sec) << "ReadIndex 竟然不比走日志快，实现有问题";
}

}  // namespace
