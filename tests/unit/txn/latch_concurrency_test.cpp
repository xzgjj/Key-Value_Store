// 文件用途：Latch 并发用例（M5 第②层，FUNC-15，SPEC-4 §3.7 latch_concurrency_test）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
// 使用场景理解：Latch 是"server 侧同 key 严格串行"的观察点（白皮书：测不出但语义必需）。
// 必须证明：(a) 并发同 key 获取严格互斥（临界区无重叠）；
// (b) 交叉 key 集（T1{a,b} vs T2{b,a}）无死锁、双方都能完成；
// (c) guard 析构释放并唤醒等待者；(d) 重复 key 在单次 acquire 内去重不自我死锁。

#include "txn/latch.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using tinykv::txn::LatchManager;

// 并发同 key：N 线程 × M 次获取同一 key，临界区（counter 自增）必须无重叠
TEST(LatchConcurrency, SerializesSameKey) {
    LatchManager latches;
    constexpr int kThreads = 4;
    constexpr int kIters = 500;
    std::atomic<int> counter{0};
    std::atomic<bool> violated{false};   // 观察点：同一时刻若有两人持有即违规
    std::atomic<bool> inside{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                auto guard = latches.acquire(std::vector<std::string>{"hot-key"});
                bool expect_free = false;
                if (!inside.compare_exchange_strong(expect_free, true)) {
                    violated.store(true);
                }
                const int v = counter.load();
                std::this_thread::yield();   // 放大窗口
                counter.store(v + 1);
                inside.store(false);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_FALSE(violated.load()) << "同 key 临界区出现重叠——Latch 未串行化";
    EXPECT_EQ(counter.load(), kThreads * kIters);
}

// 交叉 key 集：T1{a,b} vs T2{b,a}——排序加锁保证无死锁，双方都能完成
TEST(LatchConcurrency, CrossOrderDeadlockFree) {
    LatchManager latches;
    std::atomic<int> done{0};

    std::thread t1([&] {
        for (int i = 0; i < 200; ++i) {
            auto guard = latches.acquire(std::vector<std::string>{"a", "b"});
            (void)guard;
            std::this_thread::yield();
        }
        ++done;
    });
    std::thread t2([&] {
        for (int i = 0; i < 200; ++i) {
            auto guard = latches.acquire(std::vector<std::string>{"b", "a"});   // 乱序提交，manager 内部排序
            (void)guard;
            std::this_thread::yield();
        }
        ++done;
    });
    t1.join();
    t2.join();
    EXPECT_EQ(done.load(), 2) << "交叉 key 集出现死锁";
}

// guard 析构释放：等待者必须被放行（join 是同步点——waiter 能返回即证明被唤醒）
TEST(LatchConcurrency, GuardReleaseWakesWaiter) {
    LatchManager latches;
    auto holder = latches.acquire(std::vector<std::string>{"k"});
    ASSERT_TRUE(holder.holds_key("k"));

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        auto guard = latches.acquire(std::vector<std::string>{"k"});   // 阻塞至释放
        acquired.store(guard.holds_key("k"));
    });

    { auto release = std::move(holder); }   // RAII 析构释放 → 唤醒等待者
    waiter.join();                           // 无限等待即证明唤醒失败
    EXPECT_TRUE(acquired.load());
}

// 同一次 acquire 内重复 key：去重处理（未去重 = 重复 lock 同一 mutex = 未定义行为/自锁）
TEST(LatchConcurrency, DuplicateKeysDeduped) {
    LatchManager latches;
    auto guard = latches.acquire(std::vector<std::string>{"a", "a", "b", "a"});
    EXPECT_TRUE(guard.holds_key("a"));
    EXPECT_TRUE(guard.holds_key("b"));
    { auto release = std::move(guard); }   // 显式小作用域：出块即释放（局部变量活到作用域尾）
    auto again = latches.acquire(std::vector<std::string>{"b", "a"});
    EXPECT_TRUE(again.holds_key("a") && again.holds_key("b"));
}

}  // namespace
