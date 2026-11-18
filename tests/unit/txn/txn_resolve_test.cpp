// 文件用途：锁等待+决议时序（M5 第④层，FUNC-14，SPEC-4 §3.7 txn_resolve_test）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：SPEC 样例——事务 A prewrite(k) 后"崩溃"（Lock 残留），事务 B 读 →
// TTL 未过期时被阻塞、过期后 CheckTxnStatus 决议 → BatchRollback 清 A 锁 → B 读旧值、
// 自身 prewrite 成功。断言：A 无"半提交"状态；B 全程无死锁、无脏读。
// 时钟全部注入（now_ms），不需要真实等待 3s——这也是 ENG-04 禁 sleep 的解法。

#include "txn/client.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>

#include "txn_fixture.h"

namespace {

using Fixture = tinykv::testlib::TxnFixture;
using tinykv::Ts;
using tinykv::txn::TxnClient;
using tinykv::txn::Mutation;


// 主线样例：A 崩溃留锁 → B 等 TTL → 决议回滚 → B 读旧值 + 自身写成功
TEST(TxnResolve, CrashedTxnResolvedAfterTtlExpiry) {
    Fixture f;
    // 先有旧版本 "old"（已提交）
    {
        auto c = f.client();
        (void)c.begin();
        (void)c.set("k", "old");
        f.advance(10);
        ASSERT_TRUE(c.commit(f.now_ms).has_value());
    }

    // 事务 A：prewrite(k,"new") 后"崩溃"——不 commit 不 rollback，锁残留
    auto a = f.client();
    (void)a.begin();
    (void)a.set("k", "new");
    f.advance(10);
    {
        // 直接走 MvccTxn 层模拟崩溃点：prewrite 已落盘、后续编排永不发生
        tinykv::txn::MvccTxn mvcc(*f.engine);
        std::vector<tinykv::txn::Mutation> muts{
            {.op = tinykv::txn::Mutation::Op::kPut, .key = "k", .value = "new"}};
        ASSERT_TRUE(mvcc.prewrite(muts, "k", a.start_ts(), 3000, f.now_ms).has_value());
    }
    // 注意：A 的 client 层并未 commit——锁以 start_ts=a 残留在 Lock CF

    // 事务 B：get(k) → TTL 未过期时被阻塞（不等待——用注入时钟推进判定）
    auto b = f.client();
    (void)b.begin();
    {
        // 现在时刻还在 A 的 TTL 内 → get 报 blocked
        tinykv::txn::MvccTxn mvcc(*f.engine);
        auto r = mvcc.get("k", b.start_ts());
        ASSERT_FALSE(r.has_value());
        ASSERT_TRUE(std::holds_alternative<tinykv::LockConflict>(r.error()));
    }
    {
        auto probe = f.client();
        (void)probe.begin();
        auto r = probe.get("k", f.now_ms);   // now_ms 仍在 TTL 内
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->blocked) << "TTL 未过期时应阻塞等待";
    }

    // 时间推进越过 TTL → B 的决议编排清 A 锁 → 读到旧值
    f.advance(3001);
    auto got = b.get("k", f.now_ms);
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->blocked) << "TTL 过期后决议应完成，不再阻塞";
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "old") << "A 已回滚：B 读到旧值（无脏读、无半提交）";

    // A 无半提交态：决议后 k 的锁为墓碑/不存在，且 A 的未提交数据已清
    {
        tinykv::txn::MvccTxn mvcc(*f.engine);
        auto st = mvcc.check_txn_status("k", a.start_ts(), f.now_ms);
        ASSERT_TRUE(st.has_value());
        EXPECT_TRUE(st->rolled_back) << "A 被决议为回滚，不存在半提交";
    }

    // B 自身 prewrite 成功（锁已清）
    (void)b.set("k", "from-b");
    f.advance(10);
    auto commit = b.commit(f.now_ms);
    ASSERT_TRUE(commit.has_value()) << "决议清锁后 B 可正常提交";
}

// 正向推进：primary 已提交（崩溃发生在 commit primary 之后）→ 残留 secondary 锁被补提交
TEST(TxnResolve, ForwardProgressWhenPrimaryCommitted) {
    Fixture f;
    // A prewrite p+s 后只提交 primary（模拟崩溃在 primary 提交后、secondary 提交前）
    tinykv::txn::MvccTxn mvcc(*f.engine);
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "p", .value = "pv"},
        {.op = tinykv::txn::Mutation::Op::kPut, .key = "s", .value = "sv"}};
    const auto a_start = f.tso.get_timestamps(1)->front();   // 真实分配（防 begin 撞号）
    f.advance(10);
    ASSERT_TRUE(mvcc.prewrite(muts, "p", a_start, 3000, f.now_ms).has_value());
    const auto commit_ts = f.tso.get_timestamps(1)->front();
    ASSERT_GT(commit_ts.value, a_start.value) << "TSO 授时必须严格晚于事务 start_ts";
    ASSERT_TRUE(mvcc.commit(std::vector<std::string>{"p"}, a_start, commit_ts).has_value());

    // B 在 A 的 primary 提交之后开始（start_ts > commit_ts）：读 secondary 遇残留锁
    auto b = f.client();
    (void)b.begin();
    ASSERT_GT(b.start_ts().value, commit_ts.value);
    f.advance(4000);
    auto got = b.get("s", f.now_ms);
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->blocked);
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "sv") << "primary 已提交 → secondary 必须正向推进可见";
    // secondary 的锁已被清：状态判 committed
    {
        auto st = mvcc.check_txn_status("s", a_start, f.now_ms);
        ASSERT_TRUE(st.has_value());
        EXPECT_TRUE(st->committed);
        EXPECT_EQ(st->commit_ts.value, commit_ts.value);
    }
}

}  // namespace
