// 文件用途：SST 读写直测——尾部 CRC 自洽、墓碑保留、乱序拒绝、损坏即拒（SPEC-1 §1.4.4）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：flush 写出的表若 CRC 范围算错，正常写入与读取都看不出问题，只有"重开时校验失败"
//   才暴露——所以必须有一个直接对着 SST 文件断言自洽性的用例，而不是只靠引擎层面的重开冒烟。

#include "storage/sst.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/crc32c.h"
#include "common/error.h"
#include "common/varint.h"
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

// code-review（第三批）：空 key 是合法字节串，writer 不能用 last_key_.empty() 当"没有上一条"，
// 否则 add("") 之后重复 add("") 会绕过严格升序检查。
TEST(Sst, EmptyKeyOrderingIsChecked) {
    TempDir tmp("tinykv_sst_empty_key");
    auto w = SstWriter::create(tmp.path() / "t.sst");
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE((*w)->add("", std::string("first")).has_value());
    EXPECT_FALSE((*w)->add("", std::string("dup")).has_value()) << "重复空 key 必须被拒";
    ASSERT_TRUE((*w)->add("a", std::string("1")).has_value());
    ASSERT_TRUE((*w)->finish().has_value());
}

namespace {

void put_u32_le(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

// 手工拼一张表：便于构造"CRC 正确但语义非法"的文件
void append_record(std::string& body, const std::string& key, std::uint8_t kind,
                   const std::string& value) {
    tinykv::put_varint(body, key.size());
    body.append(key);
    body.push_back(static_cast<char>(kind));
    tinykv::put_varint(body, value.size());
    body.append(value);
}

void write_table(const fs::path& path, const std::string& body, std::uint32_t count) {
    std::string file = body;
    put_u32_le(file, count);
    put_u32_le(file, tinykv::crc32c(tinykv::as_bytes(tinykv::span{body})));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
}

}  // namespace

// code-review（第三批）：解析时必须校验严格升序与 kind 取值。
// 乱序表会让二分查找 find() 返回错误结果；未知 kind 被当成墓碑 = 静默丢数据。
TEST(Sst, UnsortedBodyRejected) {
    TempDir tmp("tinykv_sst_unsorted");
    std::string body;
    append_record(body, "b", 0, "2");
    append_record(body, "a", 0, "1");
    const auto path = tmp.path() / "bad.sst";
    write_table(path, body, 2);

    auto r = SstReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(r.error()));
}

TEST(Sst, UnknownKindRejected) {
    TempDir tmp("tinykv_sst_kind");
    std::string body;
    append_record(body, "k", 7, "v");  // kind 只允许 0/1
    const auto path = tmp.path() / "bad.sst";
    write_table(path, body, 1);

    auto r = SstReader::open(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::Corrupted>(r.error()));
}

}  // namespace
