// 文件用途：预写日志——追加段文件 + 逐条 CRC32C + fsync 可配 + 只读重放（SPEC-1 §1.4.2/§1.4.3）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-02 / REL-02）。
//
// 记录布局（段文件内顺序追加，一条记录 = 一个写入批次）：
//   [u32 len][u32 crc32c(payload)][payload]
//   payload = [varint 条数] + 每条 [u8 kind][u8 cf][varint klen][key][varint vlen][value]
//   一条记录一次 fwrite，批内原子：半条记录在重放时整批丢弃，回到上一批末尾。
//
// 红线（SPEC-1 §1.5-1）：启动与重放路径**只读不写**，校验失败停在内存里，绝不 truncate 段文件。
// 对照参考项目 KVstorageBaseRaft-cpp 的持久化构造即清空文件——那类实现重启必读空。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/error.h"
#include "storage/storage.h"

namespace tinykv {

struct WalOptions {
    std::filesystem::path dir;               // 段文件目录（引擎的 wal/ 子目录）
    bool fsync_on_commit = true;             // false 仅供基准对比，掉电会丢窗口内数据
    std::size_t segment_bytes = 128U << 20U; // 单段上限，超过则切新段
};

// 重放结果：batches 按写入顺序；stopped=true 表示尾部不完整或校验失败，reason 记录原因
struct WalReplayResult {
    std::vector<ModifyVec> batches;
    bool stopped{false};
    std::uint64_t stopped_offset{0};
    std::string reason;
};

class Wal {
public:
    // 打开（或创建）最新段用于追加；目录不存在则创建
    [[nodiscard]] static result<std::unique_ptr<Wal>> open(const WalOptions& opt);

    // 只读重放全部段：绝不改写文件，遇到坏记录即停
    [[nodiscard]] static result<WalReplayResult> replay(const std::filesystem::path& dir);

    // 删除全部段（仅在 flush 已把数据固化到 SST 之后调用）
    [[nodiscard]] static result<void> clear_all(const std::filesystem::path& dir);

    // 追加一个批次；返回即代表已按 fsync 策略落盘
    [[nodiscard]] result<void> append(std::span<const Modify> batch);

    // 手动刷盘（group commit 的实现点；当前策略为 append 内直接刷）
    [[nodiscard]] result<void> sync();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) = delete;
    Wal& operator=(Wal&&) = delete;
    ~Wal();

private:
    Wal(std::filesystem::path dir, bool fsync_on_commit, std::size_t segment_bytes);

    [[nodiscard]] result<void> open_new_segment();
    [[nodiscard]] result<void> flush_to_disk();

    std::filesystem::path dir_;
    std::filesystem::path current_;
    std::FILE* file_{nullptr};
    bool fsync_on_commit_{true};
    std::size_t segment_bytes_{0};
    std::uint32_t next_index_{1};
    std::uint64_t current_size_{0};
};

}  // namespace tinykv
