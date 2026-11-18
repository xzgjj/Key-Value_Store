// 文件用途：事务命令经 raft 的集成用例（M5 第⑤层，SPEC-4 架构落地的可判定证据）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：③④层证明的是"TxnClient + MvccTxn 编排语义对"；本文件证明同一套编排
// 换到 **经 raft 的执行端**（PeerTxnExecutor）后依然成立——事务命令走共识、三副本
// 各自 apply 三列族、冲突错误从 apply 侧结构化传回 client。三个断言面：
//   (a) 提交的数据在三副本可见（MVCC 状态被共识复制）；
//   (b) 写写冲突从 apply 侧传回 client（WriteConflict 结构化错误）；
//   (c) 崩溃残留锁经决议（resolve 走 raft 清锁）后所有副本一致、client 可继续。

#include "txn_fixture.h"

#include "peer_txn.h"
#include "txn/client.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "peer_cluster.h"

namespace {

using tinykv::testlib::PeerCluster;
using tinykv::testlib::PeerTxnExecutor;
using tinykv::txn::TxnClient;
using tinykv::tso::MonotonicTsProvider;

// 断言 commit 以 WriteConflict 失败（冲突从 apply 侧结构化传回；文件内单份避免复制粘贴）
static void expect_write_conflict(TxnClient& t, std::uint64_t now) {
    auto r = t.commit(now);
    EXPECT_FALSE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tinykv::WriteConflict>(r.error()));
}

struct Fixture : tinykv::testlib::TxnFixture {
    PeerCluster cluster{3};

    Fixture() {
        EXPECT_TRUE(cluster.elect()) << "三节点集群必须能选出 leader";
        // 执行端只建一次：client 持其引用，重复重建会让先前 client 悬垂（CI 构建段错误首跑踩坑）；
        // leader 查找在执行端内部每次动态进行，重建并无必要
        exec_ = std::make_unique<PeerTxnExecutor>(cluster);
    }

    // 覆盖基类 client()：执行端换成经 raft 的 Peer 端
    [[nodiscard]] TxnClient client() override { return TxnClient(*exec_, tso); }

    // 已提交基线（走本执行端；与 TxnFixture::commit_external 同语义）
    void commit_external(const std::string& key, const std::string& value) {
        auto c = client();
        now_ms += 10;
        (void)c.begin();
        (void)c.set(key, value);
        auto r = c.commit(now_ms);
        EXPECT_TRUE(r.has_value()) << "外部事务提交不应失败";
    }

    // 崩溃场景直接操作执行端（绕过 client 的 commit 全流程）
    [[nodiscard]] PeerTxnExecutor& exec() { return *exec_; }

    void tick_and_pump(int rounds = 3) {
        for (int i = 0; i < rounds; ++i) {
            for (int n = 0; n < cluster.size(); ++n) {
                cluster.peer(n).tick();
            }
            cluster.pump();
        }
    }

private:
    std::unique_ptr<PeerTxnExecutor> exec_;
};

// 提交经 raft：三副本的 MVCC 状态一致（数据被共识复制）
TEST(TxnPeer, CommitReplicatesMvccStateToAllReplicas) {
    Fixture f;
    auto c = f.client();
    (void)c.begin();
    (void)c.set("k", "v1");
    f.now_ms += 10;
    auto commit = c.commit(f.now_ms);
    ASSERT_TRUE(commit.has_value());

    for (int n = 0; n < f.cluster.size(); ++n) {
        auto got = f.cluster.peer(n).mvcc().get("k", tinykv::Ts{1000});
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->has_value()) << "副本 " << n << " 缺少已提交数据";
        EXPECT_EQ(**got, "v1") << "副本 " << n;
    }
}

// 写写冲突从 apply 侧结构化传回 client（首提交者胜，经共识判定）
TEST(TxnPeer, WriteConflictPropagatesFromApply) {
    Fixture f;
    auto [t1, t2] = f.begin_two("k", "a", "k", "b");
    ASSERT_TRUE(t2.commit(f.now_ms).has_value()) << "t2 首提交胜出";
    f.now_ms += 10;
    expect_write_conflict(t1, f.now_ms);   // 冲突从 apply 侧结构化传回
}

// 崩溃残留锁经决议（resolve 走 raft 清锁）后：副本一致、锁清除、新事务可提交
TEST(TxnPeer, CrashedTxnResolvedThroughRaftPath) {
    Fixture f;
    f.commit_external("k", "baseline");   // 已提交基线（证明决议不破坏既有数据）

    // 崩溃场景：事务 prewrite 后"停住"（不 commit），锁经 raft 落在全部副本
    auto crash = f.client();
    (void)crash.begin();
    (void)crash.set("k2", "half");
    f.now_ms += 10;
    // 直接用 PeerTxnExecutor 的 prewrite（TxnClient::commit 会连 commit 一起做完）
    {
        std::vector<tinykv::txn::Mutation> muts{
            {.op = tinykv::txn::Mutation::Op::kPut, .key = "k2", .value = "half"}};
        auto r = f.exec().prewrite(muts, "k2", crash.start_ts(), 3000, f.now_ms);
        ASSERT_TRUE(r.has_value());
    }
    for (int n = 0; n < f.cluster.size(); ++n) {
        auto st = f.cluster.peer(n).mvcc().check_txn_status("k2", crash.start_ts(), f.now_ms);
        ASSERT_TRUE(st.has_value());
        EXPECT_FALSE(st->rolled_back) << "副本 " << n << " 应看到活跃锁";
    }

    // TTL 过期后决议（resolve 的 rollback 经 raft）→ 三副本锁全清
    f.now_ms += 3001;
    {
        std::size_t cleaned = 0;
        auto r = f.exec().resolve_locks_of("k2", crash.start_ts(), std::nullopt);
        ASSERT_TRUE(r.has_value());
        cleaned = *r;
        EXPECT_EQ(cleaned, 1U);
    }
    f.tick_and_pump(5);
    for (int n = 0; n < f.cluster.size(); ++n) {
        auto st = f.cluster.peer(n).mvcc().check_txn_status("k2", crash.start_ts(), f.now_ms);
        ASSERT_TRUE(st.has_value());
        EXPECT_TRUE(st->rolled_back) << "副本 " << n << " 的锁应被决议清除";
    }

    // 清锁后新事务可正常提交同 key
    auto c2 = f.client();
    (void)c2.begin();
    (void)c2.set("k2", "fresh");
    f.now_ms += 10;
    ASSERT_TRUE(c2.commit(f.now_ms).has_value());
}

// BUG-0007: ARCH-1 P0-2——加锁时刻必须经协议到达 apply 侧。
// 使用场景：常驻进程的注入时钟已经跑过 TTL（now_ms 远大于 3000）。旧实现把 created_ms
// 写死 0，于是"现在 - 0" 恒大于 TTL，任何新锁都被判过期，并发事务会把活锁当死锁回滚。
TEST(TxnPeer, LockTtlUsesRequestCreationTime) {
    Fixture f;
    const std::uint64_t created = 500'000;   // 模拟进程已运行很久（远超默认 TTL 3s）
    auto t = f.client();
    (void)t.begin();
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "ttl_k", .value = "v"}};
    ASSERT_TRUE(f.exec().prewrite(muts, "ttl_k", t.start_ts(), 3000, created).has_value());

    // TTL 窗口内：锁必须"活着"（remaining > 0），而不是被当成出生即过期
    auto alive = f.exec().check_txn_status("ttl_k", t.start_ts(), created + 100);
    ASSERT_TRUE(alive.has_value());
    EXPECT_FALSE(alive->committed);
    EXPECT_FALSE(alive->rolled_back);
    EXPECT_GT(alive->lock_ttl_remaining_ms, 0U)
        << "created_ms 未随请求传递：运行超过 TTL 后新锁被判出生即过期";

    // 越过 TTL：过期分支仍必须可触发（修复不能把 TTL 变成"永不过期"）
    auto dead = f.exec().check_txn_status("ttl_k", t.start_ts(), created + 3000);
    ASSERT_TRUE(dead.has_value());
    EXPECT_EQ(dead->lock_ttl_remaining_ms, 0U);
    EXPECT_FALSE(dead->committed);
}

// BUG-0008: ARCH-1 P0-1——跨 region 事务必须在提案前被结构化拒绝。
// 使用场景：primary 在 region 1、secondary 在 region 2。旧实现把整批 mutations 经
// primary 所在 peer 一次提案，数据全落 region 1，读 region 2 得空——"提交成功但数据不见了"。
TEST(TxnPeer, CrossRegionPrewriteRejectedBeforeProposal) {
    Fixture f;
    // 虚拟路由：key < "m" 属 region 1，其余属 region 2（与 split 后的布局同构）
    PeerTxnExecutor guarded(
        f.cluster, [](std::string_view key) -> std::optional<tinykv::RegionId> {
            return key < "m" ? tinykv::RegionId{1} : tinykv::RegionId{2};
        });
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "a", .value = "1"},
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "z", .value = "2"}};
    auto r = guarded.prewrite(muts, "a", tinykv::Ts{100}, 3000, 10);
    ASSERT_FALSE(r.has_value()) << "跨区 prewrite 不得静默成功";
    ASSERT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
    // 护栏在提案之前：两个 key 都不应落锁（读回不报 LockConflict 即证明无锁）
    for (const std::string key : {"a", "z"}) {
        auto got = f.cluster.peer(0).mvcc().get(key, tinykv::Ts{1000});
        ASSERT_TRUE(got.has_value()) << key << " 被写入了锁：护栏没在提案前生效";
        EXPECT_FALSE(got->has_value()) << key << " 不应有数据落地";
    }
}

// 单区事务在护栏开启时照常通过（护栏不能误伤正常路径）
TEST(TxnPeer, SingleRegionPrewritePassesGuard) {
    Fixture f;
    PeerTxnExecutor guarded(
        f.cluster, [](std::string_view) { return tinykv::RegionId{1}; });
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "aa", .value = "1"},
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "ab", .value = "2"}};
    ASSERT_TRUE(guarded.prewrite(muts, "aa", tinykv::Ts{100}, 3000, 10).has_value());
}

}  // namespace
