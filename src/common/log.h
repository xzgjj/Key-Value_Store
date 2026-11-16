// 文件用途：spdlog 封装——全局日志统一入口（轮转 10MB×5，落根 log/ 目录，CLAUDE.md §4-7）。
// 归属：TinyKV-CPP（M0 基建，ENG-03；2026-11-16 热路径重构）。
//
// 约定：
//   * init 一次装配；未 init 前调用任何级别都安全丢弃（进程早期/测试态可直调）；
//   * 读路径无锁：logger 句柄缓存在 atomic<shared_ptr>，输出去掉全局互斥
//     （原实现持全局锁做文件 I/O，多线程下会把所有日志串行化）；g_mutex 只保护装配过程；
//   * 两级入口：fmt 变参版做惰性格式化（级别关闭时不格式化、不分配），
//     std::string 版供已经拼好的文本（避免二次格式化）；
//     **注意边界**：C++ 的实参永远在调用点求值，模板只能省"格式化+分配"，省不掉
//     "构造实参"。昂贵实参请在调用点用 enabled(level) 显式短路。
//   * flush_on(info)：审计日志即时落盘（写放大调优属 M1 WAL group commit 课题）；
//   * 同步 logger，无后台线程（ENG-04：禁 detach 线程）。
//
// 依赖说明：头文件 include <spdlog/common.h> 只为拿到 format_string_t 与 fmt_lib——
// 惰性格式化的代价就是把 fmt 的类型带到调用方；logger 的完整定义仍只出现在 .cpp 里。

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/common.h>

#include "common/error.h"

namespace tinykv::log {

// 日志级别（封装 spdlog 枚举，避免调用方直接依赖 spdlog 头）
enum class Level { trace, debug, info, warn, err, critical };

// 初始化全局 logger：目录不存在则创建；console 控制台镜像开关；返回 IoFailed 失败原因
[[nodiscard]] result<void> init(const std::filesystem::path& log_dir,
                                const std::string& file_base = "tinykv", bool console = true);

// 是否已初始化（测试与重启场景判定）
[[nodiscard]] bool initialized();

// 运行期调级
void set_level(Level level);

// 当前级别是否放行：调用方在构造昂贵参数前先问一句，可省掉一次格式化
[[nodiscard]] bool enabled(Level level);

namespace detail {

// 惰性入口的落地函数（在 .cpp 中实现）：text 已格式化，未初始化时安全丢弃
void write_text(Level level, std::string text);

// 模板共用的转发：级别关闭时连 format 都不做（惰性求值的关键在这一行）
template <class... Args>
void emit(Level level, spdlog::format_string_t<Args...> fmt, Args&&... args) {
    if (!enabled(level)) {
        return;
    }
    write_text(level, spdlog::fmt_lib::format(fmt, std::forward<Args>(args)...));
}

}  // namespace detail

// ---- 惰性入口（推荐）：log::info("region {} applied at {}", region.value, index.value) ----
template <class... Args>
void trace(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    detail::emit(Level::trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    detail::emit(Level::debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    detail::emit(Level::info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    detail::emit(Level::warn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    detail::emit(Level::err, fmt, std::forward<Args>(args)...);
}

// ---- 已格式化入口：文本已拼好时用，语义与惰性版一致 ----
void trace(const std::string& msg);
void debug(const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

// 关停并释放全局 logger（幂等）
void shutdown();

}  // namespace tinykv::log
