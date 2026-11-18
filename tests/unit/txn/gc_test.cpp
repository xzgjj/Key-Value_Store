// 文件用途：GC 用例（M6，FUNC-17，SPEC-4 §3.7 gc_test）。
// 归属：TinyKV-CPP（M6 运行时 / M5 事务 P2 功能）。
// 使用场景理解：GC 删"安全点以下"旧版本（红线7），两条可判定性质：
//   (a) 安全点之前的旧版本被清（读旧快照不再命中该版本）；
//   (b) 安全点之后的版本与最新已提交值不受影响（活跃读不破）。
// 走 MvccTxn 直测（语义）；走 raft 的路径由 server_app_test 的 /txn/gc 覆盖。

#include "txn/mvcc.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "storage/engine.h"
#include "temp_dir.h"
#include "txn_fixture.h"

namespace {

using tinykv::Ts;
using tinykv::txn::MvccTxn;
using tinykv::txn::Mutation;
using Fixture = tinykv::testlib::TxnFixture;

std::vector<Mutation> put(const std::string& key, const std::string& value) {
    return {Mutation{.op = Mutation::Op::kPut, .key = key, .value = value}};
}

TEST(GcTest, RemovesVersionsBelowSafePoint) {
    Fixture f;
    auto mvcc = f.mvcc();
    // 三个版本：commit 10(v1) / 20(v2) / 40(v3)
    ASSERT_TRUE(mvcc.prewrite(put("k", "v1"), "k", Ts{5}, 0, 0).has_value());
    ASSERT_TRUE(mvcc.commit(std::vector<std::string>{"k"}, Ts{5}, Ts{10}).has_value());
    ASSERT_TRUE(mvcc.prewrite(put("k", "v2"), "k", Ts{15}, 0, 0).has_value());
    ASSERT_TRUE(mvcc.commit(std::vector<std::string>{"k"}, Ts{15}, Ts{20}).has_value());
    ASSERT_TRUE(mvcc.prewrite(put("k", "v3"), "k", Ts{35}, 0, 0).has_value());
    ASSERT_TRUE(mvcc.commit(std::vector<std::string>{"k"}, Ts{35}, Ts{40}).has_value());

    // 安全点 25：保留 ≤25 的最新版本 = commit 20(v2)；commit 10 应被清
    auto gc = mvcc.gc(Ts{25});
    ASSERT_TRUE(gc.has_value());
    EXPECT_GE(gc->first, 1U) << "至少清掉一个旧 Write 版本";

    // (a) 读旧快照 15：v1 已被清 → 读不到
    auto old = mvcc.get("k", Ts{15});
    ASSERT_TRUE(old.has_value());
    EXPECT_FALSE(old->has_value()) << "安全点之前的版本应被清理";

    // (b) 读 25：v2 仍在（保留版本）；读 50：v3 不受影响
    auto at25 = mvcc.get("k", Ts{25});
    auto at50 = mvcc.get("k", Ts{50});
    ASSERT_TRUE(at25.has_value() && at50.has_value());
    EXPECT_EQ(**at25, "v2");
    EXPECT_EQ(**at50, "v3") << "安全点之后的版本必须不受影响";
}

TEST(GcTest, KeepsUncommittedAndNewerData) {
    Fixture f;
    auto mvcc = f.mvcc();
    ASSERT_TRUE(mvcc.prewrite(put("k", "committed"), "k", Ts{5}, 0, 0).has_value());
    ASSERT_TRUE(mvcc.commit(std::vector<std::string>{"k"}, Ts{5}, Ts{10}).has_value());
    // 一个未提交的活跃事务（start_ts=50 > safe_point）
    ASSERT_TRUE(mvcc.prewrite(put("k2", "pending"), "k2", Ts{50}, 3000, 0).has_value());

    auto gc = mvcc.gc(Ts{30});
    ASSERT_TRUE(gc.has_value());
    // 活跃事务的锁与数据必须保留（GC 不得破坏未提交事务）
    auto pending = mvcc.get("k2", Ts{50});
    ASSERT_TRUE(pending.has_value());
    ASSERT_TRUE(pending->has_value());
    EXPECT_EQ(**pending, "pending");
    // 已提交值仍可读
    auto got = mvcc.get("k", Ts{60});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(**got, "committed");
}

}  // namespace
