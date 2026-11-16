// 文件用途：内存表单元测试——覆盖、墓碑、有序迭代与体积记账（SPEC-1 §1.4.1）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：内存表是写路径与 flush 的交界，两条语义错了会直接导致数据错误——
//   同 key 覆盖后旧值必须消失（否则读到脏值），Delete 必须留墓碑（否则旧 SST 版本复活）。

#include "storage/memtable.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using tinykv::MemTable;

TEST(MemTable, PutThenFind) {
    MemTable t;
    t.put("k1", std::string("v1"));

    const auto* v = t.find("k1");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->value.has_value());
    EXPECT_EQ(*v->value, "v1");
    EXPECT_EQ(t.find("absent"), nullptr);
}

TEST(MemTable, OverwriteKeepsOnlyLatestValue) {
    // 同 key 多次写：内存表只保留最新版本，flush 前的重复写天然合并
    MemTable t;
    t.put("k2", std::string("old"));
    t.put("k2", std::string("new"));

    ASSERT_EQ(t.entries().size(), 1U);
    ASSERT_NE(t.find("k2"), nullptr);
    EXPECT_EQ(*t.find("k2")->value, "new");
}

TEST(MemTable, DeleteStoresTombstone) {
    // 红线：墓碑必须进表（value 为空 optional），不能读路径"跳过不写"
    MemTable t;
    t.put("k3", std::string("v"));
    t.put("k3", std::nullopt);

    const auto* v = t.find("k3");
    ASSERT_NE(v, nullptr);
    EXPECT_FALSE(v->value.has_value());
    EXPECT_EQ(t.entries().size(), 1U);
}

TEST(MemTable, EntriesAreOrderedByEncodedKey) {
    // 有序是 Scan 归并的前提：字节序 = 比较序 = 迭代序
    MemTable t;
    t.put("b", std::string("2"));
    t.put("a", std::string("1"));
    t.put("c", std::string("3"));

    std::string seen;
    for (const auto& [key, value] : t.entries()) {
        (void)value;
        seen += key;
    }
    EXPECT_EQ(seen, "abc");
}

TEST(MemTable, BytesAccountingGrowsOnInsertAndStabilizesOnOverwrite) {
    MemTable t;
    EXPECT_EQ(t.bytes(), 0U);
    t.put("k", std::string("v"));
    const std::size_t after_insert = t.bytes();
    EXPECT_GT(after_insert, 0U);

    t.put("k", std::string("v"));  // 等长覆盖：体积不变
    EXPECT_EQ(t.bytes(), after_insert);
}

}  // namespace
