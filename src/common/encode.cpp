// 文件用途：encode.h 两函数的实现——字节拼接与大端序列化（唯一实现点，禁外部自拼 key）。
// 归属：TinyKV-CPP（M0 基建，ENG-01）。

#include "common/encode.h"

namespace tinykv {
namespace {

// 大端 8 字节追加：字节序 = 数值序，故 ts 的字典序即时间序（SPEC-4 §21 依赖此性质）
void append_be64(std::string& out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

}  // namespace

// CF 前缀编码：[cf_id:1B][user_key]；空 key 拒绝（InvalidArgument，SPEC-0 §0.3.1 矩阵）
result<std::string> encode_key(Cf cf, tinykv::span<const std::byte> user_key) {
    if (user_key.empty()) {
        return tinykv::unexpected(InvalidArgument{"user_key 为空"});
    }
    std::string out;
    out.reserve(user_key.size() + 1);
    out.push_back(static_cast<char>(cf));  // cf_id 单字节定序（Default<Write<Lock）
    out.append(reinterpret_cast<const char*>(user_key.data()), user_key.size());
    return out;
}

// 事务时间戳编码：[cf_id:1B][user_key][ts:8B]；复用 encode_key 完成前缀与空 key 校验，
// 避免"两处各写一遍前缀"——前缀格式一旦分叉，MVCC 扫描会静默读错列族。
result<std::string> encode_ts_key(Cf cf, tinykv::span<const std::byte> user_key, Ts ts) {
    auto base = encode_key(cf, user_key);
    if (!base) {
        return tinykv::unexpected(base.error());
    }
    base->reserve(base->size() + 8);
    append_be64(*base, ts.value);
    return base;
}

// BUG-0004：raw KV 键 = 保留前缀 + user_key（不加 ts，不进 MVCC 版本空间）
std::string encode_raw_key(std::string_view user_key) {
    std::string out(kRawKeyPrefix);
    out.append(user_key);
    return out;
}

}  // namespace tinykv
