// 文件用途：TSO 用例（M5 第②层，FUNC-16，SPEC-4 §3.7 tso_test）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：TSO 是 SI 的时钟根基，红线6 要求"严格单调，绝不回退/重复"。
// 必须证明：(a) 单次批量内严格递增；(b) 跨调用不回退；(c) 空批量拒绝；
// (d) 底层返回非单调时 CheckedTsProvider 抛 fatal 异常（拒绝服务而非写脏数据）；
// (e) 批量窗口吞吐 ≥1e5 ts/s（FUNC-16 的量级要求）。

#include "scheduler/tso.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "scheduler/scheduler.h"

namespace {

using tinykv::InvalidArgument;
using tinykv::Ts;
using tinykv::tso::CheckedTsProvider;
using tinykv::tso::MonotonicTsProvider;

// 严格递增校验助手
bool strictly_increasing(const std::vector<Ts>& ts) {
    for (std::size_t i = 1; i < ts.size(); ++i) {
        if (ts[i].value <= ts[i - 1].value) {
            return false;
        }
    }
    return true;
}

TEST(TsoTest, BatchStrictlyIncreasing) {
    MonotonicTsProvider tso;
    auto r = tso.get_timestamps(64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 64U);
    EXPECT_TRUE(strictly_increasing(*r));
}

TEST(TsoTest, MonotonicAcrossCalls) {
    MonotonicTsProvider tso;
    auto a = tso.get_timestamps(64);
    auto b = tso.get_timestamps(64);
    auto c = tso.get_timestamps(1);
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    EXPECT_LT(a->back().value, b->front().value) << "跨调用不得回退";
    EXPECT_LT(b->back().value, c->front().value);
    EXPECT_EQ(tso.current().value, c->back().value);
}

TEST(TsoTest, ZeroBatchRejected) {
    MonotonicTsProvider tso;
    auto r = tso.get_timestamps(0);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(r.error()));
}

// 底层破坏严格单调 → fatal 异常（拒绝服务，不进 result 通道）
TEST(TsoTest, FatalOnNonMonotonic) {
    class Broken final : public tinykv::tso::TsProvider {
        [[nodiscard]] tinykv::result<std::vector<Ts>> get_timestamps(std::size_t n) override {
            std::vector<Ts> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(Ts{10 - static_cast<std::uint64_t>(i)});   // 首个之后全部回退
            }
            return out;
        }
    };
    Broken broken;
    CheckedTsProvider checked(broken);
    EXPECT_THROW((void)checked.get_timestamps(3), tinykv::TsNotMonotonic);
}

// 调度器兼任 TSO：适配器可用且同样单调（SPEC-4 架构）
TEST(TsoTest, SchedulerBackedProviderMonotonic) {
    tinykv::scheduler::Scheduler sch;
    tinykv::scheduler::SchedulerTsProvider tso(sch);
    auto a = tso.get_timestamps(16);
    auto b = tso.get_timestamps(16);
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_TRUE(strictly_increasing(*a));
    EXPECT_LT(a->back().value, b->front().value);
}

// BUG-0017: code-review P0-2——重启恢复必须抬高 TSO 下限。
// 使用场景：重启前盘上最大 commit_ts = 1000，重启后若从 1 重新计数，
// start_ts < 1000 会让历史版本全部读不到（静默空值）、任何写都命中 WriteConflict。
TEST(TsoTest, RaiseFloorKeepsAllocationsAboveRestoredMax) {
    MonotonicTsProvider tso;
    tso.raise_floor(Ts{1000});
    auto after = tso.get_timestamps(3);
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(after->front().value, 1000U) << "重启后新 start_ts 不得小于盘上最大 ts";
    EXPECT_TRUE(strictly_increasing(*after));
}

// 物理基线模式：ts 高位来自墙钟毫秒，重启后天然大于历史值（即使不显式 raise_floor）
TEST(TsoTest, PhysicalModeEncodesWallClockInHighBits) {
    MonotonicTsProvider tso(/*physical=*/true);
    auto a = tso.get_timestamps(1);
    ASSERT_TRUE(a.has_value());
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    EXPECT_EQ(a->front().value >> 18, now_ms) << "高位必须是物理毫秒（Percolator 布局）";
    auto b = tso.get_timestamps(1);
    ASSERT_TRUE(b.has_value());
    EXPECT_GT(b->front().value, a->front().value);
}

// 批量窗口吞吐：FUNC-16 要求 ≥1e5 ts/s（纯计数器，机械下限证明）
TEST(TsoTest, BatchWindowThroughput) {
    MonotonicTsProvider tso;
    constexpr std::size_t kTotal = 200'000;
    constexpr std::size_t kWindow = 64;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t allocated = 0;
    while (allocated < kTotal) {
        auto r = tso.get_timestamps(kWindow);
        ASSERT_TRUE(r.has_value());
        allocated += r->size();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double per_sec = static_cast<double>(allocated) / secs;
    std::printf("[bench] TSO %.0f ts/s（%zu ts in %.3f s，window=%zu）\n", per_sec, allocated,
                secs, kWindow);
    EXPECT_GE(per_sec, 100'000.0) << "TSO 吞吐低于 FUNC-16 的 1e5 ts/s";
}

}  // namespace
