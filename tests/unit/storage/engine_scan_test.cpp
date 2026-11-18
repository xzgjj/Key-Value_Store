// 文件用途：Scan 范围语义——升序、limit 截断、起点含、墓碑不出现（FUNC-01 Scan）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：Scan 的上层用途是 split-check 与事务范围读，所以"字节序 = 迭代序"必须成立；
// 同时墓碑必须被过滤掉，但又不能打断后续 key 的遍历（这是归并写错时最常见的症状）。

#include "storage/engine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace) —— 测试内便捷引用


std::vector<std::string> collect_keys(tinykv::Iterator& it) {
    std::vector<std::string> keys;
    for (; it.valid(); it.next()) {
        keys.emplace_back(it.key());
    }
    return keys;
}

// 写入 a/b/c 后按任意顺序读取，断言迭代序为字典序
TEST(EngineScan, ReturnsKeysInAscendingOrder) {
    TempDir tmp("tinykv_scan_order");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());
    ASSERT_TRUE((*engine)->write(put_batch("c", "3")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("a", "1")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("b", "2")).has_value());

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto it = (*reader)->scan(Cf::Default, {}, 10);
    ASSERT_TRUE(it.has_value());
    EXPECT_EQ(collect_keys(**it), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(EngineScan, LimitTruncatesAndStartIsInclusive) {
    TempDir tmp("tinykv_scan_limit");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());
    for (const char* k : {"a", "b", "c", "d"}) {
        ASSERT_TRUE((*engine)->write(put_batch(k, "v")).has_value());
    }

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto it = (*reader)->scan(Cf::Default, bytes("b"), 2);
    ASSERT_TRUE(it.has_value());
    EXPECT_EQ(collect_keys(**it), (std::vector<std::string>{"b", "c"}));   // 起点含 + 截断
}

TEST(EngineScan, TombstoneIsSkippedWithoutBreakingOrder) {
    TempDir tmp("tinykv_scan_tomb");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());
    for (const char* k : {"a", "b", "c"}) {
        ASSERT_TRUE((*engine)->write(put_batch(k, "v")).has_value());
    }
    ModifyVec del{Modify{Delete{.cf = Cf::Default, .key = "b"}}};
    ASSERT_TRUE((*engine)->write(del).has_value());

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto it = (*reader)->scan(Cf::Default, {}, 10);
    ASSERT_TRUE(it.has_value());
    EXPECT_EQ(collect_keys(**it), (std::vector<std::string>{"a", "c"}));   // b 消失，c 仍在
}

TEST(EngineScan, ZeroLimitRejected) {
    TempDir tmp("tinykv_scan_zero");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto it = (*reader)->scan(Cf::Default, {}, 0);
    ASSERT_FALSE(it.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(it.error()));
}

TEST(EngineScan, ValuesTravelWithKeys) {
    TempDir tmp("tinykv_scan_vals");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());
    ASSERT_TRUE((*engine)->write(put_batch("x", "10")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("y", "20")).has_value());

    auto reader = (*engine)->create_reader();
    ASSERT_TRUE(reader.has_value());
    auto it = (*reader)->scan(Cf::Default, {}, 10);
    ASSERT_TRUE(it.has_value());
    ASSERT_TRUE((*it)->valid());
    EXPECT_EQ((*it)->value(), "10");
    (*it)->next();
    ASSERT_TRUE((*it)->valid());
    EXPECT_EQ((*it)->key(), "y");
    EXPECT_EQ((*it)->value(), "20");
}

// 流式归并：同 key 跨多 SST + 内存表时取最新值，墓碑压制旧层；COW 快照不被后续写入污染。
TEST(EngineScan, StreamMergeAcrossLayersAndCowSnapshot) {
    TempDir tmp("tinykv_scan_layers");
    // 1 字节上限：每次写都 flush，造出多层 SST，最后一层留在内存表
    auto engine = Engine::open(EngineOptions{
        .dir = tmp.path() / "db", .memtable_limit_bytes = 1, .sst_compact_threshold = 0});
    ASSERT_TRUE(engine.has_value());
    ASSERT_TRUE((*engine)->write(put_batch("k", "old")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("k", "new")).has_value());

    auto reader = (*engine)->create_reader();   // 此刻 k=new（内存表最新）
    ASSERT_TRUE(reader.has_value());
    ASSERT_TRUE((*engine)->write(put_batch("k", "newer")).has_value());   // COW：不得改动快照
    ASSERT_TRUE((*engine)->write(ModifyVec{Modify{Delete{.cf = Cf::Default, .key = "k"}}})
                    .has_value());

    auto snap = (*reader)->scan(Cf::Default, {}, 10);
    ASSERT_TRUE(snap.has_value());
    ASSERT_TRUE((*snap)->valid());
    EXPECT_EQ((*snap)->value(), "new");   // 快照仍是建 Reader 时的值

    auto live = (*engine)->create_reader();
    ASSERT_TRUE(live.has_value());
    auto live_it = (*live)->scan(Cf::Default, {}, 10);
    ASSERT_TRUE(live_it.has_value());
    EXPECT_FALSE((*live_it)->valid());    // 最新视图：k 已被墓碑删除
}

}  // namespace
