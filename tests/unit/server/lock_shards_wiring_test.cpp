// 文件用途：⑨a 接线用例（M7-PERF-01 的正确性面）——分片闸接入组合根后：
//          ① 行为等价（put/get/split/跨区事务全路径照常）；② 跨 region 并发可行；
//          ③ shards=1 回退档与改造前逐字节一致。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `lock_shards_wiring_test`）。
//
// 使用场景理解：lock_registry_test 已证明"分桶原语"本身正确（同区串行/跨区并行/桶稳定）；
// 本文件证明"接线"正确——真正危险的是组合根换闸后的回归：泵只泵目标 peer、
// split 走全局闸、事务提案走分区闸。并发写用多线程打 HTTP（每线程独立 CLI 连接），
// key 均匀散在两个 region，任何闸错乱都会表现为丢写/读不到/挂死。

#include "server/server_app.h"

#include "cli/cli.h"
#include "server_daemon.h"

#include <gtest/gtest.h>

#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using tinykv::testlib::ServerDaemon;
using tinykv::testlib::session_of;

// 跨区事务提交辅助（①③共用，消除同形块）：begin → set×2 → commit；返回是否全绿
bool commit_cross_region_txn(ServerDaemon& d, const std::string& k1, const std::string& v1,
                             const std::string& k2, const std::string& v2) {
    std::string begin;
    if (d.cli({"txn", "begin"}, &begin) != 0) {
        ADD_FAILURE() << d.last_err;
        return false;
    }
    const std::string sid = session_of(begin);
    if (sid.empty()) {
        ADD_FAILURE() << "无会话号: " << begin;
        return false;
    }
    if (d.cli({"txn", "set", sid, k1, v1}) != 0) {
        ADD_FAILURE() << d.last_err;
        return false;
    }
    if (d.cli({"txn", "set", sid, k2, v2}) != 0) {
        ADD_FAILURE() << d.last_err;
        return false;
    }
    if (d.cli({"txn", "commit", sid}) != 0) {
        ADD_FAILURE() << d.last_err;
        return false;
    }
    return true;
}

// ① 分片开启：split + 两区 kv 读写 + 跨区事务全路径照常（闸换了，语义不换）
TEST(LockShardsWiringTest, ShardedGateKeepsAllPathsCorrect) {
    ServerDaemon d(16, /*cross_region=*/true, "tinykv_lock_shards");
    std::string out;
    ASSERT_EQ(d.cli({"region", "split", "1", "m"}, &out), 0) << d.last_err << out;

    // 两区各写一条、各读一条（raw kv 路径 = 分区闸 + pump_peer_only 的组合）
    ASSERT_EQ(d.cli({"put", "a1", "va1"}), 0) << d.last_err;
    ASSERT_EQ(d.cli({"put", "z1", "vz1"}), 0) << d.last_err;
    std::string got;
    const int rc_a1 = d.cli({"get", "a1"}, &got);
    ASSERT_EQ(rc_a1, 0) << d.last_err;
    EXPECT_NE(got.find("va1"), std::string::npos) << got;
    const int rc_z1 = d.cli({"get", "z1"}, &got);
    ASSERT_EQ(rc_z1, 0) << d.last_err;
    EXPECT_NE(got.find("vz1"), std::string::npos) << got;

    // 跨区事务在分片闸下照常提交（执行器分区闸 + 决议广播全局闸的组合）
    ASSERT_TRUE(commit_cross_region_txn(d, "a2", "va2", "z2", "vz2"));
    // 事务写入在 MVCC 命名空间，必须开新会话用 txn get 读回（raw get 是另一个键空间）
    std::string begin2;
    ASSERT_EQ(d.cli({"txn", "begin"}, &begin2), 0) << d.last_err;
    const std::string sid2 = session_of(begin2);
    std::string got_z2;
    ASSERT_EQ(d.cli({"txn", "get", sid2, "z2"}, &got_z2), 0) << d.last_err;
    EXPECT_NE(got_z2.find("vz2"), std::string::npos)
        << "分片后跨区提交丢了副组写入: " << got_z2;
    ASSERT_EQ(d.cli({"txn", "rollback", sid2}), 0);

    // 分片状态下再 split 一次（全局闸 + registry 内部锁 + meta 落盘的组合）
    ASSERT_EQ(d.cli({"region", "split", "1", "f"}, &out), 0) << d.last_err << out;
    ASSERT_EQ(d.cli({"put", "b1", "vb1"}), 0) << d.last_err;   // 落 region 2 [f,m)
    const int rc_b1 = d.cli({"get", "b1"}, &got);
    ASSERT_EQ(rc_b1, 0) << d.last_err;
    EXPECT_NE(got.find("vb1"), std::string::npos) << got;
}

// ② 分片开启：多线程跨 region 并发写——任何闸错乱都会表现为丢写或读不到
TEST(LockShardsWiringTest, ConcurrentWritersAcrossRegionsAllLand) {
    ServerDaemon d(16, /*cross_region=*/true, "tinykv_lock_shards_conc");
    std::string out;
    ASSERT_EQ(d.cli({"region", "split", "1", "m"}, &out), 0) << d.last_err << out;

    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 6;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&d, &failures, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                // 一半线程写 region 1（a*），一半写 region 2（z*）：桶间并行是收益，不能串不了
                const std::string key = (t % 2 == 0 ? "a" : "z") + std::to_string(t) + "_" +
                                       std::to_string(i);
                const std::string value = "v" + key;
                // run_cli 收 span<const string>：花括号初始化不推导（notes.txt 踩坑），显式建 vector
                const std::vector<std::string> argv{"--endpoint",
                                                    "127.0.0.1:" +
                                                        std::to_string(d.app.status_port()),
                                                    "put", key, value};
                std::ostringstream o;
                std::ostringstream e;
                const int rc = tinykv::cli::run_cli(argv, o, e);
                if (rc != 0) {
                    ++failures;
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    EXPECT_EQ(failures.load(), 0) << "并发写入有 " << failures.load() << " 次失败";

    // 全部可读：48 个 key 一个不丢
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            const std::string key =
                (t % 2 == 0 ? "a" : "z") + std::to_string(t) + "_" + std::to_string(i);
            std::string got;
            const int rc = d.cli({"get", key}, &got);
            ASSERT_EQ(rc, 0) << "key=" << key << " " << d.last_err;
            EXPECT_NE(got.find("v" + key), std::string::npos) << "key=" << key << ": " << got;
        }
    }
}

// ③ shards=1 回退档：闸语义与改造前一致（同样的路径照常工作）
TEST(LockShardsWiringTest, SingleShardDegradesToLegacyBehavior) {
    ServerDaemon d(1, /*cross_region=*/true, "tinykv_lock_shards_legacy");
    std::string out;
    ASSERT_EQ(d.cli({"region", "split", "1", "m"}, &out), 0) << d.last_err << out;
    ASSERT_EQ(d.cli({"put", "a1", "va1"}), 0) << d.last_err;
    ASSERT_EQ(d.cli({"put", "z1", "vz1"}), 0) << d.last_err;
    std::string got;
    const int rc_z1 = d.cli({"get", "z1"}, &got);
    ASSERT_EQ(rc_z1, 0) << d.last_err;
    EXPECT_NE(got.find("vz1"), std::string::npos) << got;
    ASSERT_TRUE(commit_cross_region_txn(d, "a2", "va2", "z2", "vz2"));
    std::string begin2;
    ASSERT_EQ(d.cli({"txn", "begin"}, &begin2), 0) << d.last_err;
    const std::string sid2 = session_of(begin2);
    std::string got_a2;
    ASSERT_EQ(d.cli({"txn", "get", sid2, "a2"}, &got_a2), 0) << d.last_err;
    EXPECT_NE(got_a2.find("va2"), std::string::npos) << got_a2;
    ASSERT_EQ(d.cli({"txn", "rollback", sid2}), 0);
}

}  // namespace
