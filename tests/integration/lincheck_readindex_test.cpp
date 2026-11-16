// 文件用途：线性一致读的两个可判定不变量——读己之写 + 单调读（REL-01/FUNC-05）。
// 归属：TinyKV-CPP（M3 一致读，integration）。
//
// **口径说明（重要）**：这不是 Wing-Gong 完整历史搜索器。完整 lincheck 需要记录全部操作的
// 起止时刻再做全序搜索，代价很高。这里检查的是读语义最关键的两条不变量：
//   1) 读己之写：一次写**返回之后**发起的读，必须看到该值或更新的值；
//   2) 单调读：同一客户端先后两次读，后一次不得看到更旧的值。
// 这两条一旦破，用户能直接观察到"数据回退"，是线性一致被违反时最典型的症状。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "peer_cluster.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 写 v0..vN，每次写完成后立刻读，且要求读值单调不减
void check_mode(PeerCluster& c, ReadMode mode) {
    const int leader = c.leader();
    ASSERT_GE(leader, 0);
    int last_seen = -1;
    std::uint64_t seq = 0;
    for (int round = 1; round <= 12; ++round) {
        const std::string value = "v" + std::to_string(round);
        ASSERT_TRUE(c.write(leader, 7, ++seq, "lin-key", value));
        auto got = c.read(leader, "lin-key", mode, 7, ++seq);
        ASSERT_TRUE(got.has_value()) << "第 " << round << " 轮读失败";
        ASSERT_TRUE(got->has_value());
        const int seen = std::stoi(got->value().substr(1));   // got-> 是 optional<string>
        // 读己之写：写完再读，至少看到刚写的版本
        EXPECT_GE(seen, round) << "读己之写被违反：期望 ≥ v" << round << "，实际 " << got->value();
        // 单调读：不得倒退
        EXPECT_GE(seen, last_seen) << "单调读被违反：上次 v" << last_seen << "，本次 " << got->value();
        last_seen = seen;
    }
}

TEST(LincheckReadIndex, ReadYourWritesAndMonotonicReads) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    check_mode(c, ReadMode::ReadIndex);
}

TEST(LincheckLeaseRead, ReadYourWritesAndMonotonicReads) {
    PeerCluster c(3);
    ASSERT_TRUE(c.elect());
    c.peer(c.leader()).enable_lease(true);
    // 刷新租约：心跳每 3 拍才发一次，故循环到租约生效为止（不要假设某一拍一定发心跳）
    for (int i = 0; i < 20 && !c.peer(c.leader()).lease_valid(); ++i) {
        for (int n = 0; n < c.size(); ++n) {
            c.peer(n).tick();
        }
        c.pump();
    }
    ASSERT_TRUE(c.peer(c.leader()).lease_valid()) << "租约在 20 拍内未生效";
    check_mode(c, ReadMode::Lease);
}

}  // namespace
