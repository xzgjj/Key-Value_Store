// 文件用途：引擎基础读写——Put/Get/Delete、覆盖、重启后仍在（FUNC-01、REL-02 冒烟）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：这是引擎的最小可信面。重启后仍能读到，说明 WAL→重放→内存表这条链是通的。

#include "storage/engine.h"

#include <gtest/gtest.h>

#include <string>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace) —— 测试内便捷引用


TEST(EngineGetPut, PutThenGet) {
    TempDir tmp("tinykv_eng_gp");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    ASSERT_TRUE((*engine)->write(put_batch("k1", "v1")).has_value());
    auto got = (*engine)->get(Cf::Default, bytes("k1"));
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "v1");
}

TEST(EngineGetPut, MissingKeyYieldsEmptyOptional) {
    TempDir tmp("tinykv_eng_miss");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    auto got = (*engine)->get(Cf::Default, bytes("absent"));
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->value.has_value());   // 区分"没有值"与"值为空串"
}

TEST(EngineGetPut, OverwriteAndDelete) {
    TempDir tmp("tinykv_eng_ow");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    ASSERT_TRUE((*engine)->write(put_batch("k", "v1")).has_value());
    ASSERT_TRUE((*engine)->write(put_batch("k", "v2")).has_value());
    auto got = (*engine)->get(Cf::Default, bytes("k"));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got->value, "v2");

    ModifyVec del{Modify{Delete{.cf = Cf::Default, .key = "k"}}};
    ASSERT_TRUE((*engine)->write(del).has_value());
    got = (*engine)->get(Cf::Default, bytes("k"));
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->value.has_value());
}

TEST(EngineGetPut, SurvivesReopen) {
    // 显式 close → 内存表落 SST → 重开必须读到（RED LINE：恢复路径只读不写也要能读全）
    TempDir tmp("tinykv_eng_reopen");
    const auto dir = tmp.path() / "db";
    {
        auto engine = Engine::open(EngineOptions{.dir = dir});
        ASSERT_TRUE(engine.has_value());
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE((*engine)->write(put_batch("k" + std::to_string(i), "v" + std::to_string(i))).has_value());
        }
        ASSERT_TRUE((*engine)->close().has_value());
    }
    auto reopened = Engine::open(EngineOptions{.dir = dir});
    ASSERT_TRUE(reopened.has_value());
    for (int i = 0; i < 50; ++i) {
        auto got = (*reopened)->get(Cf::Default, bytes("k" + std::to_string(i)));
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value()) << "重启后丢失 k" << i;
        EXPECT_EQ(*got->value, "v" + std::to_string(i));
    }
}

TEST(EngineGetPut, EmptyBatchRejected) {
    TempDir tmp("tinykv_eng_empty");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    ModifyVec empty;
    auto r = (*engine)->write(empty);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
}

// code-review P0-1 回归：空 key 必须在写 WAL 之前被拒——旧实现先落盘后校验，
// 坏记录会留在 WAL 里，导致下次 open 重放失败、整个目录打不开。
TEST(EngineGetPut, EmptyKeyRejectedBeforeWalAndDirectoryStillOpens) {
    TempDir tmp("tinykv_eng_empty_key");
    const auto dir = tmp.path() / "db";
    {
        auto engine = Engine::open(EngineOptions{.dir = dir});
        ASSERT_TRUE(engine.has_value());
        ModifyVec bad{Modify{Put{.cf = Cf::Default, .key = "", .value = "v"}}};
        auto r = (*engine)->write(bad);
        ASSERT_FALSE(r.has_value());
        EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
        // 同一引擎仍可正常写合法数据（失败没有污染内存表）
        ASSERT_TRUE((*engine)->write(put_batch("ok", "1")).has_value());
        ASSERT_TRUE((*engine)->close().has_value());
    }
    auto reopened = Engine::open(EngineOptions{.dir = dir});
    ASSERT_TRUE(reopened.has_value()) << "空 key 坏记录不应进入 WAL：恢复路径必须仍能打开";
    auto got = (*reopened)->get(Cf::Default, bytes("ok"));
    ASSERT_TRUE(got.has_value());
    ASSERT_TRUE(got->value.has_value());
    EXPECT_EQ(*got->value, "1");
}

// 非法 CF 同样在落盘前拒绝（网络/快照输入强转 Cf 是既有风险点）
TEST(EngineGetPut, InvalidCfRejected) {
    TempDir tmp("tinykv_eng_bad_cf");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    ModifyVec bad{
        Modify{Put{.cf = static_cast<Cf>(7), .key = "k", .value = "v"}}};
    auto r = (*engine)->write(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));

    ModifyVec bad_del{Modify{Delete{.cf = static_cast<Cf>(9), .key = "k"}}};
    r = (*engine)->write(bad_del);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
}

}  // namespace
