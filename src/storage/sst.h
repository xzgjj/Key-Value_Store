// 文件用途：只读有序表（SST）——flush 产物，不可变，附整体 CRC32C 校验（SPEC-1 §1.4.4）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02）。
//
// 文件布局：
//   body   = 逐条 [varint klen][key][u8 kind][varint vlen][value]（key 升序，kind 1 = 墓碑）
//   trailer= [u32 条数][u32 crc32c(body)]
// M1 不做分层压缩（D1-1 定为 L0+L1 两层，compaction 后置），因此读端整表载入内存；
// 表规模由 memtable 上限（默认 64MB）约束，这是刻意接受的简化，性能优化留给 PERF-01 之后。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.h"

namespace tinykv {

// 表内一条记录：value 为空表示墓碑（墓碑必须落表，否则被删的旧版本会复活）
struct SstEntry {
    std::string key;                   // 已编码 key（含 CF 前缀）
    std::optional<std::string> value;
};

class SstReader {
public:
    // 打开并校验：CRC 不匹配或尾部不完整一律 Corrupted，不返回半张表
    [[nodiscard]] static result<std::unique_ptr<SstReader>> open(const std::filesystem::path& path);

    [[nodiscard]] const SstEntry* find(std::string_view key) const;
    [[nodiscard]] const std::vector<SstEntry>& entries() const { return entries_; }

private:
    SstReader() = default;
    std::vector<SstEntry> entries_;
};

class SstWriter {
public:
    [[nodiscard]] static result<std::unique_ptr<SstWriter>> create(const std::filesystem::path& path);

    // 必须按 key 严格升序调用（乱序是上层 bug，直接拒绝而不是静默排序）
    [[nodiscard]] result<void> add(std::string key, std::optional<std::string> value);
    [[nodiscard]] result<void> finish();

    SstWriter(const SstWriter&) = delete;
    SstWriter& operator=(const SstWriter&) = delete;
    SstWriter(SstWriter&&) = delete;
    SstWriter& operator=(SstWriter&&) = delete;
    ~SstWriter();

private:
    SstWriter() = default;
    std::filesystem::path path_;
    std::string body_;
    std::string last_key_;
    std::uint32_t count_{0};
    bool finished_{false};
};

}  // namespace tinykv
