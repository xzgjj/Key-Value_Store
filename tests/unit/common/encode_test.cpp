// 文件用途：common/encode 单元测试——CF 前缀编码与事务时间戳编码（SPEC-0 §0.3.1/§0.6）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。
// 使用场景理解：编码是存储层字节布局的唯一事实点（SPEC-1 §127），此处验证三条不变量：
// 组内保序、CF 间隔离、空 key 拒绝；ts 编码满足 SPEC-4 §21"同 key 多版本字节序有序"。

#include "common/encode.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace {

using tinykv::Cf;
using tinykv::Ts;

std::span<const std::byte> bytes(std::string_view s) {
    // 测试辅助：字符串视图 → 字节 span（断言场景均为 ASCII，无符号性无歧义）
    return std::as_bytes(std::span{s.data(), s.size()});
}

TEST(EncodeKey, CfPrefixKeepsOrderWithinCf) {
    // 同一 CF 内：字节序 = 比较序（白皮书 P1 核心技巧，SPEC-0 红线 1）
    auto a = tinykv::encode_key(Cf::Default, bytes("abc"));
    auto b = tinykv::encode_key(Cf::Default, bytes("abd"));
    ASSERT_TRUE(a && b);
    EXPECT_LT(*a, *b);
}

TEST(EncodeKey, CfIsolation) {
    // 不同 CF 同 key 不冲突：cf_id 前缀定序，三 CF 互不可见（SPEC-0 §0.6）
    auto d = tinykv::encode_key(Cf::Default, bytes("k"));
    auto w = tinykv::encode_key(Cf::Write, bytes("k"));
    auto l = tinykv::encode_key(Cf::Lock, bytes("k"));
    ASSERT_TRUE(d && w && l);
    EXPECT_LT(*d, *w);
    EXPECT_LT(*w, *l);
    EXPECT_NE(*d, *w);
}

TEST(EncodeKey, EmptyKeyRejected) {
    // 空 key = InvalidArgument（SPEC-0 §0.3.1 触发条件矩阵）
    auto e = tinykv::encode_key(Cf::Default, bytes(""));
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(e.error()));
}

TEST(EncodeKey, PrefixScanRangeWithinCf) {
    // 前缀扫描可用性：seek 到 "a" 与 "b" 之间即覆盖 "a*" 全部（M1 Scan 的编码基础）
    auto a1 = tinykv::encode_key(Cf::Default, bytes("a1"));
    auto a2 = tinykv::encode_key(Cf::Default, bytes("a2"));
    auto b1 = tinykv::encode_key(Cf::Default, bytes("b1"));
    ASSERT_TRUE(a1 && a2 && b1);
    EXPECT_LT(*a1, *b1);
    EXPECT_LT(*a2, *b1);
}

TEST(EncodeTsKey, VersionsOfSameKeyOrderByTs) {
    // SPEC-4 §21：cf 前缀 + user_key + 大端 ts 后缀 → 同 key 多版本字节序有序（Scan 免排序）
    auto t1 = tinykv::encode_ts_key(Cf::Write, bytes("row"), Ts{1});
    auto t2 = tinykv::encode_ts_key(Cf::Write, bytes("row"), Ts{2});
    auto t3 = tinykv::encode_ts_key(Cf::Write, bytes("row"), Ts{0x7FFFFFFF'00000001ULL});
    ASSERT_TRUE(t1 && t2 && t3);
    EXPECT_LT(*t1, *t2);
    EXPECT_LT(*t2, *t3);
}

TEST(EncodeTsKey, BareKeyIsPrefixOfAllVersions) {
    // 回归点（2026-11-16）：这条断言此前形同虚设——encode_key 带 cf 前缀而 encode_ts_key
    // 不带，两者根本不在同一 key 空间，测试却只断言了 ts 编码自身的 starts_with("a")。
    // 现在统一带前缀后，真正要锁的是"裸 key 是全部版本的公共前缀"：seek(bare) 即可命中
    // 首个版本，这是 MVCC 按 key 前缀扫描的地基。
    auto bare = tinykv::encode_key(Cf::Write, bytes("a"));
    auto v1 = tinykv::encode_ts_key(Cf::Write, bytes("a"), Ts{100});
    auto v2 = tinykv::encode_ts_key(Cf::Write, bytes("a"), Ts{200});
    ASSERT_TRUE(bare && v1 && v2);
    EXPECT_TRUE(v1->starts_with(*bare));
    EXPECT_TRUE(v2->starts_with(*bare));
    EXPECT_LT(*bare, *v1);
}

TEST(EncodeTsKey, CfIsolationForVersionedKeys) {
    // 同一 user_key + 同一 ts，落在 Write 与 Lock 两个 CF 时必须互不冲突、且定序可依赖
    auto w = tinykv::encode_ts_key(Cf::Write, bytes("k"), Ts{7});
    auto l = tinykv::encode_ts_key(Cf::Lock, bytes("k"), Ts{7});
    ASSERT_TRUE(w && l);
    EXPECT_NE(*w, *l);
    EXPECT_LT(*w, *l);  // cf_id 前缀：Default < Write < Lock
}

TEST(EncodeTsKey, EmptyKeyRejected) {
    auto e = tinykv::encode_ts_key(Cf::Write, bytes(""), Ts{1});
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(e.error()));
}

}  // namespace
