// 文件用途：列族隔离——同 key 在三个 CF 互不可见（FUNC-01、SPEC-0 §0.6 的 CF 隔离不变量）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：P4 事务靠 Write/Lock 两个列族承载提交记录与锁，如果 CF 前缀没起到隔离作用，
// 事务读会读到别人的数据、Delete 会误删另一个列族——这是最难事后排查的一类串味。

#include "storage/engine.h"

#include <gtest/gtest.h>

#include <string>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace) —— 测试内便捷引用


TEST(EngineCf, SameKeyIsolatedAcrossColumnFamilies) {
    TempDir tmp("tinykv_cf_iso");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    for (const auto& [cf, value] : {std::pair{Cf::Default, "d"}, std::pair{Cf::Write, "w"},
                                    std::pair{Cf::Lock, "l"}}) {
        ModifyVec batch{Modify{Put{.cf = cf, .key = "same", .value = value}}};
        ASSERT_TRUE((*engine)->write(batch).has_value());
    }

    for (const auto& [cf, value] : {std::pair{Cf::Default, "d"}, std::pair{Cf::Write, "w"},
                                    std::pair{Cf::Lock, "l"}}) {
        auto got = (*engine)->get(cf, bytes("same"));
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(got->value.has_value());
        EXPECT_EQ(*got->value, value) << "CF 隔离失效：读到了别的列族的值";
    }
}

TEST(EngineCf, WriteToWriteCfDoesNotLeakIntoDefault) {
    TempDir tmp("tinykv_cf_leak");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db"});
    ASSERT_TRUE(engine.has_value());

    ModifyVec batch{Modify{Put{.cf = Cf::Write, .key = "k", .value = "commit-ts"}}};
    ASSERT_TRUE((*engine)->write(batch).has_value());

    auto in_default = (*engine)->get(Cf::Default, bytes("k"));
    ASSERT_TRUE(in_default.has_value());
    EXPECT_FALSE(in_default->value.has_value());
}

}  // namespace
