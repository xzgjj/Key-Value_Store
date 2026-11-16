// 文件用途：common/ids 强类型 ID 单元测试——类型即注释（SPEC-0 §0.3.1/§0.6；清单 #2/#10）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。
// 使用场景理解：ID 混用（Term 当 LogIndex 用）是分布式事故的常见源头，防线放在编译期；
// 除行为断言外，本文件包含多个 static_assert 型"负例测试"——它们测试的是"编译不过"本身。

#include "common/ids.h"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace {

using tinykv::LogIndex;
using tinykv::RegionId;
using tinykv::Term;

// ---------- 行为断言：同 Tag 可比较、可排序 ----------

TEST(StrongId, EqualityAndOrderWithinTag) {
    EXPECT_EQ(Term{5}, Term{5});
    EXPECT_NE(Term{5}, Term{6});
    EXPECT_LT(Term{5}, Term{6});
    EXPECT_GT(LogIndex{9}, LogIndex{1});
}

TEST(StrongId, ExplicitValueAccess) {
    // 红线 2：取裸值必须显式 .value（事故源可 grep）
    constexpr Term t{7};
    static_assert(t.value == 7);
    EXPECT_EQ(t.value, 7ULL);
}

TEST(StrongId, HashableForUnorderedUse) {
    // 比较/哈希可用（SPEC-0 §0.3.1）：经项目哈希器 HashFor（std::hash 特化留给容器层按需注册）
    tinykv::HashFor<tinykv::TermTag> h;
    EXPECT_EQ(h(Term{3}), h(Term{3}));
    EXPECT_NE(h(Term{3}), h(Term{4}));
}

// ---------- 编译期负例：错误用法应当"编译不过" ----------
// 注：本机 MSYS2 GCC 16.2 对 requires 表达式内的运算符失败报硬错误（非 SFINAE 判假），
// 负例一律改用标准概念与 std::invocable 检测（语义等价且可移植）。

TEST(StrongId, CrossTagComparisonForbidden) {
    // 不同 Tag 的 StrongId 不可比较（清单 #2：交换实参编译报错）
    static_assert(!std::equality_comparable_with<Term, LogIndex>);
    static_assert(!std::totally_ordered_with<Term, LogIndex>);
    static_assert(std::equality_comparable<Term>);  // 同 Tag 全序可用
    static_assert(std::totally_ordered<Term>);
}

TEST(StrongId, NoImplicitIntegerConversion) {
    // 禁 uint64 → StrongId 隐式转换（必须显式构造）与反向隐式转换（红线 2）
    static_assert(!std::convertible_to<uint64_t, Term>);
    static_assert(!std::convertible_to<Term, uint64_t>);
    static_assert(std::constructible_from<Term, uint64_t>);  // 显式构造必须可用
}

TEST(StrongId, NoArithmeticOperators) {
    // ID 无算术语义：+/-/* 均不可用（SPEC-0 §0.3.1 注释）
    static_assert(!std::invocable<std::plus<>, Term, Term>);
    static_assert(!std::invocable<std::minus<>, Term, Term>);
    static_assert(!std::invocable<std::multiplies<>, Term, int>);
}

TEST(StrongId, EightAliasesAreDistinctTypes) {
    // 八个别名两两互异：任取三组验证"同值不同型"（完整八组由模板保证）
    static_assert(!std::same_as<tinykv::Term, tinykv::LogIndex>);
    static_assert(!std::same_as<tinykv::RegionId, tinykv::PeerId>);
    static_assert(!std::same_as<tinykv::SeqNo, tinykv::Ts>);
    static_assert(!std::same_as<tinykv::StoreId, tinykv::ClientId>);
}

}  // namespace
