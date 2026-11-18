// 文件用途：common/ids 强类型 ID 单元测试——类型即注释（SPEC-0 §0.3.1/§0.6；清单 #2/#10）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。
// 使用场景理解：ID 混用（Term 当 LogIndex 用）是分布式事故的常见源头，防线放在编译期；
// 除行为断言外，本文件包含多个 static_assert 型"负例测试"——它们测试的是"编译不过"本身。

#include "common/ids.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

using tinykv::LogIndex;
using tinykv::RegionId;
using tinykv::Term;

// ---------- 可移植的编译期探针（C++17/20/23 同义） ----------
// 为什么不用 std::concepts：17 档没有 <concepts>，而"负例测试"要表达的是
// "这个表达式能不能编译"，用 void_t 探测比概念更直接、三档语义完全一致。
template <typename A, typename B, typename = void>
struct has_eq : std::false_type {};
template <typename A, typename B>
struct has_eq<A, B, std::void_t<decltype(std::declval<A>() == std::declval<B>())>>
    : std::true_type {};

template <typename A, typename B, typename = void>
struct has_lt : std::false_type {};
template <typename A, typename B>
struct has_lt<A, B, std::void_t<decltype(std::declval<A>() < std::declval<B>())>> : std::true_type {
};

template <typename Op, typename A, typename B, typename = void>
struct can_invoke : std::false_type {};
template <typename Op, typename A, typename B>
struct can_invoke<Op, A, B,
                  std::void_t<decltype(std::declval<Op>()(std::declval<A>(), std::declval<B>()))>>
    : std::true_type {};

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
    static_assert(!has_eq<Term, LogIndex>::value, "跨 Tag 相等比较必须编译失败");
    static_assert(!has_lt<Term, LogIndex>::value, "跨 Tag 序比较必须编译失败");
    static_assert(has_eq<Term, Term>::value);  // 同 Tag 可用
    static_assert(has_lt<Term, Term>::value);
}

TEST(StrongId, NoImplicitIntegerConversion) {
    // 禁 uint64 → StrongId 隐式转换（必须显式构造）与反向隐式转换（红线 2）
    static_assert(!std::is_convertible_v<uint64_t, Term>);
    static_assert(!std::is_convertible_v<Term, uint64_t>);
    static_assert(std::is_constructible_v<Term, uint64_t>);  // 显式构造必须可用
}

TEST(StrongId, NoArithmeticOperators) {
    // ID 无算术语义：+/-/* 均不可用（SPEC-0 §0.3.1 注释）
    static_assert(!can_invoke<std::plus<>, Term, Term>::value);
    static_assert(!can_invoke<std::minus<>, Term, Term>::value);
    static_assert(!can_invoke<std::multiplies<>, Term, int>::value);
}

TEST(StrongId, EightAliasesAreDistinctTypes) {
    // 八个别名两两互异：任取三组验证"同值不同型"（完整八组由模板保证）
    static_assert(!std::is_same_v<tinykv::Term, tinykv::LogIndex>);
    static_assert(!std::is_same_v<tinykv::RegionId, tinykv::PeerId>);
    static_assert(!std::is_same_v<tinykv::SeqNo, tinykv::Ts>);
    static_assert(!std::is_same_v<tinykv::StoreId, tinykv::ClientId>);
}

// ---------- 可用性：值初始化、无效值、单调推进、零开销 ----------
// 场景理解：只有"编译期禁错"还不够——类型要能真的装进容器、能表达"未设置"，
// 否则上层会用 optional/魔法值绕过强类型，防线反而从侧面被拆掉。

TEST(StrongId, ValueInitializableForContainers) {
    // 回归点：M0 版无默认构造 → 不满足 DefaultInsertable → vector<LogIndex>(n) 编译失败。
    // Raft 里按键值下标建定长数组是常态，这条必须可用。
    static_assert(std::is_default_constructible_v<Term>);
    std::vector<LogIndex> by_index(4);
    std::array<Term, 3> terms{};
    for (const auto& idx : by_index) {
        EXPECT_EQ(idx, LogIndex::invalid());  // 值初始化 = 未设置，而非随机的 0
    }
    for (const auto& t : terms) {
        EXPECT_FALSE(t.is_valid());
    }
    EXPECT_EQ(by_index.size(), 4U);
}

TEST(StrongId, InvalidIsExplicitAndNamed) {
    // "未设置"必须能被显式表达与判断：有效值从 1 起（proto/raft.proto：index/term 0 非法）
    constexpr Term none = Term::invalid();
    constexpr Term first{1};
    static_assert(none.value == 0);
    static_assert(!none.is_valid());
    static_assert(first.is_valid());
    EXPECT_EQ(Term{}, none);  // 默认构造与 invalid() 是同一语义
}

TEST(StrongId, NextAndPrevCarryIntent) {
    // 算术运算符被禁，但 index±1 是 Raft 常态：用命名函数让意图可读、可 grep
    constexpr LogIndex i{7};
    static_assert(i.next() == LogIndex{8});
    static_assert(i.prev() == LogIndex{6});
    static_assert(i.next().prev() == i);
    EXPECT_LT(i, i.next());
}

TEST(StrongId, StdHashWorksWithoutKnowingTag) {
    // 回归点：M0 版只有 HashFor<TermTag>，调用方必须穿透 typedef 才知道 Tag。
    static_assert(std::is_invocable_r_v<size_t, std::hash<Term>, Term>);
    std::unordered_set<RegionId> regions;
    regions.insert(RegionId{1});
    regions.insert(RegionId{1});  // 重复插入应被去重（比较 + 哈希共同生效）
    regions.insert(RegionId{2});
    EXPECT_EQ(regions.size(), 2U);

    // constexpr：编译期即可用（C++26 的 std::hash 才 constexpr，这里自己实现混合）
    constexpr size_t h1 = std::hash<Term>{}(Term{3});
    constexpr size_t h2 = std::hash<Term>{}(Term{4});
    static_assert(h1 != h2);
}

TEST(StrongId, ZeroOverheadLayout) {
    // 头文件宣称"零开销"：无 vtable、无额外存储——用 sizeof 把这句话变成可判定的
    static_assert(sizeof(Term) == sizeof(uint64_t));
    static_assert(std::is_trivially_copyable_v<Term>);
    static_assert(std::is_standard_layout_v<Term>);
}

}  // namespace
