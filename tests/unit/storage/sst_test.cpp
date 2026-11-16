// 文件用途：SST 读写直测——尾部 CRC 自洽、墓碑保留、乱序拒绝、损坏即拒（SPEC-1 §1.4.4）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：flush 写出的表若 CRC 范围算错，正常写入与读取都看不出问题，只有"重开时校验失败"
//   才暴露——所以必须有一个直接对着 SST 文件断言自洽性的用例，而不是只靠引擎层面的重开冒烟。

#include "storage/sst.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/error.h"
#include "temp_dir.h"

namespace {

namespace fs = std::filesystem;
using tinykv::SstEntry;
using tinykv::SstReader;
using tinykv::SstWriter;
using tinykv::testlib::TempDir;

TEST(Sst, WriteThenReopenIsSelfConsistent) {
    // BUG-0001: SstWriter::finish 先追加条数再算 CRC，导致校验范围少 4 字节，重开必 Corrupted
    // 这条用例直接守 SstWriter::finish 的 CRC 范围：尾部 8 字节不得计入校验
    TempDir tmp("tinykv_sst_rt");
    const fs::path path = tmp.path() / "t.sst";
    {
        auto w = SstWriter::create(path);
        ASSERT_TRUE(w.has_value());
        ASSERT_TRUE((*w)->add("a", std::string("1")).has_value());
        ASSERT_TRUE((*w)->add("b", std::nullopt).has_value());   // 墓碑
        ASSERT_TRUE((*w)->add("c", std::string("3")).has_value());
        ASSERT_TRUE((*w)->finish().has_value());
    }

    auto r = SstReader::open(path);
    ASSERT_TRUE(r.has_value());
    const auto& entries = (*r)->entries();
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].key, "a");
    ASSERT_TRUE(entries[1].value.has_value() == false);
    EXPECT_EQ(entries[2].key, "c");

    ASSERT_NE((*r)->find("c"), nullptr);
    EXPECT_EQ(*(*r)->find("c")->value, "3");
    EXPECT_EQ((*r)->find("absent"), nullptr);
}

TEST(Sst, OutOfOrderKeysRejected) {
    TempDir tmp("tinykv_sst_order");
    auto w = SstWriter::create(tmp.path() / "t.sst");
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE((*w)->add("b", std::string("2")).has_value());
    auto r = (*w)->add("a", std::string("1"));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
}

TEST(Sst, CorruptedByteIsDetected) {
    TempDir tmp("tinykv_sst_corrupt");
    const fs::path path = tmp.path() / "t.sst";
    {
        auto w = SstWriter::create(path);
        ASSERT_TRUE(w.has_value());
        ASSERT_TRUE((*w)->add("k", std::string("v")).has_value());
        ASSERT_TRUE((*w)->finish().has_value());
    }
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open());
        f.seekp(0, std::ios::beg);
        f.put('\x7F');   // 破坏首个字节（key 长度）
    }

    auto r = SstReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(r.error()));
}

}  // namespace
