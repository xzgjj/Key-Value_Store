// 文件用途：SST 合并回归——表数过阈值必须合并，重开后数据一致且墓碑不复活。
// 归属：TinyKV-CPP（M7 ② 日志压缩阈值 + SST 合并）。
// 使用场景理解：没有合并时每次 flush 只增表，长跑会无限累积文件与打开句柄；
// 合并若把旧墓碑丢错，被删的键会在重开后复活——这是最隐蔽的错误形态。

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "storage/engine.h"
#include "storage_test_base.h"

namespace {

using tinykv::Cf;
using tinykv::Engine;
using tinykv::EngineOptions;
using tinykv::GetResult;
using tinykv::Modify;
using tinykv::Put;
using tinykv::Delete;
using tinykv::testlib::bytes;
using tinykv::testlib::TempDir;

std::string key_of(int i) { return "k" + std::to_string(100 + i); }
std::string value_of(int i) { return "v" + std::to_string(100 + i); }

TEST(EngineCompaction, SstCountStaysBoundedAndDataSurvivesReopen) {
    TempDir tmp("tinykv_sst_compact");
    const auto dir = tmp.path() / "db";
    {
        auto opened = Engine::open(EngineOptions{.dir = dir,
                                                 .fsync_on_commit = true,
                                                 .memtable_limit_bytes = 96,
                                                 .wal_segment_bytes = 1U << 20U,
                                                 .sst_compact_threshold = 3});
        ASSERT_TRUE(opened.has_value());
        for (int i = 0; i < 12; ++i) {
            tinykv::ModifyVec batch{
                Modify{Put{.cf = Cf::Default, .key = key_of(i), .value = value_of(i)}}};
            ASSERT_TRUE((*opened)->write(batch).has_value()) << i;
        }
        EXPECT_LE((*opened)->sst_count(), 2U) << "表数必须被阈值压住";

        // 墓碑随下一次 flush 落表，随后的全量合并不得让旧值复活
        tinykv::ModifyVec del{Modify{Delete{.cf = Cf::Default, .key = key_of(0)}}};
        ASSERT_TRUE((*opened)->write(del).has_value());
        for (int i = 12; i < 15; ++i) {
            tinykv::ModifyVec batch{
                Modify{Put{.cf = Cf::Default, .key = key_of(i), .value = value_of(i)}}};
            ASSERT_TRUE((*opened)->write(batch).has_value()) << i;
        }
        EXPECT_TRUE((*opened)->close().has_value());
    }

    auto reopened = Engine::open(EngineOptions{.dir = dir,
                                               .fsync_on_commit = true,
                                               .memtable_limit_bytes = 96,
                                               .wal_segment_bytes = 1U << 20U,
                                               .sst_compact_threshold = 3});
    ASSERT_TRUE(reopened.has_value());
    EXPECT_LE((*reopened)->sst_count(), 2U);

    auto gone = (*reopened)->get(Cf::Default, bytes(key_of(0)));
    ASSERT_TRUE(gone.has_value());
    EXPECT_FALSE(gone->value.has_value()) << "被删的键不得复活";
    for (int i = 1; i < 15; ++i) {
        auto got = (*reopened)->get(Cf::Default, bytes(key_of(i)));
        ASSERT_TRUE(got.has_value()) << i;
        ASSERT_TRUE(got->value.has_value()) << i;
        EXPECT_EQ(*got->value, value_of(i)) << i;
    }
}

TEST(EngineCompaction, ThresholdZeroKeepsEveryFlushedTable) {
    TempDir tmp("tinykv_sst_nocompact");
    const auto dir = tmp.path() / "db";
    auto opened = Engine::open(EngineOptions{.dir = dir,
                                             .fsync_on_commit = true,
                                             .memtable_limit_bytes = 40,
                                             .wal_segment_bytes = 1U << 20U,
                                             .sst_compact_threshold = 0});
    ASSERT_TRUE(opened.has_value());
    for (int i = 0; i < 4; ++i) {
        tinykv::ModifyVec batch{
            Modify{Put{.cf = Cf::Default, .key = key_of(i), .value = value_of(i)}}};
        ASSERT_TRUE((*opened)->write(batch).has_value());
    }
    EXPECT_EQ((*opened)->sst_count(), 4U);
}

}  // namespace
