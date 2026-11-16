// 文件用途：LEB128 变长整数编解码——WAL 与 SST 共用同一实现（禁止各写一份，CI 会判重复）。
// 归属：TinyKV-CPP（M1 单机存储，SPEC-1 §1.4.2）。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tinykv {

// 追加变长整数（小端 7 位分组，最高位为续位）
inline void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<char>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<char>(v & 0x7FU));
}

// 从 in[pos] 起读一个变长整数；越界或超 64 位返回 false（不移动 pos 之外的副作用）
[[nodiscard]] inline bool get_varint(std::string_view in, std::size_t& pos, std::uint64_t& v) {
    v = 0;
    int shift = 0;
    while (pos < in.size() && shift <= 63) {
        const auto byte = static_cast<std::uint8_t>(in[pos++]);
        v |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            return true;
        }
        shift += 7;
    }
    return false;
}

}  // namespace tinykv
