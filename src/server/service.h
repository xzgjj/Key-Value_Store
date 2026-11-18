// 文件用途：Service 生命周期契约（M6 第①层：daemon 化的地基，SPEC-0 UX/ENG 延伸）。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 设计：daemon 的一切可启停单元实现 Service；LifecycleManager 按注册顺序 start、
// 逆序 stop（依赖关闭顺序正确性的结构保证）。stop 幂等；start 失败即停已启者并上抛。
// 不做优先级/拓扑声明——顺序即注册顺序（组合根的职责是按正确顺序注册）。

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.h"
#include "common/error_format.h"

namespace tinykv::server {

class Service {
public:
    virtual ~Service() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual result<void> start() = 0;
    // 幂等：未启动时调用返回成功
    [[nodiscard]] virtual result<void> stop() = 0;
};

class LifecycleManager {
public:
    struct Entry {
        std::unique_ptr<Service> service;
        bool started{false};
    };

    // 按启动顺序注册（所有权移交）
    void register_service(std::unique_ptr<Service> service) {
        entries_.push_back(Entry{std::move(service), false});
    }

    // 顺序启动；失败即逆序停掉已启动者并上抛（不留下半启动状态）
    [[nodiscard]] result<void> start_all() {
        for (auto& e : entries_) {
            if (auto r = e.service->start(); !r.has_value()) {
                auto fail = tinykv::unexpected(std::move(r.error()));
                rollback_started();
                return fail;
            }
            e.started = true;
        }
        running_ = true;
        return {};
    }

    // 逆序停止；幂等；逐个收集错误（一个失败不阻断其余关闭），全部错误拼接返回
    [[nodiscard]] result<void> stop_all() {
        if (!running_) {
            return {};
        }
        running_ = false;
        std::string first_err;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (!it->started) {
                continue;
            }
            if (auto r = it->service->stop(); !r.has_value() && first_err.empty()) {
                first_err = std::string(it->service->name()) + ": " +
                            std::string(tinykv::describe_kind(r.error()));
            }
            it->started = false;
        }
        if (!first_err.empty()) {
            return tinykv::unexpected(IoFailed{.path = "lifecycle:stop", .reason = first_err});
        }
        return {};
    }

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    void rollback_started() {
        for (auto rit = entries_.rbegin(); rit != entries_.rend(); ++rit) {
            if (rit->started) {
                (void)rit->service->stop();
                rit->started = false;
            }
        }
        running_ = false;
    }

    std::vector<Entry> entries_;
    bool running_{false};
};

}  // namespace tinykv::server
