// 文件用途：TSO 时间戳分配（M5 第②层，SPEC-4 §4.3.2/红线6）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
//
// 三件套：TsProvider 抽象（调度器兼任实现的接口面）/ MonotonicTsProvider（进程内
// 严格单调分配）/ CheckedTsProvider（fatal 校验装饰器——任何底层返回非严格递增即抛
// TsNotMonotonic 拒绝服务，不变量破坏不进 result 错误通道，CLAUDE.md §7-1）。
// "批量窗口"是调用方语义（store 端按 tso_batch=64 预取）；Provider 本身只保证
// 「单次调用内严格递增 + 跨调用不回退」两条不变量。

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "common/error.h"
#include "common/ids.h"

namespace tinykv::tso {

class TsProvider {
public:
    virtual ~TsProvider() = default;

    // 一次授 n 个 ts：调用内严格递增；与此前任何调用相比不回退（红线6）
    [[nodiscard]] virtual result<std::vector<Ts>> get_timestamps(std::size_t n) = 0;
};

// 进程内 TSO：物理时间基线 + 逻辑计数（Percolator 同款布局：ts = physical_ms << 18 | logical）。
// 为什么必须带物理基线：纯计数器在重启后从 0 重新计数，新事务的 start_ts 会小于盘上所有
// commit_ts——MVCC 只认 commit_ts ≤ start_ts 的版本，于是历史数据全部读不到（静默空值），
// 而 latest_commit_bound > start_ts 让任何写都命中 WriteConflict（事务面实际不可写）。
// physical=false 保留纯计数器模式：测试要确定性、要小 ts 值（Ts{1000} 这类断言依赖它）。
class MonotonicTsProvider final : public TsProvider {
public:
    explicit MonotonicTsProvider(bool physical = false) : physical_(physical) {}

    [[nodiscard]] result<std::vector<Ts>> get_timestamps(std::size_t n) override {
        if (n == 0) {
            return std::unexpected(InvalidArgument{"批量授时 n 必须 > 0"});
        }
        std::lock_guard lock(mu_);
        std::vector<Ts> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(Ts{next_locked()});
        }
        return out;
    }

    // 已分配到的最大 ts（诊断用）
    [[nodiscard]] Ts current() const {
        std::lock_guard lock(mu_);
        return Ts{counter_};
    }

    // 重启恢复：保证后续分配严格大于盘上已存在的最大 ts（提交位点或锁的 start_ts）。
    // 只在启动装配期调用一次；运行期调用等价于"强制跳过一段 ts"，语义安全但浪费。
    void raise_floor(Ts floor) {
        std::lock_guard lock(mu_);
        if (floor.value > counter_) {
            counter_ = floor.value;
        }
    }

private:
    static constexpr int kLogicalBits = 18;   // 低 18 位逻辑计数（Percolator：每毫秒 26 万个）

    [[nodiscard]] std::uint64_t next_locked() {
        std::uint64_t base = counter_ + 1;
        if (physical_) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            const auto physical = (static_cast<std::uint64_t>(ms) << kLogicalBits);
            if (physical > base) {
                base = physical;
            }
        }
        counter_ = base;
        return counter_;
    }

    mutable std::mutex mu_;
    bool physical_{false};
    std::uint64_t counter_{0};
};

// fatal 校验装饰器：包住任何 TsProvider，破坏严格单调即抛 TsNotMonotonic。
// 抛出即拒绝服务——修一个"会回退的 TSO"比修"用回退 ts 写出的脏数据"便宜得多。
class CheckedTsProvider final : public TsProvider {
public:
    explicit CheckedTsProvider(TsProvider& inner) : inner_(inner) {}

    [[nodiscard]] result<std::vector<Ts>> get_timestamps(std::size_t n) override {
        auto r = inner_.get_timestamps(n);
        if (!r) {
            return r;
        }
        std::lock_guard lock(mu_);
        for (const auto ts : *r) {
            if (ts.value <= last_) {
                throw TsNotMonotonic(ts.value, last_);
            }
            last_ = ts.value;
        }
        return r;
    }

private:
    TsProvider& inner_;
    mutable std::mutex mu_;
    std::uint64_t last_{0};
};

}  // namespace tinykv::tso
