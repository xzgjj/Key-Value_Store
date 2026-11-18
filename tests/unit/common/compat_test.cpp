// 文件用途：语言标准兼容层（src/common/compat.h）的语义回归——保证 17/20/23 三档行为一致。
// 归属：TinyKV-CPP（M7 工程化：多编译器 × 多标准支持面）。
//
// 使用场景理解：兼容层最危险的失败模式不是"编译不过"（那会立刻暴露），而是
// "在某个标准下悄悄换了语义"——比如 result 的 error 取到的是另一个变体、
// span 的 size 按元素数算还是按字节算不一致。故本文件断言的是**语义**而非存在性：
//   1) result<T> / result<void> 的取值、取错、宽转换；
//   2) span 的三种构造方式与 as_bytes 的字节口径；
//   3) starts_with / unreachable 的可调用性。
// 同一份断言在三档下都必须通过（CI 的 compat 矩阵逐档执行）。

#include "common/compat.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "common/error.h"

namespace {

using tinykv::as_bytes;
using tinykv::result;
using tinykv::span;
using tinykv::starts_with;
using tinykv::unexpected;

// result<T>：正常值、错误值、错误类型宽转换（unexpected<具体变体> → KvError）
TEST(CompatTest, ExpectedCarriesValueAndError) {
    result<int> ok{42};
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_EQ(*ok, 42);
    EXPECT_EQ(ok.value(), 42);

    result<int> bad = unexpected<tinykv::InvalidArgument>(tinykv::InvalidArgument{"负数"});
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(bad.error()))
        << "宽转换后必须仍是原来的变体（不能退化成别的错误种类）";
    EXPECT_EQ(std::get<tinykv::InvalidArgument>(bad.error()).what, "负数");
}

// result<void>：只表达"成功/失败"，成功时可默认构造
TEST(CompatTest, ExpectedVoidReportsSuccessOrError) {
    result<void> ok{};
    ASSERT_TRUE(ok.has_value());
    ok.value();  // 不抛

    result<void> bad = unexpected<tinykv::IoFailed>(tinykv::IoFailed{.path = "p", .reason = "r"});
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(std::get<tinykv::IoFailed>(bad.error()).reason, "r");
}

// result<unique_ptr<Derived>> 由 unique_ptr<Base> 承载：宽值转换必须成立
TEST(CompatTest, ExpectedConvertsDerivedPointer) {
    struct Base {
        virtual ~Base() = default;
        [[nodiscard]] virtual int id() const { return 0; }
    };
    struct Derived final : Base {
        [[nodiscard]] int id() const override { return 7; }
    };
    result<std::unique_ptr<Base>> r{std::make_unique<Derived>()};
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->id(), 7);
}

// span：指针+长度 / 定长数组 / 容器三种构造，size 一率按"元素个数"
TEST(CompatTest, SpanConstructsFromPointerArrayAndContainer) {
    const std::array<int, 3> arr{1, 2, 3};
    const span<const int> from_array{arr};
    EXPECT_EQ(from_array.size(), 3U);
    EXPECT_EQ(from_array.front(), 1);
    EXPECT_EQ(from_array.back(), 3);

    const std::vector<int> vec{4, 5};
    const span<const int> from_vec{vec};
    EXPECT_EQ(from_vec.size(), 2U);
    EXPECT_EQ(from_vec[1], 5);

    const int raw[2] = {8, 9};
    const span<const int> from_ptr{raw, 2};
    EXPECT_EQ(from_ptr.size(), 2U);
    EXPECT_EQ(from_ptr.last(1).front(), 9);
}

// as_bytes：字节口径 = 元素数 × sizeof(元素)，这是存储层所有接口的前提
TEST(CompatTest, AsBytesCountsBytesNotElements) {
    const std::string text = "abcd";
    const span<const char> chars{text};
    const auto bytes = as_bytes(chars);
    EXPECT_EQ(chars.size(), 4U);
    EXPECT_EQ(bytes.size(), 4U);

    const std::array<std::uint32_t, 3> words{1, 2, 3};
    const auto word_bytes = as_bytes(span<const std::uint32_t>{words});
    EXPECT_EQ(word_bytes.size(), 3U * sizeof(std::uint32_t));
    EXPECT_EQ(word_bytes.size(), 12U);
    EXPECT_EQ(static_cast<unsigned char>(word_bytes[0]), 1U);  // 小端首字节
}

// 右值容器可直接作为实参传进"接收 span"的函数（生命周期到全表达式结束为止）。
// 这正是 `storage.write(vector_from_fn())` 这类调用的形态：函数体内使用，不存出去。
// 注意不能把这样的视图存进具名变量——那才是悬垂（clang 的 -Wdangling-gsl 会直接报）。
namespace {
std::size_t count_via_span(span<const int> view) {
    return view.size();
}
}  // namespace

TEST(CompatTest, SpanAcceptsTemporaryContainerAsConstView) {
    const auto make = [] { return std::vector<int>{1, 2, 3}; };
    EXPECT_EQ(count_via_span(make()), 3U);
}

// starts_with：前缀判定（17 档没有 std::string::starts_with，调用点统一走这里）
TEST(CompatTest, StartsWithMatchesPrefixOnly) {
    EXPECT_TRUE(starts_with("tinykv-server", "tinykv"));
    EXPECT_TRUE(starts_with(std::string{"tinykv"}, "tiny"));
    EXPECT_FALSE(starts_with("ti", "tinykv"));     // 前缀比自身长
    EXPECT_FALSE(starts_with("xtinykv", "tiny"));  // 不是前缀
    EXPECT_TRUE(starts_with("abc", ""));           // 空前缀恒真
}

}  // namespace
