// 文件用途：Scan 一致性快照——Reader 建立后发生 flush，结果集不受影响（SPEC-1 红线 3）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：split-check 与事务范围读都是"边扫边写"的场景。若 Reader 与 flush 共享可变状态，
// 扫描结果会出现"一半来自旧层、一半来自新层"的撕裂，测试把它固定成断言。

#include "storage/engine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace) —— 测试内便捷引用


TEST(EngineScanConcurrent, ReaderUnaffectedByFlush) {
    TempDir tmp("tinykv_scan_snap");
    // 内存表上限压到 1 字节：每次写入都会触发 flush，确保扫描期间确实发生了换层
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db", .memtable_limit_bytes = 1});
    ASSERT_TRUE(engine.has_value());

    for (const char* k : {"a", "b", "c"}) {
        ASSERT_TRUE((*engine)->write(put_batch(k, "v")).has_value());
    }

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());

    // Reader 建立后再写两条：这两条必须不出现在该 Reader 的结果里
    ASSERT_TRUE((*engine)->write(put_batch("d", "v")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("e", "v")).has_value());

    auto it = (*reader)->scan(Cf::Default, {}, 100);
    ASSERT_TRUE(it.has_value());
    std::vector<std::string> keys;
    for (; (*it)->valid(); (*it)->next()) {
        keys.emplace_back((*it)->key());
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(EngineScanConcurrent, ReaderStillSeesPreFlushWrites) {
    TempDir tmp("tinykv_scan_snap2");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db", .memtable_limit_bytes = 1});
    ASSERT_TRUE(engine.has_value());
    ASSERT_TRUE((*engine)->write(put_batch("only", "kept")).has_value());

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto got = (*reader)->get(Cf::Default, bytes("only"));
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "kept");
}

}  // namespace
