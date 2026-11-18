// 文件用途：读路径统一回归——raw 与 txn 读都必须经 ReadIndex 确认并等 applied 追平。
// 归属：TinyKV-CPP（M7 ③ 读路径统一 ReadIndex）。
// 使用场景理解：leader 本地直读在分区/落后副本上会返回旧值。这里用三副本进程内集群
// 证明读真的发起了 ReadIndex，并且读到的是已提交且已 apply 的版本。

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "common/error_format.h"
#include "peer_cluster.h"
#include "peer_txn.h"
#include "raft_test_base.h"

namespace {

using tinykv::Cf;
using tinykv::Ts;
using tinykv::testlib::PeerCluster;
using tinykv::testlib::PeerTxnExecutor;

TEST(ReadPath, RawReadWaitsForReadIndexAndApplied) {
    PeerCluster cluster(3);
    ASSERT_TRUE(cluster.elect());
    const int leader = cluster.leader();
    ASSERT_GE(leader, 0);
    ASSERT_TRUE(cluster.write(leader, 7, 1, "rk", "rv"));

    auto& p = cluster.peer(leader);
    auto ri = p.wait_read_index([&] { cluster.pump(); });
    ASSERT_TRUE(ri.has_value());
    EXPECT_GE(p.read_index_calls(), 1U);
    auto got = p.read_directly(Cf::Default, "rk");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "rv");
}

TEST(ReadPath, TxnExecutorGetUsesReadIndex) {
    PeerCluster cluster(3);
    ASSERT_TRUE(cluster.elect());
    const int leader = cluster.leader();
    ASSERT_GE(leader, 0);

    PeerTxnExecutor exec(cluster);

    auto before = exec.get("tk", Ts{1000});
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->has_value());
    EXPECT_GE(cluster.peer(leader).read_index_calls(), 1U);

    std::vector<tinykv::txn::Mutation> mutations{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "tk", .value = "tv"}};
    auto pre = exec.prewrite(mutations, "tk", Ts{10}, 3000, 0);
    ASSERT_TRUE(pre.has_value()) << (pre ? "" : tinykv::describe(pre.error()));
    const std::vector<std::string> commit_keys{"tk"};
    ASSERT_TRUE(exec.commit(commit_keys, Ts{10}, Ts{20}).has_value());

    auto after = exec.get("tk", Ts{1000});
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->has_value());
    EXPECT_EQ(**after, "tv");
    EXPECT_GE(cluster.peer(leader).read_index_calls(), 2U);   // 两次 get 各发一次 ReadIndex
}

}  // namespace
