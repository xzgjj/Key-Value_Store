// 文件用途：测试辅助——string_view → span<const byte>（编码接口统一收字节视图，避免 ptr+len）。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace tinykv::testlib {

// 生命周期跟随入参：调用方保证 s 在 span 使用期间有效
[[nodiscard]] inline std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

}  // namespace tinykv::testlib
