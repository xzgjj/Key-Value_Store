// 文件用途：内存表——有序、可冻结的写缓冲（SPEC-1 §1.4.1）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02）。
//
// 容器选择（D1-5 偏差，见 implementation-notes §3）：SPEC-1 原定手写跳表，
// 本项目按用户"优先语言机制"的硬规则改用 std::map——有序、迭代稳定、零自研 BUG。
// 跳表的收益只在写放大与内存布局，属 PERF-01 达标后的优化项；届时若 std::map 成为瓶颈
// 再换，接口不变（find/entries/bytes），换实现不影响上层。
//
// 关键语义：
//   * 墓碑必须进表（value 为空 optional）——Delete 不参与归并会让旧 SST 版本复活；
//   * put 同 key 覆盖：内存表只保留最新版本，flush 前的多次写天然合并；
//   * freeze：flush 时由引擎调用，之后本表只读；Reader 持有 shared_ptr 因此不受后续写影响。

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tinykv {

// 一条内存记录：value 为空表示墓碑
struct MemValue {
    std::optional<std::string> value;
};

class MemTable {
public:
    // 比较器用 std::less<>（异质查找）：find 传 string_view 不必构造临时 string
    using Map = std::map<std::string, MemValue, std::less<>>;

    // 写入或覆盖一条记录；encoded_key 必须是已编码的完整 key（调用方不得自拼前缀）
    // 注意：不能写成 try_emplace(std::move(key), MemValue{std::move(value)})——
    // 第二参无论是否插入都会先被构造，value 会被提前移走，覆盖分支会写入空值。
    void put(std::string encoded_key, std::optional<std::string> value) {
        const std::size_t incoming = entry_bytes(encoded_key, value);
        if (const auto it = map_.find(encoded_key); it != map_.end()) {
            bytes_ -= entry_bytes(it->first, it->second.value);
            it->second.value = std::move(value);
            bytes_ += incoming;
            return;
        }
        map_.emplace(std::move(encoded_key), MemValue{std::move(value)});
        bytes_ += incoming;
    }

    [[nodiscard]] const MemValue* find(std::string_view encoded_key) const {
        const auto it = map_.find(encoded_key);
        return it == map_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const Map& entries() const { return map_; }
    [[nodiscard]] std::size_t bytes() const { return bytes_; }
    [[nodiscard]] bool empty() const { return map_.empty(); }

private:
    // 估算口径：key 长度 + value 长度 + 每条约 64 字节的节点/容器开销
    static std::size_t entry_bytes(const std::string& key, const std::optional<std::string>& v) {
        return key.size() + (v ? v->size() : 0U) + 64U;
    }

    Map map_;
    std::size_t bytes_{0};
};

}  // namespace tinykv
