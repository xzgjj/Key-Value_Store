// 文件用途：Worker 泵循环用例（M6 第②层）。
// 归属：TinyKV-CPP（M6 运行时）。
// 使用场景理解：Worker 是"给泵接上电的人"。可判定面：
// (a) start 后循环驱动泵函数（cycles 增长）；(b) stop 阻塞至循环退出且幂等；
// (c) 重复 start 拒绝；(d) 泵函数抛异常不杀死线程（健壮性）。

#include "server/worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>

namespace {

using tinykv::server::Worker;

// 有界等待（ENG-04 禁 sleep_for；yield 自旋 + deadline）
bool spin_until(const std::function<bool()>& pred, int ms = 600) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

TEST(WorkerTest, DrivesPumpLoopAndStops) {
    std::atomic<int> pumps{0};
    Worker worker([&] { pumps.fetch_add(1); }, std::chrono::milliseconds(5));
    ASSERT_TRUE(worker.start().has_value());
    // 轮询等待（ ENG-04 禁 sleep；用 cycles 作为就绪信号 + 有界自旋）
    EXPECT_TRUE(spin_until([&] { return worker.cycles() >= 5; })) << "泵循环未运转";
    ASSERT_TRUE(worker.stop().has_value());
    const auto stopped_at = worker.cycles();
    std::this_thread::yield();
    std::this_thread::yield();
    EXPECT_EQ(worker.cycles(), stopped_at);
}

TEST(WorkerTest, StopIsIdempotentAndDoubleStartRejected) {
    Worker worker([] {}, std::chrono::milliseconds(5));
    EXPECT_TRUE(worker.stop().has_value());   // 未启动即 stop：幂等成功
    ASSERT_TRUE(worker.start().has_value());
    EXPECT_FALSE(worker.start().has_value()) << "重复 start 必须拒绝";
    ASSERT_TRUE(worker.stop().has_value());
    ASSERT_TRUE(worker.stop().has_value());
}

TEST(WorkerTest, PumpExceptionDoesNotKillLoop) {
    std::atomic<int> calls{0};
    Worker worker([&] {
        if (calls.fetch_add(1) == 0) {
            throw std::runtime_error("首次泵抛异常");
        }
    }, std::chrono::milliseconds(5));
    ASSERT_TRUE(worker.start().has_value());
    EXPECT_TRUE(spin_until([&] { return calls.load() >= 3; })) << "泵抛异常后循环必须继续";
    ASSERT_TRUE(worker.stop().has_value());
}

}  // namespace
