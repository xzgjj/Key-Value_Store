// 文件用途：Envelope 批量日志格式——把多条已序列化 RaftCmdRequest 打包进一条 Entry.data（M4 第①层）。
// 归属：TinyKV-CPP（M4 Multi-Raft 批处理基础设施；PERF-01 收口）。
//
// 为什么需要：batch=1 时每条写要走一次完整的共识往返（propose→复制→commit→apply），
// 而往返次数是吞吐瓶颈（实测 batch=64 比 batch=1 快 45×）。Envelope 把窗口内多条请求
// 合成一条日志 → 一次广播 → apply 时解包逐条执行，等价于服务端自动做了客户端 batch。
//
// 格式：[magic u32][count u32][len u32][data]...；
//   magic = 0x544B5642（"TKBV"），protobuf 消息首字节是字段 tag（≤0x7a），不会误判。
//   向后兼容：解包返回 false 表示这不是 Envelope，调用方按旧的单条 protobuf data 处理。
//   不改 proto 契约：Entry.data 本就是 opaque bytes。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinykv::raft {

constexpr std::uint32_t kEntryBatchMagic = 0x544B5642U;   // "TKBV"

inline void put_batch_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

inline std::uint32_t get_batch_u32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

// 把多条 data 打包成一个 Envelope（空输入产生 count=0 的合法 Envelope）
inline std::string pack_entries(const std::vector<std::string>& items) {
    std::string out;
    out.reserve(8U + items.size() * 8U);
    put_batch_u32(out, kEntryBatchMagic);
    put_batch_u32(out, static_cast<std::uint32_t>(items.size()));
    for (const auto& s : items) {
        put_batch_u32(out, static_cast<std::uint32_t>(s.size()));
        out.append(s);
    }
    return out;
}

// 尝试解包：成功返回 true 且 out 为按序还原的 data；非 Envelope 或损坏返回 false。
// 任何长度越界/尾随垃圾都判失败，绝不返回半条——apply 不执行错乱命令。
inline bool try_unpack_entries(std::string_view data, std::vector<std::string>& out) {
    if (data.size() < 8U || get_batch_u32(data.data()) != kEntryBatchMagic) {
        return false;
    }
    const auto count = get_batch_u32(data.data() + 4);
    if (count > data.size() / 8U) {
        return false;   // 每条至少占 4 字节长度头，条数不可能超过 size/8
    }
    std::vector<std::string> result;
    result.reserve(count);
    std::size_t pos = 8U;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + 4U > data.size()) {
            return false;
        }
        const auto len = get_batch_u32(data.data() + pos);
        pos += 4U;
        if (len > data.size() - pos) {
            return false;
        }
        result.emplace_back(data.substr(pos, len));
        pos += len;
    }
    if (pos != data.size()) {
        return false;   // 尾随垃圾 = 损坏
    }
    out = std::move(result);
    return true;
}

}  // namespace tinykv::raft
