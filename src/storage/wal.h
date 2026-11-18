// 文件用途：预写日志——追加段文件 + 逐条 CRC32C + group commit（批量 fsync）+ 只读重放（SPEC-1 §1.4.2/§1.4.3）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-02 / REL-02；M2 PERF-01 收口）。
//
// 记录布局（段文件内顺序追加，一条记录 = 一个写入批次）：
//   [u32 len][u32 crc32c(payload)][payload]
//   payload = [varint 条数] + 每条 [u8 kind][u8 cf][varint klen][key][varint vlen][value]
//   一条记录一次 fwrite，批内原子：半条记录在重放时整批丢弃，回到上一批末尾。
//
// group commit（延迟确认窗口，用户 2026-11-16 选定）：
//   append 内 fwrite + fflush 把记录推进内核页缓存后即返回（不等 fsync）；
//   后台提交者在 commit_window 窗口内把多条写合并成一次 fsync。
//   持久边界：进程级崩溃（_Exit/kill -9）因页缓存仍在 → 不丢；机器掉电可能丢窗口内已确认写。
//   窗口准确语义与认知修正见 implementation-notes.md §6.3。
//
// 红线（SPEC-1 §1.5-1）：启动与重放路径**只读不写**，校验失败停在内存里，绝不 truncate 段文件。
// 对照参考项目 KVstorageBaseRaft-cpp 的持久化构造即清空文件——那类实现重启必读空。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/error.h"
#include "storage/storage.h"

namespace tinykv {

struct WalOptions {
    std::filesystem::path dir;                          // 段文件目录（引擎的 wal/ 子目录）
    bool fsync_on_commit = true;                        // false 仅供基准对比，掉电会丢窗口内数据
    std::size_t segment_bytes = 128U << 20U;            // 单段上限，超过则切新段
    std::chrono::milliseconds commit_window{2};         // group commit 攒批窗口（SPEC-2 D2-2）
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
    // 打开（或创建）最新段用于追加，并启动后台提交者；目录不存在则创建
    [[nodiscard]] static result<std::unique_ptr<Wal>> open(const WalOptions& opt);

    // 只读重放全部段：绝不改写文件，遇到坏记录即停
    [[nodiscard]] static result<WalReplayResult> replay(const std::filesystem::path& dir);

    // 删除全部段（仅在 flush 已把数据固化到 SST 之后调用）
    [[nodiscard]] static result<void> clear_all(const std::filesystem::path& dir);

    // 追加一个批次：fwrite+fflush 后即返回；fsync 由后台提交者在本窗口内合并执行
    [[nodiscard]] result<void> append(std::span<const Modify> batch);

    // 同步刷盘原语（fflush+fsync）：析构、切段、测试与关闭路径使用
    [[nodiscard]] result<void> sync();

    // 累计 fsync 次数（含批量与同步原语），供基准/测试观测合并效果
    [[nodiscard]] std::uint64_t fsync_count() const noexcept { return fsync_count_.load(); }

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) = delete;
    Wal& operator=(Wal&&) = delete;
    ~Wal();

private:
    Wal(std::filesystem::path dir, bool fsync_on_commit, std::size_t segment_bytes,
        std::chrono::milliseconds commit_window);

    [[nodiscard]] result<void> open_new_segment();
    [[nodiscard]] result<void> sync_locked();   // 持 mu_ 时调用：强制 fflush+fsync 当前段
    // 后台提交者落盘失败 → 粘住错误（后续 append 一律 IoFailed），不再空转重试（code-review P1-1）
    void mark_failed_locked(std::string_view reason);
    void commit_loop();                         // 后台提交者：窗口内合并 fsync

    std::filesystem::path dir_;
    std::filesystem::path current_;
    std::FILE* file_{nullptr};
    bool fsync_on_commit_{true};
    std::size_t segment_bytes_{0};
    std::uint32_t next_index_{1};
    std::uint64_t current_size_{0};

    std::chrono::milliseconds commit_window_{2};
    std::mutex mu_;            // 保护 file_/current_size_ 与写路径
    std::mutex commit_mu_;     // 提交者等待窗口用；notify 前须持有它，避免唤醒丢失
    std::condition_variable cv_;
    std::thread committer_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<std::uint64_t> fsync_count_{0};
    std::atomic<bool> failed_{false};   // 粘性失败：落盘链路已不可信（code-review P1-1）
    std::string fail_reason_;           // 受 mu_ 保护
};

}  // namespace tinykv
