// 文件用途：applied 位点持久化与崩溃重放回归——已提交但未 apply 的条目在重启后必须重放。
// 归属：TinyKV-CPP（M2/M5 正确性，BUG-0005）。
// 使用场景理解：Raft 的 committed 先落盘、状态机 apply 后执行，两者之间天然存在崩溃窗口。
// 若把 committed 当成 applied，窗口内的写就永久丢失；若重放没有幂等保证，补回数据又会
// 破坏 split 元数据与事务可见性。本文件同时钉死“不丢”与“重放不改坏”两侧。

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/error_format.h"
#include "raft_test_base.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"

namespace {

using namespace tinykv;   // NOLINT(google-build-using-namespace)
using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::LogIndex;
using tinykv::raft::Config;
using tinykv::raft::RaftLog;
using tinykv::raft::Ready;
using tinykv::raftstore::Peer;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;

// 单副本装置：引擎目录跨 close/open 存活，用来表达“进程重启但盘还在”
struct PersistPeer {
    TempDir tmp{"tinykv_applied"};
    std::filesystem::path dir{tmp.path() / "db"};
    std::unique_ptr<Engine> engine;
    std::unique_ptr<Peer> peer;
    Config cfg{.id = PeerId{1},
               .voters = {PeerId{1}},
               .election_timeout = 4,
               .heartbeat_interval = 2};

    PersistPeer() { open(); }

    void open() {
        auto opened = Engine::open({.dir = dir});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
        auto p = Peer::open(cfg, *engine);
        EXPECT_TRUE(p.has_value());
        peer = std::move(*p);
        // 崩溃窗口必须建立在"提案已落盘"之上；攒批会让 propose 只入队，故关掉
        peer->enable_batching(false);
        for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
            peer->tick();
        }
        EXPECT_TRUE(peer->is_leader());
    }

    void close() {
        peer.reset();
        if (engine != nullptr) {
            EXPECT_TRUE(engine->close().has_value());
            engine.reset();
        }
    }
};

// 模拟“状态机已 apply 但 applied 标记落盘失败”：直接把持久化位点拨回 0
void rewind_applied(Engine& engine) {
    auto log = RaftLog::open(engine);
    ASSERT_TRUE(log.has_value());
    EXPECT_TRUE((*log)->set_applied(LogIndex{0}).has_value());
}

// BUG-0005: applied 必须与 committed 分开持久化，否则重启无法知道哪些条目尚未 apply
TEST(AppliedRecovery, AppliedPositionSurvivesReopen) {
    TempDir tmp("tinykv_applied_meta");
    const auto dir = tmp.path() / "db";
    {
        auto engine = Engine::open({.dir = dir});
        ASSERT_TRUE(engine.has_value());
        auto log = RaftLog::open(**engine);
        ASSERT_TRUE(log.has_value());
        EXPECT_EQ((*log)->applied(), LogIndex{0});
        ASSERT_TRUE((*log)->set_applied(LogIndex{3}).has_value());
        EXPECT_TRUE((*engine)->close().has_value());
    }

    auto engine = Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    auto log = RaftLog::open(**engine);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ((*log)->applied(), LogIndex{3});
}

// BUG-0005: committed 已落盘而 applied 缺失时，重启后的首个 Ready 必须带出未 apply 条目
TEST(AppliedRecovery, ReopenReplaysCommittedButUnappliedEntries) {
    TempDir tmp("tinykv_applied_replay");
    const auto dir = tmp.path() / "db";
    {
        auto engine = Engine::open({.dir = dir});
        ASSERT_TRUE(engine.has_value());
        auto log = RaftLog::open(**engine);
        ASSERT_TRUE(log.has_value());
        std::vector<Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b")};
        ASSERT_TRUE((*log)->append(entries).has_value());
        ASSERT_TRUE((*log)->set_committed(LogIndex{2}).has_value());
        EXPECT_TRUE((*engine)->close().has_value());
    }

    auto engine = Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    auto log = RaftLog::open(**engine);
    ASSERT_TRUE(log.has_value());
    Config cfg{.id = PeerId{1}, .voters = {PeerId{1}}};
    Raft raft(cfg, **log);
    const Ready ready = raft.take_ready();
    EXPECT_EQ(ready.to_apply.size(), 2U);
}

// BUG-0005: 提案已提交并落盘、还没 on_ready 就崩溃 → 重启后数据必须出现
TEST(AppliedRecovery, PeerReplaysWritesAfterCommitBeforeApplyCrash) {
    PersistPeer f;
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 1, "k1", "v1")).has_value());
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 2, "k2", "v2")).has_value());
    // 故意不调 on_ready：这就是崩溃窗口

    f.close();
    f.open();
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 2U);

    for (const auto& item : {std::pair<const char*, const char*>{"k1", "v1"},
                             std::pair<const char*, const char*>{"k2", "v2"}}) {
        auto got = f.peer->get(Cf::Default, item.first);
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value()) << item.first;
        EXPECT_EQ(*got->value, item.second);
    }
}

// BUG-0005: applied 标记丢失导致整批重放时，Put 的重复执行不得改坏已落盘的值
TEST(AppliedRecovery, ReplayOfAlreadyAppliedWriteLeavesValueUnchanged) {
    PersistPeer f;
    ASSERT_TRUE(f.peer->propose(make_put_request(7, 1, "k", "v1")).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());

    rewind_applied(*f.engine);
    f.close();
    f.open();
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 1U);

    auto got = f.peer->get(Cf::Default, "k");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "v1");
}

// BUG-0005: split admin 重放不得把元数据切成三片或制造区间重叠
TEST(AppliedRecovery, SplitReplayDoesNotDuplicateRegion) {
    PersistPeer f;
    const Region original{.id = RegionId{1},
                          .start_key = "",
                          .end_key = "",
                          .peers = {},
                          .epoch = {1, 0},
                          .size_hint = 0};
    StoreMeta meta;
    ASSERT_TRUE(meta.add_region(original).has_value());
    f.peer->set_region(original);
    f.peer->set_store_meta(&meta);

    cmdpb::RaftCmdRequest split;
    split.mutable_header()->set_client_id(7);
    split.mutable_header()->set_sequence_id(1);
    split.mutable_admin()->mutable_split()->set_split_key("m");
    split.mutable_admin()->mutable_split()->set_new_region_id(2);
    ASSERT_TRUE(f.peer->propose(split).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());
    EXPECT_EQ(meta.region_count(), 2U);

    rewind_applied(*f.engine);
    f.close();
    f.open();
    f.peer->set_region(original);
    f.peer->set_store_meta(&meta);
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value()) << (applied ? "" : tinykv::describe(applied.error()));
    EXPECT_EQ(*applied, 1U);
    EXPECT_EQ(meta.region_count(), 2U);
    auto right = meta.find_region("m");
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(right->id, RegionId{2});
}

// BUG-0005: 事务 prewrite+commit 整批重放后，已提交版本必须仍可读
TEST(AppliedRecovery, TxnReplayLeavesCommittedValueUnchanged) {
    PersistPeer f;
    kvpb::TxnPrewriteRequest pre;
    pre.set_primary_lock("tk");
    pre.set_start_ts(10);
    pre.set_lock_ttl(3000);
    auto* mutation = pre.add_mutations();
    mutation->set_key("tk");
    mutation->set_value("tv");
    mutation->set_op(kvpb::Mutation::PUT);
    ASSERT_TRUE(f.peer->propose_txn_prewrite(pre));
    ASSERT_TRUE(f.peer->on_ready().has_value());

    kvpb::TxnCommitRequest commit;
    commit.add_keys("tk");
    commit.set_start_ts(10);
    commit.set_commit_ts(20);
    ASSERT_TRUE(f.peer->propose_txn_commit(commit));
    ASSERT_TRUE(f.peer->on_ready().has_value());

    auto before = f.peer->mvcc().get("tk", Ts{1000});
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(before->has_value());
    EXPECT_EQ(**before, "tv");

    rewind_applied(*f.engine);
    f.close();
    f.open();
    auto applied = f.peer->on_ready();
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(*applied, 2U);

    auto after = f.peer->mvcc().get("tk", Ts{1000});
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->has_value());
    EXPECT_EQ(**after, "tv");
}

// 阈值压缩只切掉已 apply 且全部 voter 已 match 的前缀，重开后新日志仍能接续提案
TEST(AppliedRecovery, ThresholdCompactionKeepsReplaySafe) {
    PersistPeer f;
    f.peer->set_log_compact_threshold(3);
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(f.peer->propose(make_put_request(9, static_cast<std::uint64_t>(i) + 1,
                                                     "ck" + std::to_string(i),
                                                     "cv" + std::to_string(i)))
                        .has_value());
    }
    ASSERT_TRUE(f.peer->on_ready().has_value());

    auto log = RaftLog::open(*f.engine);
    ASSERT_TRUE(log.has_value());
    EXPECT_GT((*log)->first_index().value, 1U);
    EXPECT_EQ((*log)->applied(), LogIndex{8});

    f.close();
    f.open();
    f.peer->set_log_compact_threshold(3);
    ASSERT_TRUE(f.peer->propose(make_put_request(9, 9, "ck8", "cv8")).has_value());
    ASSERT_TRUE(f.peer->on_ready().has_value());
    auto got = f.peer->get(Cf::Default, "ck8");
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "cv8");
}

}  // namespace
