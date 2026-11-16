// 文件用途：WAL 单元测试——追加/重放/坏记录即停/绝不改写文件（SPEC-1 §1.4.2、REL-02）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：WAL 是 crash-safe 的唯一凭据。测试要证明三件事：
//   (a) 写入的批次能按原序原样重放（含墓碑）；
//   (b) 尾部半条记录或 CRC 损坏时，只丢弃坏记录及其之后，之前的数据仍可用；
//   (c) 重放**不改写**段文件——参考项目"构造即 truncate"的教训，这里用文件大小做断言。

#include "storage/wal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/error.h"
#include "temp_dir.h"

namespace {

namespace fs = std::filesystem;
using tinykv::Cf;
using tinykv::Delete;
using tinykv::Modify;
using tinykv::ModifyVec;
using tinykv::Put;
using tinykv::Wal;
using tinykv::WalOptions;
using tinykv::testlib::TempDir;

ModifyVec batch_put(std::string key, std::string value) {
    return ModifyVec{Modify{Put{.cf = Cf::Default, .key = std::move(key), .value = std::move(value)}}};
}

ModifyVec batch_delete(std::string key) {
    return ModifyVec{Modify{Delete{.cf = Cf::Write, .key = std::move(key)}}};
}

std::uintmax_t file_size_of_largest(const fs::path& dir) {
    std::uintmax_t biggest = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file()) {
            biggest = std::max(biggest, e.file_size());
        }
    }
    return biggest;
}

// 单段场景下取那唯一的段文件；无段返回空路径（调用方断言非空）
fs::path sole_segment(const fs::path& dir) {
    fs::path found;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file()) {
            found = e.path();
        }
    }
    return found;
}

// 两个用例共用的前置：同目录写入两条记录（用于制造 CRC 损坏 / 尾部截断）
void write_two_batches(const fs::path& dir) {
    auto wal = Wal::open(WalOptions{.dir = dir});
    ASSERT_TRUE(wal.has_value());
    ASSERT_TRUE((*wal)->append(batch_put("k1", "v1")).has_value());
    ASSERT_TRUE((*wal)->append(batch_put("k2", "v2")).has_value());
}

TEST(Wal, AppendThenReplayRoundtrip) {
    TempDir tmp("tinykv_wal_rt");
    const fs::path dir = tmp.path() / "wal";

    {
        auto wal = Wal::open(WalOptions{.dir = dir});
        ASSERT_TRUE(wal.has_value()) << wal.error().index();
        ASSERT_TRUE((*wal)->append(batch_put("k1", "v1")).has_value());
        ASSERT_TRUE((*wal)->append(batch_delete("k2")).has_value());
    }

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    ASSERT_FALSE(replay->stopped);
    ASSERT_EQ(replay->batches.size(), 2U);

    const auto& first = replay->batches[0];
    ASSERT_EQ(first.size(), 1U);
    const auto* put = std::get_if<Put>(&first[0].op);
    ASSERT_NE(put, nullptr);
    EXPECT_EQ(put->cf, Cf::Default);
    EXPECT_EQ(put->key, "k1");
    EXPECT_EQ(put->value, "v1");

    const auto* del = std::get_if<Delete>(&replay->batches[1][0].op);
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->cf, Cf::Write);
    EXPECT_EQ(del->key, "k2");
}

TEST(Wal, ReopenContinuesSameSegment) {
    // 重开不应制造新段：段切换只由体积阈值触发
    TempDir tmp("tinykv_wal_reopen");
    const fs::path dir = tmp.path() / "wal";

    {
        auto wal = Wal::open(WalOptions{.dir = dir});
        ASSERT_TRUE(wal.has_value());
        ASSERT_TRUE((*wal)->append(batch_put("a", "1")).has_value());
    }
    {
        auto wal = Wal::open(WalOptions{.dir = dir});
        ASSERT_TRUE(wal.has_value());
        ASSERT_TRUE((*wal)->append(batch_put("b", "2")).has_value());
    }

    std::size_t segment_count = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file()) {
            ++segment_count;
        }
    }
    EXPECT_EQ(segment_count, 1U);

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->batches.size(), 2U);
}

TEST(Wal, CorruptCrcStopsReplayButKeepsPrefix) {
    TempDir tmp("tinykv_wal_crc");
    const fs::path dir = tmp.path() / "wal";

    write_two_batches(dir);

    // 翻转文件最后一个字节（第二条记录载荷内）→ CRC 必不匹配
    const fs::path segment = sole_segment(dir);
    ASSERT_FALSE(segment.empty());
    {
        std::fstream f(segment, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekg(-1, std::ios::end);
        char last = 0;
        f.read(&last, 1);
        last = static_cast<char>(last ^ 0x5A);
        f.seekp(-1, std::ios::end);
        f.write(&last, 1);
    }

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_TRUE(replay->stopped);
    ASSERT_EQ(replay->batches.size(), 1U);  // 第一条完好
    EXPECT_EQ(replay->reason, "CRC32C 不匹配");
}

TEST(Wal, TruncatedTailStopsReplayWithoutRewritingFile) {
    TempDir tmp("tinykv_wal_trunc");
    const fs::path dir = tmp.path() / "wal";

    write_two_batches(dir);

    const fs::path segment = sole_segment(dir);
    ASSERT_FALSE(segment.empty());
    const auto size_before = fs::file_size(segment);
    fs::resize_file(segment, size_before - 3);   // 模拟掉电写了半条
    const auto size_after_truncate = fs::file_size(segment);

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_TRUE(replay->stopped);
    ASSERT_EQ(replay->batches.size(), 1U);

    // 红线：重放不得改写文件（参考项目构造即清空的教训）
    EXPECT_EQ(fs::file_size(segment), size_after_truncate);
}

TEST(Wal, ClearAllRemovesSegments) {
    TempDir tmp("tinykv_wal_clear");
    const fs::path dir = tmp.path() / "wal";

    {
        auto wal = Wal::open(WalOptions{.dir = dir});
        ASSERT_TRUE(wal.has_value());
        ASSERT_TRUE((*wal)->append(batch_put("k", "v")).has_value());
    }
    EXPECT_GT(file_size_of_largest(dir), 0U);

    ASSERT_TRUE(Wal::clear_all(dir).has_value());
    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_TRUE(replay->batches.empty());
}

TEST(Wal, RotationBySegmentSizeProducesOrderedSegments) {
    TempDir tmp("tinykv_wal_rot");
    const fs::path dir = tmp.path() / "wal";
    {
        // 段上限压到 20 字节：单条记录约 17 字节（8 头 + 9 载荷）→ 每条各占一段
        auto wal = Wal::open(WalOptions{.dir = dir, .segment_bytes = 20});
        ASSERT_TRUE(wal.has_value());
        ASSERT_TRUE((*wal)->append(batch_put("k1", "v1")).has_value());
        ASSERT_TRUE((*wal)->append(batch_put("k2", "v2")).has_value());
        ASSERT_TRUE((*wal)->append(batch_put("k3", "v3")).has_value());
    }

    std::size_t segments = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file()) {
            ++segments;
        }
    }
    EXPECT_GE(segments, 2U);

    auto replay = Wal::replay(dir);
    ASSERT_TRUE(replay.has_value());
    EXPECT_FALSE(replay->stopped);
    ASSERT_EQ(replay->batches.size(), 3U);
    // 段按编号重放 → 顺序与写入顺序一致
    EXPECT_EQ(std::get<Put>(replay->batches[0][0].op).key, "k1");
    EXPECT_EQ(std::get<Put>(replay->batches[2][0].op).key, "k3");
}

}  // namespace
