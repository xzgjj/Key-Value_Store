// 文件用途：CRC32C（Castagnoli）查表实现——WAL 与 SST 的记录完整性校验唯一实现点。
// 归属：TinyKV-CPP（M1 单机存储，SPEC-1 §1.4.2）。
//
// 为什么用 Castagnoli 而非 IEEE CRC32：SSD/存储栈普遍有该多项式的硬件实现，
// 后续若换硬件指令只需替换本文件；校验范围覆盖"长度+载荷"，避免半条记录被当作完整记录。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "common/compat.h"

namespace tinykv {
namespace detail {

// 反射多项式 0x82F63B78（0x1EDC6F41 的位反转形式），编译期生成 256 项表
constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = ((c & 1U) != 0U) ? (0x82F63B78U ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

inline constexpr auto kCrc32cTable = make_crc32c_table();

}  // namespace detail

// 一次性计算整段字节的 CRC32C（seed 供分片续算，本项目单段调用时传 0）
[[nodiscard]] inline std::uint32_t crc32c(tinykv::span<const std::byte> data,
                                          std::uint32_t seed = 0) {
    std::uint32_t crc = ~seed;
    for (const std::byte b : data) {
        const auto idx = static_cast<std::uint8_t>((crc ^ static_cast<std::uint8_t>(b)) & 0xFFU);
        crc = detail::kCrc32cTable[idx] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace tinykv
