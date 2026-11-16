// 文件用途：encode.h 两函数的实现——字节拼接与大端序列化（唯一实现点，禁外部自拼 key）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。

#include "common/encode.h"

namespace tinykv {

// CF 前缀编码：[cf_id:1B][user_key]；空 key 拒绝（InvalidArgument，SPEC-0 §0.3.1 矩阵）
result<std::string> encode_key(Cf cf, std::span<const std::byte> user_key) {
    if (user_key.empty()) {
        return std::unexpected(InvalidArgument{"user_key 为空"});
    }
    std::string out;
    out.reserve(user_key.size() + 1);
    out.push_back(static_cast<char>(cf));  // cf_id 单字节定序（Default<Write<Lock）
    out.append(reinterpret_cast<const char*>(user_key.data()), user_key.size());
    return out;
}

// 事务时间戳编码：[user_key][ts:8B 大端]；大端保证 ts 的字节序 = 数值序
result<std::string> encode_ts_key(std::span<const std::byte> user_key, Ts ts) {
    if (user_key.empty()) {
        return std::unexpected(InvalidArgument{"user_key 为空"});
    }
    std::string out;
    out.reserve(user_key.size() + 8);
    out.append(reinterpret_cast<const char*>(user_key.data()), user_key.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        // 高字节在前：逐字节大端落盘（reinterpret_cast<byte→char> 为合法别名转换）
        out.push_back(static_cast<char>((ts.value >> shift) & 0xFFU));
    }
    return out;
}

}  // namespace tinykv
