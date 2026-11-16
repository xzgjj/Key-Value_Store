// 文件用途：log.h 实现——spdlog rotating sink 装配 + 无锁读路径 + 惰性格式化落地
//          （单文件 ≤10MB、保留 ≤5 个）。
// 归属：TinyKV-CPP（M0 基建，ENG-03；2026-11-16 热路径重构）。

#include "common/log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tinykv::log {
namespace {

constexpr size_t kMaxFileSize = 10ULL * 1024 * 1024;  // 单文件 ≤10MB（CLAUDE.md §4-7）
constexpr size_t kMaxFiles = 5;                       // 保留 ≤5 个
constexpr char kLoggerName[] = "tinykv";              // 全局唯一 logger 名
constexpr char kPattern[] = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";  // 文件与控制台同一格式

// 只保护装配（init/shutdown）；输出路径不碰这把锁
std::mutex g_init_mutex;

// 发布点：原子持有的 logger 句柄。读者 acquire 一次即可，无需全局互斥。
std::atomic<std::shared_ptr<spdlog::logger>> g_logger{};

// 级别阈值镜像：enabled() 用它做无锁短路，避免为了一次判断去碰 logger 内部状态
std::atomic<int> g_threshold{static_cast<int>(spdlog::level::info)};

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
    // 枚举已全覆盖：此处不可达。用 unreachable 而非兜底 return，保住编译器对
    // "新增枚举成员未处理"的穷尽性告警（兜底 return 会把它静默掉）。
    std::unreachable();
}

}  // namespace

namespace detail {

// 惰性入口的落地：text 已由调用方在级别放行后格式化好
void write_text(Level level, std::string text) {
    auto logger = g_logger.load(std::memory_order_acquire);
    if (logger == nullptr) {
        return;  // 未初始化：安全丢弃（进程早期/测试态）
    }
    logger->log(to_spdlog(level), text);
}

}  // namespace detail

// 装配：目录创建 + rotating 文件 sink（+ 可选控制台镜像）+ flush_on(info)
result<void> init(const std::filesystem::path& log_dir, const std::string& file_base,
                  bool console) {
    std::lock_guard lock(g_init_mutex);
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        return std::unexpected(
            IoFailed{.path = log_dir.string(), .reason = "create_directories: " + ec.message()});
    }

    try {
        // 重入语义：先关停旧实例（释放文件句柄、腾出注册名），再装配新实例
        if (auto old = g_logger.exchange(nullptr); old != nullptr) {
            old->flush();
            spdlog::drop(kLoggerName);
        }

        std::vector<spdlog::sink_ptr> sinks;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (log_dir / (file_base + ".log")).string(), kMaxFileSize, kMaxFiles);
        file_sink->set_pattern(kPattern);
        sinks.push_back(file_sink);
        if (console) {
            // make_shared 显式构造：spdlog 中该名同时是 sink 类型与工厂函数，直接调用有二义
            auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            console_sink->set_pattern(kPattern);  // 与文件同格式，便于日志与终端对照
            sinks.push_back(console_sink);
        }

        auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);  // 审计日志即时落盘
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);

        // 发布顺序：先阈值后句柄。读者以句柄非空作为"已初始化"判据，
        // 因此必须先让阈值就位，否则可能出现"已初始化但阈值仍是默认值"的窗口。
        g_threshold.store(static_cast<int>(spdlog::level::info), std::memory_order_relaxed);
        g_logger.store(std::move(logger), std::memory_order_release);
    } catch (const spdlog::spdlog_ex& ex) {
        return std::unexpected(
            IoFailed{.path = (log_dir / file_base).string(), .reason = ex.what()});
    }
    return {};
}

bool initialized() {
    return g_logger.load(std::memory_order_acquire) != nullptr;
}

void set_level(Level level) {
    std::lock_guard lock(g_init_mutex);
    if (auto logger = g_logger.load(std::memory_order_relaxed); logger != nullptr) {
        logger->set_level(to_spdlog(level));
        g_threshold.store(static_cast<int>(to_spdlog(level)), std::memory_order_relaxed);
    }
}

bool enabled(Level level) {
    if (g_logger.load(std::memory_order_acquire) == nullptr) {
        return false;  // 未初始化：连格式化都不做
    }
    return static_cast<int>(to_spdlog(level)) >= g_threshold.load(std::memory_order_relaxed);
}

void trace(const std::string& msg) {
    detail::write_text(Level::trace, msg);
}
void debug(const std::string& msg) {
    detail::write_text(Level::debug, msg);
}
void info(const std::string& msg) {
    detail::write_text(Level::info, msg);
}
void warn(const std::string& msg) {
    detail::write_text(Level::warn, msg);
}
void error(const std::string& msg) {
    detail::write_text(Level::err, msg);
}

void shutdown() {
    std::lock_guard lock(g_init_mutex);
    if (auto logger = g_logger.exchange(nullptr); logger != nullptr) {
        logger->flush();
        spdlog::drop(kLoggerName);
    }
}

}  // namespace tinykv::log
