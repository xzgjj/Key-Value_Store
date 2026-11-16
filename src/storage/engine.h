// 文件用途：存储引擎——Storage 的唯一实现：WAL + 内存表 + 只读 SST + Manifest（SPEC-1 §1.3.2）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02、REL-02、UX-05）。
//
// 写路径：write(batch) → 先写 WAL（持久化优先）→ 再改内存表 → 超限则 flush。
//         先 WAL 后内存表是 REL-02 的关键：WAL 失败时内存表未变，不存在"内存有、盘上没有"。
// 读路径：内存表 → SST 由新到旧，首次命中即返回；墓碑命中即"不存在"。
// 恢复：读 Manifest 载入 SST → 重放 WAL → 只读路径，绝不改写任何文件。
// 快照：create_reader() 拷贝当前内存表与 SST 列表，之后 flush 换表不影响该 Reader（红线 3）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/error.h"
#include "storage/memtable.h"
#include "storage/sst.h"
#include "storage/storage.h"
#include "storage/wal.h"

namespace tinykv {

struct EngineOptions {
    std::filesystem::path dir;                  // 数据目录（内含 manifest / wal/ / sst/）
    bool fsync_on_commit = true;                // REL-02 开关；关掉仅供基准对比
    std::size_t memtable_limit_bytes = 64U << 20U;
    std::size_t wal_segment_bytes = 128U << 20U;
};

class Engine final : public Storage {
public:
    [[nodiscard]] static result<std::unique_ptr<Engine>> open(const EngineOptions& opt);

    [[nodiscard]] result<GetResult> get(Cf cf, std::span<const std::byte> key) override;
    [[nodiscard]] result<void> write(std::span<const Modify> batch) override;
    [[nodiscard]] result<std::unique_ptr<Reader>> create_reader() override;

    // 显式关闭：flush 内存表并清理已落盘 WAL；幂等
    [[nodiscard]] result<void> close();

    ~Engine() override;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

private:
    explicit Engine(EngineOptions opt);

    [[nodiscard]] result<void> recover();
    [[nodiscard]] result<void> apply_batch(std::span<const Modify> batch);
    [[nodiscard]] result<void> flush_locked();
    [[nodiscard]] result<std::vector<std::string>> load_manifest() const;
    [[nodiscard]] result<void> write_manifest(const std::vector<std::string>& names) const;

    EngineOptions opt_;
    std::filesystem::path sst_dir_;
    std::filesystem::path wal_dir_;
    std::unique_ptr<Wal> wal_;
    std::shared_ptr<MemTable> mem_;
    std::vector<std::shared_ptr<SstReader>> ssts_;  // 旧 → 新
    std::uint32_t next_sst_seq_{1};
    bool closed_{false};
    mutable std::mutex mu_;
};

}  // namespace tinykv
