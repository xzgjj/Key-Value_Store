// 文件用途：SI 语义冲突矩阵（M5 第④层，FUNC-13/REL-06，SPEC-4 §3.7 si_semantics_test）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：冲突矩阵全分支——同 key 写写/不同 key 并发/读-写（可重复读）/写-读（无脏读）/
// 事务内自读（读己之写）/冲突后无中间态。客户端用 TxnClient（④层决议编排在 get 内），
// 时钟注入 now_ms 保证可复现。两条 REL-06 语义：无脏写（冲突方不留任何中间态）、
// 首提交者胜（后提交者冲突 abort）。

#include "txn/client.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "txn_fixture.h"

namespace {

using tinykv::testlib::TempDir;
using Fixture = tinykv::testlib::TxnFixture;
using tinykv::Ts;


// 同 key 写写冲突：t2 后提交胜，t1 commit → WriteConflict；再读 = b（无中间态）
TEST(SiSemantics, SameKeyWriteWriteConflict) {
    Fixture f;
    auto [t1, t2] = f.begin_two("k", "a", "k", "b");
    ASSERT_TRUE(t2.commit(f.now_ms).has_value()) << "t2 首提交应胜出";

    f.now_ms += 10;
    auto r = t1.commit(f.now_ms);
    ASSERT_FALSE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tinykv::WriteConflict>(r.error()));
    EXPECT_GT(std::get<tinykv::WriteConflict>(r.error()).conflict_commit_ts, t1.start_ts().value);

    // SI：无脏写——t1 未留下任何中间态，读到的仍是 t2 的值
    f.now_ms += 10;
    auto probe = f.client();
    (void)probe.begin();
    auto got = probe.get("k", f.now_ms);
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "b");
}

// 不同 key 并发写：两个事务都提交成功（无假冲突）
TEST(SiSemantics, DifferentKeysConcurrentWritesCommit) {
    Fixture f;
    auto t1 = f.client();
    auto t2 = f.client();
    // 交错 begin/set（注意 begin 会清空 writes_，set 必须在各自 begin 之后）
    (void)t1.begin();
    (void)t1.set("a", "1");
    (void)t2.begin();
    (void)t2.set("b", "2");
    f.now_ms += 10;
    EXPECT_TRUE(t1.commit(f.now_ms).has_value());
    f.now_ms += 10;
    EXPECT_TRUE(t2.commit(f.now_ms).has_value());
}

// 读-写：t1 快照读后他人更新提交 → t1 再读仍旧值（可重复读）
TEST(SiSemantics, RepeatableReadAcrossOthersCommit) {
    Fixture f;
    f.commit_external("k", "v0");
    auto t1 = f.client();
    (void)t1.begin();
    f.now_ms += 10;
    auto first = t1.get("k", f.now_ms);
    ASSERT_TRUE(first.has_value() && first->value.has_value());
    EXPECT_EQ(*first->value, "v0");

    f.commit_external("k", "v1");   // 他人更新并提交
    f.now_ms += 10;
    auto second = t1.get("k", f.now_ms);
    ASSERT_TRUE(second.has_value() && second->value.has_value());
    EXPECT_EQ(*second->value, "v0") << "SI 快照读必须可重复";
}

// 写-读：t1 写未提交（prewrite 后崩溃点）→ t2 读看不到（SI 无脏读）
TEST(SiSemantics, NoDirtyReadOfUncommittedWrite) {
    Fixture f;
    f.commit_external("k", "old");
    // t1 走 MvccTxn 层 prewrite 后"停住"（未 commit）——未提交数据已落 Default CF
    tinykv::txn::MvccTxn mvcc(*f.engine);
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "k", .value = "dirty"}};
    f.now_ms += 10;
    const auto t1_start = f.tso.get_timestamps(1)->front();   // 真实分配（占号不消费会被 begin 撞号）
    ASSERT_TRUE(mvcc.prewrite(muts, "k", t1_start, 3000, f.now_ms).has_value());

    auto t2 = f.client();
    (void)t2.begin();
    f.now_ms += 10;
    auto seen = t2.get("k", f.now_ms);
    ASSERT_TRUE(seen.has_value());
    EXPECT_TRUE(seen->blocked) << "未过期锁必须阻塞读取";
    EXPECT_FALSE(seen->value.has_value()) << "未提交写入对他人绝不可见（SI 无脏读）";
}

// 事务内自读：t1.set 后 t1.get 读到自己的未提交值（读己之写）
TEST(SiSemantics, ReadOwnWritesInsideTxn) {
    Fixture f;
    auto t1 = f.client();
    (void)t1.begin();
    (void)t1.set("k", "mine");
    f.now_ms += 10;
    auto got = t1.get("k", f.now_ms);
    ASSERT_TRUE(got.has_value() && got->value.has_value());
    EXPECT_EQ(*got->value, "mine");
}

// 冲突 abort（显式 rollback）后无中间态：锁清除、旧值保留
TEST(SiSemantics, AbortedTxnLeavesNoTrace) {
    Fixture f;
    f.commit_external("k", "base");
    // t2 prewrite 后显式回滚（MvccTxn 层真实锁）
    tinykv::txn::MvccTxn mvcc(*f.engine);
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "k", .value = "aborted"}};
    f.now_ms += 10;
    const auto t2_start = f.tso.get_timestamps(1)->front();
    ASSERT_TRUE(mvcc.prewrite(muts, "k", t2_start, 3000, f.now_ms).has_value());
    ASSERT_TRUE(mvcc.rollback(std::vector<std::string>{"k"}, t2_start).has_value());
    f.now_ms += 10;
    auto probe = f.client();
    (void)probe.begin();
    auto got = probe.get("k", f.now_ms);
    ASSERT_TRUE(got.has_value() && got->value.has_value());
    EXPECT_EQ(*got->value, "base") << "回滚事务不留痕";
}

}  // namespace
