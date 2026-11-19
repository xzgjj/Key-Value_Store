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
    ASSERT_TRUE(f.log->set_applied(LogIndex{3}).has_value());

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

TEST(RaftLog, CompactCannotPassApplied) {
    Fixture f;
    std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b")};
    ASSERT_TRUE(f.log->append(entries).has_value());
    ASSERT_TRUE(f.log->set_committed(LogIndex{2}).has_value());
    // applied 仍为 0：崩溃窗口内只允许重放，不允许把未确认 apply 的条目压掉
    EXPECT_FALSE(f.log->compact(LogIndex{2}).has_value());
    ASSERT_TRUE(f.log->set_applied(LogIndex{2}).has_value());
    EXPECT_TRUE(f.log->compact(LogIndex{2}).has_value());
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

// code-review P0-2 回归夹具：可注入写失败的存储装饰器。
// 生产里写失败来自磁盘满/WAL 失败；用例要能确定性地造出那一刻，才能验证
// "落盘失败时内存不回退、错误能上抛"。
class FlakyStorage : public tinykv::Storage {
public:
    explicit FlakyStorage(tinykv::Storage& inner) : inner_(inner) {}

    [[nodiscard]] tinykv::result<tinykv::GetResult> get(
        Cf cf, tinykv::span<const std::byte> key) override {
        return inner_.get(cf, key);
    }
    [[nodiscard]] tinykv::result<void> write(tinykv::span<const tinykv::Modify> batch) override {
        if (fail_writes_) {
            return tinykv::unexpected(
                tinykv::IoFailed{.path = "flaky", .reason = "注入写失败"});
        }
        return inner_.write(batch);
    }
    [[nodiscard]] tinykv::result<std::unique_ptr<tinykv::Reader>> create_reader() override {
        return inner_.create_reader();
    }

    bool fail_writes_{false};

private:
    tinykv::Storage& inner_;
};

// raft log 的 entry 键：[0x00]re + be64(index)
std::string entry_key_of(std::uint64_t index) {
    std::string key("\0re", 3);
    for (int shift = 56; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((index >> shift) & 0xFFU));
    }
    return key;
}

// P0-2：set_hard_state 必须"写成功才改内存"。旧实现先改 meta_ 再落盘，
// 磁盘满时内存 term 已前进、盘上没变 —— 重启后 term/vote 回退，可能同任期投两次票。
TEST(RaftLog, HardStateFailureDoesNotAdvanceMemory) {
    Fixture f;
    FlakyStorage flaky(*f.engine);
    auto log = RaftLog::open(flaky);
    ASSERT_TRUE(log.has_value());

    ASSERT_TRUE((*log)->set_hard_state(Term{7}, PeerId{3}).has_value());
    EXPECT_EQ((*log)->current_term(), Term{7});

    flaky.fail_writes_ = true;
    auto failed = (*log)->set_hard_state(Term{8}, std::nullopt);
    EXPECT_FALSE(failed.has_value());
    EXPECT_EQ((*log)->current_term(), Term{7}) << "落盘失败不得推进内存任期";
    ASSERT_TRUE((*log)->voted_for().has_value());
    EXPECT_EQ(*(*log)->voted_for(), PeerId{3});

    flaky.fail_writes_ = false;
    auto reopened = RaftLog::open(flaky);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ((*reopened)->current_term(), Term{7});
}

// P0-2：commit/applied 同样"写成功才改内存"，失败不得让位点前进
TEST(RaftLog, CommitFailureDoesNotAdvanceMemory) {
    Fixture f;
    FlakyStorage flaky(*f.engine);
    auto log = RaftLog::open(flaky);
    ASSERT_TRUE(log.has_value());
    ASSERT_TRUE((*log)->append(std::vector<Entry>{make_entry(1, 1, "a")}).has_value());
    ASSERT_TRUE((*log)->set_committed(LogIndex{1}).has_value());

    flaky.fail_writes_ = true;
    EXPECT_FALSE((*log)->set_applied(LogIndex{1}).has_value());
    EXPECT_EQ((*log)->applied(), LogIndex{0}) << "applied 落盘失败时不得前进";
}

// P0-2 附带：meta blob 长度不符必须报 Corrupted，不能当默认值把 term/vote 归零
TEST(RaftLog, CorruptedMetaBlobRejected) {
    Fixture f;
    std::string bad(10, '\0');
    tinykv::ModifyVec batch{tinykv::Modify{tinykv::Put{
        .cf = tinykv::Cf::Default, .key = std::string("\0lm", 3), .value = std::move(bad)}}};
    ASSERT_TRUE(f.engine->write(batch).has_value());

    auto reopened = RaftLog::open(*f.engine);
    ASSERT_FALSE(reopened.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(reopened.error()));
}

// P0-3：已提交条目读不出来时必须失败并停在原地，绝不能跳过该条推进 applied
TEST(RaftLog, CollectApplyFailsOnMissingCommittedEntry) {
    Fixture f;
    ASSERT_TRUE(f.log->append(std::vector<Entry>{make_entry(1, 1, "a")}).has_value());
    ASSERT_TRUE(f.log->set_committed(LogIndex{1}).has_value());
    // 模拟底层损坏：删掉 entry 1，但 meta 仍说 committed=1
    tinykv::ModifyVec drop{tinykv::Modify{
        tinykv::Delete{.cf = tinykv::Cf::Default, .key = entry_key_of(1)}}};
    ASSERT_TRUE(f.engine->write(drop).has_value());

    Raft raft(tinykv::raft::Config{.id = PeerId{1}, .voters = {PeerId{1}}}, *f.log);
    EXPECT_TRUE(raft.failed()) << "缺失已提交条目必须进入失败态，而不是静默跳过";
    EXPECT_NE(raft.failure().find("恢复待 apply"), std::string::npos);
}

// P0-2：提案落盘失败必须上抛且进入粘性失败态（调用方不能以为"已提案"）
TEST(RaftLog, ProposeFailurePropagatesAndStopsRaft) {
    Fixture f;
    FlakyStorage flaky(*f.engine);
    auto log = RaftLog::open(flaky);
    ASSERT_TRUE(log.has_value());
    Raft raft(tinykv::raft::Config{.id = PeerId{1}, .voters = {PeerId{1}}}, **log);
    ASSERT_FALSE(raft.failed());

    tinykv::raftpb::Message hup;
    hup.set_id(tinykv::raftpb::MSG_HUP);
    ASSERT_TRUE(raft.step(hup).has_value());
    ASSERT_TRUE(raft.is_leader());

    flaky.fail_writes_ = true;
    auto prop = raft.propose("x");
    EXPECT_FALSE(prop.has_value());
    EXPECT_TRUE(raft.failed());
    // 失败态下继续提案也必须被拒（不再产生"假成功"）
    EXPECT_FALSE(raft.propose("y").has_value());
}

}  // namespace
