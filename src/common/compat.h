// 文件用途：语言标准兼容层——C++23（默认）/20/17 三档共用同一份源码的桥接点。
// 归属：TinyKV-CPP（M7 工程化：多编译器 × 多标准支持面）。
//
// 设计取舍（为什么是"桥接"而不是"降级"）：
//   * 上层代码只写 tinykv::expected / tinykv::span / tinykv::unexpected / tinykv::as_bytes；
//     23/20 档这些别名直通 std，零开销、零行为差异；
//   * 17 档用本文件的最小实现顶上：语义相同（result 的 has_value/value/error/operator*），
//     不做 23 档没有的扩展，避免"两个标准下行为不同"这类最难查的差异。
//
// 17 档无法用语言机制绕过的两处（已在根 CMakeLists 与 docs/testing.md 说明）：
//   1) 指定初始化器（`.field = v`）：GCC/Clang/MSVC 在 17 档都按扩展接受，故 17 档关 -Wpedantic；
//   2) `std::string::starts_with` / `operator<=>`：由本文件的 starts_with() 与
//      ids.h 的显式比较运算符替代（不提供 <=>，因为它的默认实现对 RegionEpoch 是错的）。
//
// 引入顺序约束：本头只依赖标准库与 common/ids.h 之外的东西，故可被任何层包含。

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
// <version> 是库特性宏的唯一可靠来源：只探测 __cplusplus 会把"头文件存在但未包含"
// 误判成"能力可用"（本项目就踩过：去掉 <expected> 后宏消失，回落到自带实现）。
#include <type_traits>
#include <utility>
#include <variant>
#include <version>

// 能力探测：用特性宏而不是 __cplusplus 版本号，libstdc++/libc++ 的实现进度并不等价
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#define TINYKV_HAS_STD_EXPECTED 1
#else
#define TINYKV_HAS_STD_EXPECTED 0
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#define TINYKV_HAS_STD_SPAN 1
#else
#define TINYKV_HAS_STD_SPAN 0
#endif

#if TINYKV_HAS_STD_EXPECTED
#include <expected>
#endif
#if TINYKV_HAS_STD_SPAN
#include <span>
#endif

namespace tinykv {

#if TINYKV_HAS_STD_SPAN

template <typename T>
using span = std::span<T>;
using std::as_bytes;
using std::as_writable_bytes;

#else

// ---- C++17 兜底：只读/可写视图（覆盖项目用到的构造方式：指针+长度、数组、容器） ----
template <typename T>
class span {
public:
    using element_type = T;
    using value_type = std::remove_cv_t<T>;
    using pointer = T*;
    using reference = T&;
    using iterator = T*;

    constexpr span() noexcept = default;
    constexpr span(pointer data, std::size_t size) noexcept : data_(data), size_(size) {}
    // 定长数组：span<const std::byte>{arr}
    template <std::size_t N>
    constexpr span(element_type (&arr)[N]) noexcept : data_(arr), size_(N) {}  // NOLINT
    // 容器：与 std::span 一致地取 data()/size()（T 由下面的推导指引给出）
    template <typename Container,
              typename = std::enable_if_t<
                  !std::is_array_v<Container> && !std::is_pointer_v<Container> &&
                  std::is_convertible_v<decltype(std::declval<Container&>().data()), T*>>>
    constexpr span(Container& c) noexcept : data_(c.data()), size_(c.size()) {}  // NOLINT
    // 右值容器：与 std::span 一致——只允许绑定到"元素为 const"的视图，
    // 否则 span{临时 vector} 会留下悬垂指针。项目里大量 `write(vec_from_fn())` 依赖它。
    template <typename Container,
              typename = std::enable_if_t<
                  !std::is_lvalue_reference_v<Container> && std::is_const_v<T> &&
                  std::is_convertible_v<decltype(std::declval<Container&>().data()), T*>>>
    constexpr span(Container&& c) noexcept : data_(c.data()), size_(c.size()) {}  // NOLINT
    // 视图转换：span<char> → span<const char>（std::span 的隐式限定转换）
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    constexpr span(span<U> other) noexcept : data_(other.data()), size_(other.size()) {}  // NOLINT

    [[nodiscard]] constexpr pointer data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr reference operator[](std::size_t i) const { return data_[i]; }
    [[nodiscard]] constexpr iterator begin() const noexcept { return data_; }
    [[nodiscard]] constexpr iterator end() const noexcept { return data_ + size_; }
    [[nodiscard]] constexpr reference front() const { return data_[0]; }
    [[nodiscard]] constexpr reference back() const { return data_[size_ - 1]; }
    [[nodiscard]] constexpr span first(std::size_t n) const noexcept { return {data_, n}; }
    [[nodiscard]] constexpr span last(std::size_t n) const noexcept {
        return {data_ + (size_ - n), n};
    }
    [[nodiscard]] constexpr span subspan(std::size_t off) const noexcept {
        return {data_ + off, size_ - off};
    }

private:
    pointer data_{nullptr};
    std::size_t size_{0};
};

// CTAD：`span{ptr, len}` 与 `span{容器}` 在 17 档也要能用（调用点写法与 20+ 完全一致）
template <typename T>
span(T*, std::size_t) -> span<T>;

template <typename Container>
span(Container&) -> span<std::remove_pointer_t<decltype(std::declval<Container&>().data())>>;

// std::as_bytes 的等价物：把任意元素类型的 span 视为字节视图
template <typename T>
[[nodiscard]] inline span<const std::byte> as_bytes(span<T> s) noexcept {
    static_assert(std::is_trivially_copyable_v<std::remove_cv_t<T>>,
                  "as_bytes 只接受可平凡复制的元素（否则字节视图没有意义）");
    return {reinterpret_cast<const std::byte*>(s.data()), s.size() * sizeof(T)};
}

template <typename T>
[[nodiscard]] inline span<std::byte> as_writable_bytes(span<T> s) noexcept {
    static_assert(!std::is_const_v<T>, "as_writable_bytes 不接受 const 视图");
    return {reinterpret_cast<std::byte*>(s.data()), s.size() * sizeof(T)};
}

#endif  // TINYKV_HAS_STD_SPAN

#if TINYKV_HAS_STD_EXPECTED

template <typename T, typename E>
using expected = std::expected<T, E>;
template <typename E>
using unexpected = std::unexpected<E>;

#else

// ---- C++17 兜底：最小 expected（够项目用：取值/取错/判空，不实现 monadic 接口） ----
template <typename E>
class unexpected {
public:
    explicit constexpr unexpected(E e) : error_(std::move(e)) {}
    [[nodiscard]] constexpr const E& error() const& noexcept { return error_; }
    [[nodiscard]] constexpr E& error() & noexcept { return error_; }

private:
    E error_;
};

template <typename T, typename E>
class expected {
public:
    constexpr expected(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    constexpr expected(unexpected<E> err) : storage_(std::in_place_index<1>, std::move(err)) {}
    // 宽转换：unique_ptr<Derived> → result<unique_ptr<Base>> 这类"值可构造"的返回
    template <typename U,
              typename = std::enable_if_t<
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<U>>, T> &&
                  !std::is_same_v<std::remove_cv_t<std::remove_reference_t<U>>, unexpected<E>> &&
                  std::is_constructible_v<T, U&&>>>
    constexpr expected(U&& value)  // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<0>, T(std::forward<U>(value))) {}
    // 宽转换：`result<T>` 的 E 是 KvError（variant），而调用点写的是 unexpected<具体变体>。
    // 标准库的 std::expected 允许这种"错误类型可构造"的转换，17/20 档必须保持一致。
    template <typename E2, typename = std::enable_if_t<!std::is_same_v<E2, E> &&
                                                       std::is_constructible_v<E, const E2&>>>
    constexpr expected(unexpected<E2> err)  // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<1>, unexpected<E>(E(err.error()))) {}
    // void 特化用不到，故不提供；T 必须可移动可析构（本项目的 result<T> 全部满足）

    [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == 0; }
    explicit constexpr operator bool() const noexcept { return has_value(); }

    [[nodiscard]] constexpr T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] constexpr const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] constexpr T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] constexpr T& operator*() & { return value(); }
    [[nodiscard]] constexpr const T& operator*() const& { return value(); }
    [[nodiscard]] constexpr T* operator->() { return &value(); }
    [[nodiscard]] constexpr const T* operator->() const { return &value(); }

    [[nodiscard]] constexpr const E& error() const& { return std::get<1>(storage_).error(); }
    [[nodiscard]] constexpr E& error() & { return std::get<1>(storage_).error(); }

private:
    std::variant<T, unexpected<E>> storage_;
};

// void 特化：result<void> 是"只报错不带值"的主力形态
template <typename E>
class expected<void, E> {
public:
    constexpr expected() noexcept : storage_(std::in_place_index<0>) {}
    constexpr expected(unexpected<E> err) : storage_(std::in_place_index<1>, std::move(err)) {}
    template <typename E2, typename = std::enable_if_t<!std::is_same_v<E2, E> &&
                                                       std::is_constructible_v<E, const E2&>>>
    constexpr expected(unexpected<E2> err)  // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<1>, unexpected<E>(E(err.error()))) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == 0; }
    explicit constexpr operator bool() const noexcept { return has_value(); }
    constexpr void value() const {}

    [[nodiscard]] constexpr const E& error() const& { return std::get<1>(storage_).error(); }
    [[nodiscard]] constexpr E& error() & { return std::get<1>(storage_).error(); }

private:
    std::variant<std::monostate, unexpected<E>> storage_;
};

#endif  // TINYKV_HAS_STD_EXPECTED

// tinykv::unreachable：23 档有，20/17 档没有。用 abort 而不是 __builtin_unreachable()——
// 语义都是"不可达"，但真的走到了，abort 留得下栈，UB 只会变成一个无法归因的错值。
[[noreturn]] inline void unreachable() noexcept {
    std::abort();
}

// ---- std::string::starts_with 的统一替代（17 档没有；20+ 直接内联同一实现，开销为零） ----
[[nodiscard]] inline bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace tinykv
