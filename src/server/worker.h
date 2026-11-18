// 文件用途：Worker——Raft 泵循环驱动者（M6 第②层：把"接上电的人"补上）。
// 归属：TinyKV-CPP（M6 运行时）。
//
// M2 以来的刻意设计：Raft 状态机是 poll/泵模型（take_ready + 外部 tick/pump），
// ENG-04 禁后台线程——代价是需要驱动者。daemon 的驱动者就是本类：
//   * joinable 线程 + 原子停止标志（绝不 detach，析构前必须 stop）；
//   * 等待用 condition_variable::wait_for（违禁清单禁 sleep_for）；
//   * 泵函数由组合根注入（闭包 tick_all + pump + on_ready）；
//   * cycles 计数是 /status 的可观测点。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "common/error.h"

namespace tinykv::server {

class Worker {
public:
    using PumpFn = std::function<void()>;

    Worker(PumpFn pump, std::chrono::milliseconds interval)
        : pump_(std::move(pump)), interval_(interval) {}

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    ~Worker() {
        (void)stop();  // RAII 兜底：忘了显式 stop 也绝不留下悬空线程
    }

    [[nodiscard]] result<void> start() {
        std::lock_guard lock(mu_);
        if (thread_.joinable()) {
            return tinykv::unexpected(InvalidArgument{"Worker 已启动"});
        }
        stop_.store(false);
        thread_ = std::thread([this] { loop(); });
        return {};
    }

    // 幂等；阻塞至循环退出（joinable 线程，不 detach）
    [[nodiscard]] result<void> stop() {
        {
            std::lock_guard lock(mu_);
            if (!thread_.joinable()) {
                return {};
            }
            stop_.store(true);
        }
        cv_.notify_all();
        thread_.join();
        return {};
    }

    [[nodiscard]] std::uint64_t cycles() const { return cycles_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pump_errors() const {
        return pump_errors_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::string last_error() const {
        std::lock_guard el(err_mu_);
        return last_error_;
    }
    [[nodiscard]] bool running() const { return thread_.joinable() && !stop_.load(); }

private:
    void loop() {
        std::unique_lock cv_lock(cv_mu_);
        while (!stop_.load()) {
            cv_lock.unlock();
            try {
                pump_();  // 泵函数异常不杀死循环——daemon 的心跳必须比任何单次故障活得久
            } catch (const std::exception& e) {
                std::lock_guard el(err_mu_);
                pump_errors_.fetch_add(1, std::memory_order_relaxed);
                last_error_ = e.what();
            } catch (...) {
                std::lock_guard el(err_mu_);
                pump_errors_.fetch_add(1, std::memory_order_relaxed);
                last_error_ = "unknown";
            }
            cycles_.fetch_add(1, std::memory_order_relaxed);
            cv_lock.lock();
            cv_.wait_for(cv_lock, interval_, [&] { return stop_.load(); });
        }
    }

    PumpFn pump_;
    std::chrono::milliseconds interval_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> cycles_{0};
    std::atomic<std::uint64_t> pump_errors_{0};
    mutable std::mutex err_mu_;
    std::string last_error_;
    std::mutex mu_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
};

}  // namespace tinykv::server
