// 文件用途：MvccTxn 机制用例（M5 第③层，FUNC-13 支撑，SPEC-4 §4.4）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：si_semantics/txn_resolve 证明的是"业务语义对"，这里证明"机械件对"：
// (a) 三列族布局与编码往返；(b) prewrite 的写写冲突/锁冲突拦截与幂等重放；
// (c) commit 的锁归属校验 + 幂等；(d) rollback 墓碑语义 + 不动他者之锁（红线2）；
// (e) check_txn_status 三态（committed / TTL 未过期 / 过期可回滚）；
// (f) 快照读按 commitTs ≤ start_ts 定位（多版本并存）。

#include "txn/mvcc.h"

#include <gtest/gtest.h>

#include <memory>
#include "storage/engine.h"
#include "temp_dir.h"

#include <string>
#include <vector>

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::txn::Mutation;
using tinykv::txn::MvccTxn;
using tinykv::txn::TxnStatus;

struct Fixture {
    TempDir tmp{"tinykv_mvcc"};
    std::unique_ptr<tinykv::Engine> engine;

    Fixture() {
        auto opened = tinykv::Engine::open({.dir = tmp.path() / "db", .fsync_on_commit = false});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
    }

    [[nodiscard]] MvccTxn txn() const { return MvccTxn(*engine); }
};

std::vector<Mutation> put(const std::string& key, const std::string& value) {
    return {Mutation{.op = Mutation::Op::kPut, .key = key, .value = value}};
}

// 直接写 n 个 Write 版本 + 对应 Default 数据（扫描上限用例共用）
void write_versions(tinykv::Engine& engine, std::size_t n, const std::string& key) {
    tinykv::ModifyVec batch;
    batch.reserve(n * 2);
    for (std::size_t i = 1; i <= n; ++i) {
        batch.push_back(tinykv::Modify{tinykv::Put{
            .cf = tinykv::Cf::Write,
            .key = tinykv::txn::write_key(key, tinykv::Ts{i}),
            .value = tinykv::txn::be64_str(i)}});
        batch.push_back(tinykv::Modify{tinykv::Put{
            .cf = tinykv::Cf::Default,
            .key = tinykv::txn::data_key(key, tinykv::Ts{i}),
            .value = "v" + std::to_string(i)}});
    }
    ASSERT_TRUE(engine.write(batch).has_value());
}

TEST(MvccTxnTest, PrewriteCommitReadRoundTrip) {
    Fixture f;
    auto t = f.txn();
    ASSERT_TRUE(t.prewrite(put("k1", "v1"), "k1", tinykv::Ts{100}, 3000, 1000).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k1"}, tinykv::Ts{100}, tinykv::Ts{110}).has_value());

    auto got = t.get("k1", tinykv::Ts{120});
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->has_value());
    EXPECT_EQ(**got, "v1");
    // start_ts < commit_ts 的快照读不到该版本
    auto older = t.get("k1", tinykv::Ts{105});
    ASSERT_TRUE(older.has_value());
    EXPECT_FALSE(older->has_value());
}

TEST(MvccTxnTest, WriteConflictAbortsImmediately) {
    Fixture f;
    auto t = f.txn();
    // t1 先提交 k1
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{110}).has_value());
    // t2（start_ts=105 < 110）prewrite 同 key → WriteConflict（立即 abort，无等待）
    auto r = t.prewrite(put("k", "b"), "k", tinykv::Ts{105}, 3000, 0);
    ASSERT_FALSE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tinykv::WriteConflict>(r.error()));
    const auto& wc = std::get<tinykv::WriteConflict>(r.error());
    EXPECT_EQ(wc.conflict_commit_ts, 110U);
    EXPECT_EQ(wc.start_ts, 105U);
}

TEST(MvccTxnTest, LockConflictOnOthersLock) {
    Fixture f;
    auto t = f.txn();
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    // 另一事务读被锁的 key → LockConflict（携带 primary 供决议）
    auto r = t.prewrite(put("k", "b"), "k", tinykv::Ts{110}, 3000, 0);
    ASSERT_FALSE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<tinykv::LockConflict>(r.error()));
    EXPECT_EQ(std::get<tinykv::LockConflict>(r.error()).primary, "k");

    // 读路径遇他者活跃锁 → LockConflict 错误（等待/决议由上层编排，红线2）
    auto read = t.get("k", tinykv::Ts{120});
    ASSERT_FALSE(read.has_value());
    ASSERT_TRUE(std::holds_alternative<tinykv::LockConflict>(read.error()));
}

TEST(MvccTxnTest, IdempotentPrewriteRetry) {
    Fixture f;
    auto t = f.txn();
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    // 客户端重试同一 prewrite（同 start_ts）：幂等成功，不产生第二把锁/冲突
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{120}).has_value());
    auto got = t.get("k", tinykv::Ts{130});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(**got, "a");
}

TEST(MvccTxnTest, CommitValidatesLockOwnership) {
    Fixture f;
    auto t = f.txn();
    // 无锁的 key 直接 commit → 拒绝（防伪造提交）
    auto r = t.commit(std::vector<std::string>{"ghost"}, tinykv::Ts{100}, tinykv::Ts{110});
    ASSERT_FALSE(r.has_value());
    // commit_ts <= start_ts → 拒绝
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    auto bad = t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{100});
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(bad.error()));
    // 幂等重放：commit 两次，第二次成功（Write 已在）
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{110}).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{110}).has_value());
}

TEST(MvccTxnTest, RollbackLeavesTombstoneAndOldData) {
    Fixture f;
    auto t = f.txn();
    // 先有旧版本 v0（commit_ts=50）
    ASSERT_TRUE(t.prewrite(put("k", "v0"), "k", tinykv::Ts{40}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{40}, tinykv::Ts{50}).has_value());
    // 新事务 prewrite 后回滚
    ASSERT_TRUE(t.prewrite(put("k", "v1"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    ASSERT_TRUE(t.rollback(std::vector<std::string>{"k"}, tinykv::Ts{100}).has_value());
    // 回滚后：读到旧版本 v0（未留下中间态，SI 无脏写）
    auto got = t.get("k", tinykv::Ts{120});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(**got, "v0");
    // 墓碑存在：check_txn_status 判回滚（红线2/3）
    auto st = t.check_txn_status("k", tinykv::Ts{100}, 0);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(st->rolled_back);
    // 回滚后可重新 prewrite（墓碑可覆盖）
    ASSERT_TRUE(t.prewrite(put("k", "v2"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{100}, tinykv::Ts{150}).has_value());
    auto got2 = t.get("k", tinykv::Ts{160});
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(**got2, "v2");
}

TEST(MvccTxnTest, RollbackNeverTouchesOthersLock) {
    Fixture f;
    auto t = f.txn();
    ASSERT_TRUE(t.prewrite(put("k", "a"), "k", tinykv::Ts{100}, 3000, 0).has_value());
    // 他者（start_ts=200）试图回滚本锁 → 不动（红线2）
    ASSERT_TRUE(t.rollback(std::vector<std::string>{"k"}, tinykv::Ts{200}).has_value());
    auto st = t.check_txn_status("k", tinykv::Ts{100}, 100);
    ASSERT_TRUE(st.has_value());
    EXPECT_FALSE(st->rolled_back);
    EXPECT_GT(st->lock_ttl_remaining_ms, 0U);
}

TEST(MvccTxnTest, CheckTxnStatusThreeStates) {
    Fixture f;
    auto t = f.txn();
    // (1) 活跃锁未过期 → remaining > 0
    ASSERT_TRUE(t.prewrite(put("p", "v"), "p", tinykv::Ts{100}, 3000, 1000).has_value());
    auto live = t.check_txn_status("p", tinykv::Ts{100}, 2000);
    ASSERT_TRUE(live.has_value());
    EXPECT_FALSE(live->committed);
    EXPECT_FALSE(live->rolled_back);
    EXPECT_EQ(live->lock_ttl_remaining_ms, 2000U);
    // (2) TTL 过期 → 全零（调用方执行 BatchRollback）
    auto expired = t.check_txn_status("p", tinykv::Ts{100}, 5000);
    ASSERT_TRUE(expired.has_value());
    EXPECT_EQ(expired->lock_ttl_remaining_ms, 0U);
    EXPECT_FALSE(expired->committed);
    EXPECT_FALSE(expired->rolled_back);
    // (3) 已提交 → commit_ts 带回
    ASSERT_TRUE(t.commit(std::vector<std::string>{"p"}, tinykv::Ts{100}, tinykv::Ts{110}).has_value());
    auto done = t.check_txn_status("p", tinykv::Ts{100}, 5000);
    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(done->committed);
    EXPECT_EQ(done->commit_ts.value, 110U);
    // (4) 从未存在 → 判回滚
    auto ghost = t.check_txn_status("ghost", tinykv::Ts{100}, 5000);
    ASSERT_TRUE(ghost.has_value());
    EXPECT_TRUE(ghost->rolled_back);
}

TEST(MvccTxnTest, MultiVersionSnapshotReads) {
    Fixture f;
    auto t = f.txn();
    ASSERT_TRUE(t.prewrite(put("k", "v1"), "k", tinykv::Ts{10}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{10}, tinykv::Ts{20}).has_value());
    ASSERT_TRUE(t.prewrite(put("k", "v2"), "k", tinykv::Ts{30}, 3000, 0).has_value());
    ASSERT_TRUE(t.commit(std::vector<std::string>{"k"}, tinykv::Ts{30}, tinykv::Ts{40}).has_value());

    auto r15 = t.get("k", tinykv::Ts{15});
    auto r35 = t.get("k", tinykv::Ts{35});
    auto r50 = t.get("k", tinykv::Ts{50});
    ASSERT_TRUE(r15.has_value() && r35.has_value() && r50.has_value());
    EXPECT_FALSE(r15->has_value()) << "快照 15 < 首个提交 20，什么都不能看见";
    EXPECT_EQ(**r35, "v1");   // 20 ≤ 35 < 40：只见 v1
    EXPECT_EQ(**r50, "v2");
}

// code-review P0-5：空 value 与 Delete 墓碑共用同一表示，事务路径必须显式拒绝。
// 旧行为：Put("", 空串) 提交后 get 返回"不存在"（快照读语义错误）。
TEST(MvccTxnTest, PrewriteRejectsEmptyValue) {
    Fixture f;
    auto t = f.txn();
    auto r = t.prewrite(put("k", ""), "k", tinykv::Ts{100}, 3000, 0);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
    // 拒绝发生在写入前：锁与数据都不该留下
    auto got = t.get("k", tinykv::Ts{200});
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->has_value());
}

// code-review P0-6：同一 key 版本数超过扫描上限时必须显式报错，不能把第 4096 个
// 版本当成最新版本（静默返回旧数据）。
TEST(MvccTxnTest, WriteVersionScanOverLimitReportsError) {
    Fixture f;
    constexpr std::size_t kVersions = 4097;  // 超过 kScanLimit=4096 一条
    write_versions(*f.engine, kVersions, "hot");

    auto t = f.txn();
    auto got = t.get("hot", tinykv::Ts{kVersions + 10});
    ASSERT_FALSE(got.has_value()) << "截断视图上读出的结果不可信，必须显式失败";
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(got.error()));

    // 写写冲突检测同样不得在截断视图上给结论
    auto pre = t.prewrite(put("hot", "v"), "hot", tinykv::Ts{kVersions + 10}, 3000, 0);
    ASSERT_FALSE(pre.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(pre.error()));
}

// 边界正向用例：恰好 4096 个版本仍可读（上限语义是"超过"才拒绝）
TEST(MvccTxnTest, WriteVersionScanAtLimitStillReads) {
    Fixture f;
    constexpr std::size_t kVersions = 4096;
    write_versions(*f.engine, kVersions, "hot");

    auto t = f.txn();
    auto got = t.get("hot", tinykv::Ts{kVersions + 10});
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->has_value());
    // 最新版本指向的 Default 数据 = 第 4096 个版本的 start_ts
    EXPECT_EQ(**got, "v" + std::to_string(kVersions));
}

}  // namespace
