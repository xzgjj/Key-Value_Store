// 文件用途：Raft 日志单元测试——dummy entry、冲突截断、压缩、重启保持（SPEC-2 §2.3.1 四指针）。
// 归属：TinyKV-CPP（M2 Raft 共识）。
// 使用场景理解：日志是 Raft 正确性的地基。三条最容易被写错的语义在这里钉死：
//   (a) index 0 是 dummy，不是"没有条目"的另一种说法；
//   (b) 冲突回退必须能删掉 idx 之后的全部条目（否则旧 term 的尾巴会被当成本 term 复制出去）；
//   (c) 压缩后 first_index 前移、dummy_term 变成被压缩最后一条的任期（prevLogTerm 判定依赖它）。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::testlib::make_entry;

// 每个用例独立的引擎实例（raft log 架在引擎上，故需要真实存储）
struct Fixture {
    TempDir tmp{"tinykv_raftlog"};
    std::unique_ptr<tinykv::Engine> engine;
    std::unique_ptr<RaftLog> log;

    Fixture() {
        auto opened = tinykv::Engine::open({.dir = tmp.path() / "db"});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
        auto log_open = RaftLog::open(*engine);
        EXPECT_TRUE(log_open.has_value());
        log = std::move(*log_open);
    }
};

TEST(RaftLog, FreshLogHasDummyAtZero) {
    Fixture f;
    EXPECT_EQ(f.log->first_index(), LogIndex{1});
    EXPECT_EQ(f.log->last_index(), LogIndex{0});
    EXPECT_EQ(f.log->committed(), LogIndex{0});
    auto dummy = f.log->term(LogIndex{0});
    ASSERT_TRUE(dummy.has_value());
    EXPECT_EQ(*dummy, Term{0});
}

TEST(RaftLog, AppendAdvancesLastIndexAndKeepsTerms) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b"), make_entry(3, 2, "c")};
    ASSERT_TRUE(f.log->append(entries).has_value());

    EXPECT_EQ(f.log->last_index(), LogIndex{3});
    auto t2 = f.log->term(LogIndex{2});
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ(*t2, Term{1});
    auto t3 = f.log->term(LogIndex{3});
    ASSERT_TRUE(t3.has_value());
    EXPECT_EQ(*t3, Term{2});
}

TEST(RaftLog, AppendMustBeContiguous) {
    Fixture f;
    std::vector<Entry> gap{make_entry(2, 1, "x")};   // 期望从 index 1 开始
    auto r = f.log->append(gap);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
}

TEST(RaftLog, TruncateFromRemovesTail) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b"), make_entry(3, 1, "c")};
    ASSERT_TRUE(f.log->append(entries).has_value());

    ASSERT_TRUE(f.log->truncate_from(LogIndex{2}).has_value());
    EXPECT_EQ(f.log->last_index(), LogIndex{1});
    // 被截掉的条目必须真的不存在，否则旧 term 的尾巴会污染后续复制
    auto gone = f.log->at(LogIndex{2});
    EXPECT_FALSE(gone.has_value());
}

TEST(RaftLog, TruncateRefusesCommitted) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b")};
    ASSERT_TRUE(f.log->append(entries).has_value());
    ASSERT_TRUE(f.log->set_committed(LogIndex{2}).has_value());

    auto r = f.log->truncate_from(LogIndex{2});
    ASSERT_FALSE(r.has_value());   // 已提交条目不可截断
}

TEST(RaftLog, CompactMovesFirstIndexAndDummyTerm) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 3, "b"), make_entry(3, 3, "c")};
    ASSERT_TRUE(f.log->append(entries).has_value());
    ASSERT_TRUE(f.log->set_committed(LogIndex{3}).has_value());

    ASSERT_TRUE(f.log->compact(LogIndex{2}).has_value());
    EXPECT_EQ(f.log->first_index(), LogIndex{3});
    // dummy term 取被压缩的最后一条（term 3），prevLogTerm 判定靠它
    auto dummy = f.log->term(LogIndex{0});
    ASSERT_TRUE(dummy.has_value());
    EXPECT_EQ(*dummy, Term{3});
    // 被压缩的 index 再查任期必须报错而不是返回错值
    auto old = f.log->term(LogIndex{2});
    ASSERT_FALSE(old.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(old.error()));
}

TEST(RaftLog, CompactCannotPassCommitted) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b")};
    ASSERT_TRUE(f.log->append(entries).has_value());
    auto r = f.log->compact(LogIndex{2});   // committed 仍为 0
    ASSERT_FALSE(r.has_value());
}

TEST(RaftLog, StateSurvivesReopen) {
    TempDir tmp("tinykv_raftlog_reopen");
    const auto dir = tmp.path() / "db";
    {
        auto engine = tinykv::Engine::open({.dir = dir});
        ASSERT_TRUE(engine.has_value());
        auto log = RaftLog::open(**engine);
        ASSERT_TRUE(log.has_value());
        std::vector<Entry> entries{make_entry(1, 4, "a"), make_entry(2, 4, "tail")};
        ASSERT_TRUE((*log)->append(entries).has_value());
        ASSERT_TRUE((*log)->set_hard_state(Term{4}, PeerId{7}).has_value());
        ASSERT_TRUE((*log)->set_committed(LogIndex{1}).has_value());
        ASSERT_TRUE((*engine)->close().has_value());
    }

    auto engine = tinykv::Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    auto log = RaftLog::open(**engine);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ((*log)->last_index(), LogIndex{2});
    EXPECT_EQ((*log)->committed(), LogIndex{1});
    EXPECT_EQ((*log)->current_term(), Term{4});
    ASSERT_TRUE((*log)->voted_for().has_value());
    EXPECT_EQ(*(*log)->voted_for(), PeerId{7});
    auto entry = (*log)->at(LogIndex{1});
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->data(), "a");
}

}  // namespace
