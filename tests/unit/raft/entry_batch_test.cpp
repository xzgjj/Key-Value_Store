// 文件用途：Envelope 编解码单测——批量 propose 的内部格式往返、向后兼容、坏数据拒绝（M4 第①层）。
// 归属：TinyKV-CPP（M4 Multi-Raft 批处理基础设施）。
// 使用场景理解：Envelope 是 Entry.data 的内部打包格式（不改 proto）。它必须：
//   (a) 多条 data 打包后能原样、按序解出（回执对齐的前提）；
//   (b) 旧的单条 protobuf data 不被误判为 Envelope（向后兼容崩溃重放）；
//   (c) 损坏的长度/截断不被解成半条（否则 apply 会执行错乱命令）。

#include "raft/entry_batch.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using tinykv::raft::pack_entries;
using tinykv::raft::try_unpack_entries;

TEST(EntryBatch, RoundTripPreservesOrderAndBytes) {
    std::vector<std::string> in{"alpha", "b\0eta", "\x0a\x01\xff", ""};
    const auto packed = pack_entries(in);
    std::vector<std::string> out;
    ASSERT_TRUE(try_unpack_entries(packed, out));
    ASSERT_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(out[i], in[i]) << "第 " << i << " 条不一致";
    }
}

TEST(EntryBatch, SingleItemStillUsesEnvelope) {
    const auto packed = pack_entries({"only"});
    std::vector<std::string> out;
    ASSERT_TRUE(try_unpack_entries(packed, out));
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0], "only");
}

TEST(EntryBatch, LegacyProtobufDataIsNotMistakenForEnvelope) {
    // protobuf RaftCmdRequest 首字节是字段 tag 0x0a；不能被当成 magic
    const std::string legacy("\x0a\x05hello", 7);
    std::vector<std::string> out;
    EXPECT_FALSE(try_unpack_entries(legacy, out));
}

TEST(EntryBatch, TruncatedEnvelopeIsRejected) {
    const auto packed = pack_entries({"one", "two"});
    std::vector<std::string> out;
    // 截掉尾部，使最后一条长度声明越界
    EXPECT_FALSE(try_unpack_entries(packed.substr(0, packed.size() - 2), out));
}

TEST(EntryBatch, WrongMagicIsRejected) {
    std::string bad = pack_entries({"x"});
    bad[0] = static_cast<char>(bad[0] ^ 0xFFU);   // 破坏 magic
    std::vector<std::string> out;
    EXPECT_FALSE(try_unpack_entries(bad, out));
}

}  // namespace
