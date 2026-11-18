// 文件用途：server 侧 key 粒度 Latch（M5 第②层，SPEC-4 §4.3.2/红线4）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
//
// 白皮书原文语义："测不出但语义必需"——并发同 key 提案在 server 侧必须严格串行，
// 否则同一 raft 组里两个事务的 prewrite/commit 交错 apply 会产生不可判定的中间态。
// 无死锁保证：acquire 按排序后的 key 顺序逐个加锁（全局全序），交叉持有不可能成环。
// RAII：LatchGuard 析构逆序释放；持有至 apply 完成（上层约定）。

#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/compat.h"
#include "common/error.h"

namespace tinykv::txn {

class LatchManager {
    struct Slot {  // 槽类型前置定义（guard 析构需要完整类型释放 shared_ptr）
        std::mutex mu;
    };

public:
    class LatchGuard {
    public:
        LatchGuard() = default;
        LatchGuard(LatchGuard&& other) noexcept
            : slots_(std::move(other.slots_)), keys_(std::move(other.keys_)) {}
        LatchGuard& operator=(LatchGuard&&) = delete;
        LatchGuard(const LatchGuard&) = delete;
        LatchGuard& operator=(const LatchGuard&) = delete;

        ~LatchGuard() {
            // 逆序释放：对称于加锁顺序，习惯性防错（全序下顺序无关，但保持对称）
            for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
                (*it)->mu.unlock();
            }
        }

        // 观察点（latch_concurrency_test 用）：本 guard 是否持有该 key
        [[nodiscard]] bool holds_key(std::string_view key) const {
            return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
        }

        // keys_ 构造后不可变：holds_key 免锁直读（观察点）
        LatchGuard(std::vector<std::shared_ptr<Slot>> slots, std::vector<std::string> keys)
            : slots_(std::move(slots)), keys_(std::move(keys)) {}

        std::vector<std::shared_ptr<Slot>> slots_;  // 已加锁的槽（按 key 排序）
        std::vector<std::string> keys_;
    };

    // 取得全部 key 的 latch：阻塞至每个 key 都空闲；key 排序后加锁（防死锁，红线4）
    [[nodiscard]] LatchGuard acquire(tinykv::span<const std::string> keys) {
        std::vector<std::string> sorted(keys.begin(), keys.end());
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

        std::vector<std::shared_ptr<Slot>> slots;
        slots.reserve(sorted.size());
        {
            std::lock_guard lock(mu_);  // 只保护槽表本身；槽内 mutex 在锁外逐个取
            for (const auto& key : sorted) {
                auto& slot = slots_[key];
                if (!slot) {
                    slot = std::make_shared<Slot>();
                }
                slots.push_back(slot);
            }
        }
        for (auto& slot : slots) {
            slot->mu.lock();  // 全序加锁：不可能成环（无死锁的结构性保证）
        }
        return LatchGuard(std::move(slots), std::move(sorted));
    }

private:
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Slot>> slots_;
};

}  // namespace tinykv::txn
