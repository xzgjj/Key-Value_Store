// 文件用途：region 级 trace 开关与专属日志（M4 第⑦b层，UX-04，docs/cli.md §10.3）。
// 归属：TinyKV-CPP（M4 集成层）。
//
// 约定（CLAUDE.md §4 / docs/cli.md §10.3）：trace 日志写 `log/trace-region-<id>.log`，
// spdlog rotating——单文件 ≤10MB、保留 ≤5 个，与全局 logger 同规格。
// 未开启的 region 调 emit 直接丢弃（零开销路径）；关闭只置位不销毁 logger，
// 重复开关不重复建 sink（rotating_file_sink 会 truncate 索引，重复建会绕过保留窗口）。

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string_view>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace tinykv::server {

class RegionTrace {
public:
    explicit RegionTrace(std::filesystem::path dir) : dir_(std::move(dir)) {}

    // on：确保该 region 的 rotating logger 存在并记一条开启事件；off：置位关闭
    void set(std::uint64_t region_id, bool on) {
        std::lock_guard lock(mu_);
        if (on) {
            ensure_logger_locked(region_id);
            enabled_[region_id] = true;
            loggers_[region_id]->info("trace on（region {}）", region_id);
        } else {
            const bool was = enabled_[region_id];
            enabled_[region_id] = false;
            if (was) {
                ensure_logger_locked(region_id);
                loggers_[region_id]->info("trace off（region {}）", region_id);
            }
        }
    }

    [[nodiscard]] bool enabled(std::uint64_t region_id) const {
        std::lock_guard lock(mu_);
        const auto it = enabled_.find(region_id);
        return it != enabled_.end() && it->second;
    }

    // 写一条 region 粒度 trace；未开启时丢弃（调用方无需判空）
    void emit(std::uint64_t region_id, std::string_view line) {
        std::lock_guard lock(mu_);
        if (!enabled(region_id)) {
            return;
        }
        ensure_logger_locked(region_id);
        loggers_[region_id]->info("{}", line);
    }

private:
    // 调用方持锁：logger 惰性装配（rotating 10MB×5，与全局日志同规格）
    void ensure_logger_locked(std::uint64_t region_id) {
        if (loggers_.count(region_id) != 0) {
            return;
        }
        const auto path = dir_ / ("trace-region-" + std::to_string(region_id) + ".log");
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path.string(), 10ULL * 1024 * 1024, 5);
        auto logger = std::make_shared<spdlog::logger>(
            "trace-region-" + std::to_string(region_id), std::move(sink));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S] %v");
        loggers_[region_id] = std::move(logger);
    }

    std::filesystem::path dir_;
    mutable std::mutex mu_;
    std::map<std::uint64_t, std::shared_ptr<spdlog::logger>> loggers_;
    std::map<std::uint64_t, bool> enabled_;
};

}  // namespace tinykv::server
