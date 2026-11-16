// 文件用途：spdlog 封装——全局日志统一入口（轮转 10MB×5，落根 log/ 目录，CLAUDE.md §4-7）。
// 归属：TinyKV-CPP（M0 基建，ENG-03）。
//
// 约定：
//   * init 一次装配；未 init 前调用 info/warn/error 安全地丢弃（进程早期/测试态可直调）；
//   * flush_on(info)：审计日志即时落盘（写放大调优属 M1 WAL group commit 课题）；
//   * 同步 logger，无后台线程（ENG-04：禁 detach 线程）；
//   * 调用方用 std::format 预格式化，本层不做 fmt 依赖透出。

#pragma once

#include <filesystem>
#include <string>

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

// 四档便捷输出：未初始化时安全丢弃
void trace(const std::string& msg);
void debug(const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

// 关停并释放全局 logger（幂等）
void shutdown();

}  // namespace tinykv::log
