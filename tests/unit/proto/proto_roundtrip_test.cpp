// 文件用途：proto 生成装配冒烟测试——契约文件可编译可序列化（SPEC-0 §0.1 交付物证据）。
// 归属：TinyKV-CPP（M0 基建）。
// 使用场景理解：proto 是接口文档即代码；本测试只验证生成代码的序列化回环与 oneof
// 可空性语义，不验证业务规则（业务规则在对应里程碑的行为测试中覆盖）。

#include <gtest/gtest.h>

#include <raft_cmdpb.pb.h>
#include <raft.pb.h>

#include <string>

namespace {

using namespace tinykv::raftpb;  // NOLINT(google-build-using-namespace) —— 测试内便捷引用

TEST(ProtoRoundtrip, RaftMessageRoundtrip) {
    // MsgAppend：七字段复用集合的字段回环（term/index/log_term/commit/reject/entries）
    Message m;
    m.set_id(MSG_APPEND);
    m.set_term(3);
    m.set_from(11);
    m.set_to(12);
    m.set_index(5);       // prev_log_index 语义
    m.set_log_term(2);    // prev_log_term 语义
    m.set_commit(4);
    m.add_entries()->set_index(5);
    m.mutable_entries(0)->set_term(2);
    m.mutable_entries(0)->set_data("cmd-1");

    std::string wire;
    ASSERT_TRUE(m.SerializeToString(&wire));
    EXPECT_FALSE(wire.empty());

    Message back;
    ASSERT_TRUE(back.ParseFromString(wire));
    EXPECT_EQ(back.id(), MSG_APPEND);
    EXPECT_EQ(back.term(), 3);
    EXPECT_EQ(back.index(), 5);
    EXPECT_EQ(back.log_term(), 2);
    EXPECT_EQ(back.commit(), 4);
    EXPECT_EQ(back.entries_size(), 1);
    EXPECT_EQ(back.entries(0).data(), "cmd-1");
}

TEST(ProtoRoundtrip, EmptyEntriesAllowedInMessage) {
    // 红线：Entry.data 可空（空提案合法）；Heartbeat 无 entries（PERF-08 元数据-only）
    Message hb;
    hb.set_id(MSG_HEARTBEAT);
    hb.set_term(7);
    hb.set_commit(9);
    std::string wire;
    ASSERT_TRUE(hb.SerializeToString(&wire));
    Message back;
    ASSERT_TRUE(back.ParseFromString(wire));
    EXPECT_EQ(back.entries_size(), 0);
    EXPECT_FALSE(back.has_snapshot());
}

TEST(ProtoRoundtrip, RegionErrorNotLeaderHintOptionality) {
    // UX-03：leader_hint 未知的 NotLeader 不得伪造——proto 侧用 not set 表达
    using tinykv::cmdpb::RegionError;
    RegionError err;
    auto *nl = err.mutable_not_leader();
    nl->set_region_id(3);  // 不设置 leader_hint

    std::string wire;
    ASSERT_TRUE(err.SerializeToString(&wire));
    RegionError back;
    ASSERT_TRUE(back.ParseFromString(wire));
    ASSERT_TRUE(back.has_not_leader());
    EXPECT_EQ(back.not_leader().region_id(), 3);
    EXPECT_FALSE(back.not_leader().has_leader_hint());
}

TEST(ProtoRoundtrip, RaftCmdRequestBatchShape) {
    // 契约关键：requests[] 批量一等公民（白皮书 QPS 0 分教训的反向锁定）
    tinykv::cmdpb::RaftCmdRequest cmd;
    auto *put1 = cmd.add_requests()->mutable_put();
    put1->set_key("k1");
    put1->set_value("v1");
    auto *put2 = cmd.add_requests()->mutable_put();
    put2->set_key("k2");
    put2->set_value("");

    std::string wire;
    ASSERT_TRUE(cmd.SerializeToString(&wire));
    tinykv::cmdpb::RaftCmdRequest back;
    ASSERT_TRUE(back.ParseFromString(wire));
    EXPECT_EQ(back.requests_size(), 2);
    EXPECT_EQ(back.requests(0).put().key(), "k1");
    EXPECT_EQ(back.requests(1).put().key(), "k2");
}

}  // namespace
