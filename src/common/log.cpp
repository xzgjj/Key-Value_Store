// 文件用途：log.h 实现——spdlog rotating sink 装配（单文件 ≤10MB、保留 ≤5 个）。
// 归属：TinyKV-CPP（M0 基建，ENG-03）。

#include "common/log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <vector>

namespace tinykv::log {
namespace {

// spdlog 级别映射（本封装枚举 → spdlog 枚举）
spdlog::level::level_enum to_spdlog(Level level) {
    switch (level) {
        case Level::trace:
            return spdlog::level::trace;
        case Level::debug:
            return spdlog::level::debug;
        case Level::info:
            return spdlog::level::info;
        case Level::warn:
            return spdlog::level::warn;
        case Level::err:
            return spdlog::level::err;
        case Level::critical:
            return spdlog::level::critical;
    }
    return spdlog::level::info;  // 防编译器警告的兜底（枚举全覆盖时不可达）
}

constexpr size_t kMaxFileSize = 10ULL * 1024 * 1024;  // 单文件 ≤10MB（CLAUDE.md §4-7）
constexpr size_t kMaxFiles = 5;                       // 保留 ≤5 个
constexpr char kLoggerName[] = "tinykv";              // 全局唯一 logger 名（传参处转 std::string）

std::mutex g_mutex;  // init/shutdown 与并发输出的装配互斥（输出本身由 mt sink 保护）

// 未初始化时安全丢弃（进程早期/测试态直调不崩）
void log_or_drop(spdlog::level::level_enum lvl, const std::string& msg) {
    std::lock_guard lock(g_mutex);
    auto logger = spdlog::get(kLoggerName);
    if (logger != nullptr) {
        logger->log(lvl, msg);
    }
}

}  // namespace

// 装配：目录创建 + rotating 文件 sink（+ 可选控制台镜像）+ flush_on(info)
result<void> init(const std::filesystem::path& log_dir, const std::string& file_base,
                  bool console) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        return std::unexpected(
            IoFailed{.path = log_dir.string(), .reason = "create_directories: " + ec.message()});
    }

    try {
        // 重入语义：先关停旧实例（释放文件句柄、腾出注册名），再装配新实例
        if (auto old = spdlog::get(kLoggerName); old != nullptr) {
            old->flush();
            spdlog::drop(kLoggerName);
        }

        std::vector<spdlog::sink_ptr> sinks;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (log_dir / (file_base + ".log")).string(), kMaxFileSize, kMaxFiles);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(file_sink);
        if (console) {
            // make_shared 显式构造：spdlog 中该名同时是 sink 类型与工厂函数，直接调用有二义
            sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
        }

        auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);  // 审计日志即时落盘
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
    } catch (const spdlog::spdlog_ex& ex) {
        return std::unexpected(
            IoFailed{.path = (log_dir / file_base).string(), .reason = ex.what()});
    }
    return {};
}

bool initialized() {
    std::lock_guard lock(g_mutex);
    return spdlog::get(kLoggerName) != nullptr;
}

void set_level(Level level) {
    std::lock_guard lock(g_mutex);
    auto logger = spdlog::get(kLoggerName);
    if (logger != nullptr) {
        logger->set_level(to_spdlog(level));
    }
}

void trace(const std::string& msg) {
    log_or_drop(spdlog::level::trace, msg);
}
void debug(const std::string& msg) {
    log_or_drop(spdlog::level::debug, msg);
}
void info(const std::string& msg) {
    log_or_drop(spdlog::level::info, msg);
}
void warn(const std::string& msg) {
    log_or_drop(spdlog::level::warn, msg);
}
void error(const std::string& msg) {
    log_or_drop(spdlog::level::err, msg);
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (auto logger = spdlog::get(kLoggerName); logger != nullptr) {
        logger->flush();
        spdlog::drop(kLoggerName);
    }
}

}  // namespace tinykv::log
