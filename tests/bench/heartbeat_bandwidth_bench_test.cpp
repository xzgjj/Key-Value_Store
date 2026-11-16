// 文件用途：Multi-Raft 心跳带宽规模化基准（PERF-08，SPEC-3 §3.7）。
// 归属：TinyKV-CPP（M4 Multi-Raft，LEVEL=bench → nightly）。
// 使用场景理解：PERF-08 的红线是"心跳只带元数据、禁止全量日志"。在 Multi-Raft 下
// 带宽随 region 数线性放大（region×peer 规模化），所以两条可判定性质是：
//   (a) **单条心跳体积与日志长度无关**（1k 条日志 vs 10 条日志的心跳字节相同）——
//       这条若破，region 一多、日志一长，心跳会退化为全量复制；
//   (b) 按 region 数外推的总带宽打印成表，供与 S0 目标对比。
// M2 的 cluster_bench 已测过"单 region 每拍心跳 <128B"；本文件补 Multi-Raft 维度。

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 强制 leader 发一轮心跳，返回（条数, 总字节）。心跳间隔 3 拍，tick 3 次必触发。
struct HbSample {
    std::size_t count{0};
    std::size_t bytes{0};
};

HbSample heartbeat_sample(Cluster& c) {
    const int leader = c.leader_index();
    if (leader < 0) {
        return {};   // 无 leader（elect 失败）时调用方断言会兜住
    }
    for (int i = 0; i < 3; ++i) {
        c.node(leader).tick();
    }
    auto ready = c.node(leader).take_ready();
    HbSample out;
    for (const auto& m : ready.messages) {
        if (m.id() == tinykv::raftpb::MSG_HEARTBEAT) {
            ++out.count;
            out.bytes += m.ByteSizeLong();
        }
    }
    return out;
}

TEST(HeartbeatBandwidthBench, SizeInvariantToLogLengthAndScalingTable) {
    // (a) 日志长度不变量：短日志 vs 长日志，leader 发出的心跳字节必须相同
    Cluster short_log(3);
    ASSERT_TRUE(short_log.elect());
    for (int i = 0; i < 10; ++i) {
        (void)short_log.node(short_log.leader_index()).propose("s" + std::to_string(i));
        short_log.flush();
    }
    const auto small = heartbeat_sample(short_log);

    Cluster long_log(3);
    ASSERT_TRUE(long_log.elect());
    for (int i = 0; i < 1000; ++i) {
        std::string value(128, 'x');   // 128B 载荷，放大"日志很长"的差异
        (void)long_log.node(long_log.leader_index()).propose(
            "L" + std::to_string(i) + ":" + value);
        long_log.flush();
    }
    const auto big = heartbeat_sample(long_log);

    std::printf("[bench] 日志 10 条  : %zu 条心跳 / %zu B\n", small.count, small.bytes);
    std::printf("[bench] 日志 1000 条(128B/条): %zu 条心跳 / %zu B\n", big.count, big.bytes);

    ASSERT_GT(small.count, 0U) << "未采到心跳（tick/间隔配置变化？）";
    EXPECT_EQ(small.count, big.count);
    // 字节不变的严格形式是"不携带 entries"（index/log_term 的 varint 长度会随日志序号
    // 合法地长 1~2 字节，不算违规）；超过 varint 增长量级才是真的带上了负载。
    EXPECT_LE(big.bytes, small.bytes + 8U)
        << "心跳体积增长超出 varint 编码量级——疑似携带了日志条目（PERF-08 红线）";
    // 心跳消息不得携带条目（结构断言，比字节上限更根本）
    const int leader = long_log.leader_index();
    for (int i = 0; i < 3; ++i) {
        long_log.node(leader).tick();
    }
    for (const auto& m : long_log.node(leader).take_ready().messages) {
        if (m.id() == tinykv::raftpb::MSG_HEARTBEAT) {
            EXPECT_EQ(m.entries_size(), 0) << "心跳携带日志条目";
        }
    }

    // (b) 规模化外推表：每 region 每 tick 两个方向（leader→peer、回执），10 心跳/s
    const double per_region_per_hb = static_cast<double>(big.bytes);
    std::printf("[bench] 规模化外推（每 region %zuB×2 对端×10HB/s）:\n", big.bytes);
    for (const int regions : {10, 100, 1000}) {
        const double kbps = per_region_per_hb * 2.0 * 10.0 * static_cast<double>(regions) / 1024.0;
        std::printf("[bench]   %5d regions : %.0f KB/s\n", regions, kbps);
    }
    EXPECT_LT(static_cast<double>(big.bytes), 128.0)
        << "单条心跳超过 128B，元数据之外可能带了负载";
}

}  // namespace
