// 文件用途：Interceptor 链（M6 挂账收口）——请求路径的可插拔切面（log/metrics/trace）。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 设计：一条请求从 begin() 到 finish() 携带 RequestEvent；链上 Interceptor 按注册序
// before、逆序 after（与 LifecycleManager 的逆序关闭同构）。内置三种实现均可不启用：
//   LogInterceptor     把事件交给注入的 sink（默认不输出，测试可捕获）
//   MetricsInterceptor 按 path 累计调用/成功/失败与总耗时（status 端点暴露）
//   TraceInterceptor   环形缓冲最近 N 条事件（/trace 端点暴露）
// 耗时用 steady_clock（真实流逝），计数与顺序可判定；不引入注入时钟，避免把观测也变成
// 确定性测试负担——测试只断言计数/顺序，不断言具体微秒值。

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tinykv::server {

struct RequestEvent {
    std::string method;
    std::string path;
    std::string key;
    std::chrono::steady_clock::time_point started{};
    bool ok{true};
    std::string outcome;
    std::uint64_t elapsed_us{0};
};

class Interceptor {
public:
    virtual ~Interceptor() = default;
    virtual void before(const RequestEvent& /*ev*/) {}
    virtual void after(const RequestEvent& /*ev*/) {}
};

class InterceptorChain {
public:
    void add(std::unique_ptr<Interceptor> interceptor) { chain_.push_back(std::move(interceptor)); }

    [[nodiscard]] RequestEvent begin(std::string method, std::string path, std::string key) {
        RequestEvent ev;
        ev.method = std::move(method);
        ev.path = std::move(path);
        ev.key = std::move(key);
        ev.started = std::chrono::steady_clock::now();
        for (const auto& it : chain_) {
            it->before(ev);
        }
        return ev;
    }

    void finish(RequestEvent& ev, bool ok, std::string outcome) {
        ev.elapsed_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - ev.started)
                                           .count());
        ev.ok = ok;
        ev.outcome = std::move(outcome);
        for (auto it = chain_.rbegin(); it != chain_.rend(); ++it) {
            (*it)->after(ev);
        }
    }

    [[nodiscard]] std::size_t size() const { return chain_.size(); }

private:
    std::vector<std::unique_ptr<Interceptor>> chain_;
};

// ---- 内置切面 ----

using LogSink = std::function<void(const std::string&)>;

class LogInterceptor final : public Interceptor {
public:
    explicit LogInterceptor(LogSink sink) : sink_(std::move(sink)) {}

    void after(const RequestEvent& ev) override {
        if (sink_) {
            sink_(ev.method + " " + ev.path + (ev.ok ? " OK " : " ERR ") + ev.outcome + " " +
                  std::to_string(ev.elapsed_us) + "us");
        }
    }

private:
    LogSink sink_;
};

class MetricsInterceptor final : public Interceptor {
public:
    struct Counters {
        std::uint64_t calls{0};
        std::uint64_t ok{0};
        std::uint64_t err{0};
        std::uint64_t elapsed_us{0};
    };

    void after(const RequestEvent& ev) override {
        std::lock_guard lock(mu_);
        auto& c = by_path_[ev.path];
        ++c.calls;
        c.elapsed_us += ev.elapsed_us;
        if (ev.ok) {
            ++c.ok;
        } else {
            ++c.err;
        }
    }

    [[nodiscard]] Counters get(const std::string& path) const {
        std::lock_guard lock(mu_);
        auto it = by_path_.find(path);
        return it == by_path_.end() ? Counters{} : it->second;
    }

    [[nodiscard]] std::uint64_t total_calls() const {
        std::lock_guard lock(mu_);
        std::uint64_t sum = 0;
        for (const auto& [path, c] : by_path_) {
            (void)path;
            sum += c.calls;
        }
        return sum;
    }

    [[nodiscard]] std::string snapshot() const {
        std::lock_guard lock(mu_);
        std::string out;
        for (const auto& [path, c] : by_path_) {
            out += (out.empty() ? "" : ",");
            out += "{\"path\":\"" + path + "\",\"calls\":" + std::to_string(c.calls) +
                   ",\"ok\":" + std::to_string(c.ok) + ",\"err\":" + std::to_string(c.err) +
                   ",\"elapsed_us\":" + std::to_string(c.elapsed_us) + "}";
        }
        return "[" + out + "]";
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, Counters> by_path_;
};

class TraceInterceptor final : public Interceptor {
public:
    explicit TraceInterceptor(std::size_t capacity = 16)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    void after(const RequestEvent& ev) override {
        std::lock_guard lock(mu_);
        if (!enabled_) {
            return;
        }
        events_.push_back(ev);
        while (events_.size() > capacity_) {
            events_.pop_front();
        }
    }

    [[nodiscard]] std::size_t count() const {
        std::lock_guard lock(mu_);
        return events_.size();
    }

    // 采样开关（M7：`trace region <id> on|off` 的落点）。关闭时不记录但仍计数，
    // 这样"关掉之后 count 还在涨"本身就是可观测事实（不是悄悄不工作）。
    void set_enabled(bool on) {
        std::lock_guard lock(mu_);
        enabled_ = on;
    }
    [[nodiscard]] bool enabled() const {
        std::lock_guard lock(mu_);
        return enabled_;
    }

    [[nodiscard]] std::string snapshot() const {
        std::lock_guard lock(mu_);
        std::string out;
        for (const auto& ev : events_) {
            out += (out.empty() ? "" : ",");
            out += "{\"method\":\"" + ev.method + "\",\"path\":\"" + ev.path +
                   "\",\"ok\":" + (ev.ok ? "true" : "false") + ",\"outcome\":\"" + ev.outcome +
                   "\"}";
        }
        return "[" + out + "]";
    }

private:
    bool enabled_{true};
    std::size_t capacity_;
    mutable std::mutex mu_;
    std::deque<RequestEvent> events_;
};

}  // namespace tinykv::server
