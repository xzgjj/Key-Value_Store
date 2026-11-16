// 文件用途：本地 CLI 的命令分发——子命令解析与输出（UX-02 本地模式）。
// 归属：TinyKV-CPP（M1 单机存储，UX-02/UX-05）。
//
// 与 main.cpp 分离的目的：让用例可以直调 run_cli 而不必 spawn 进程（跨平台、快、可断言输出）。
// 退出码契约（docs/cli.md 有完整表）：0 成功 / 1 用法错误 / 2 运行错误 / 3 key 不存在。

#pragma once

#include <ostream>
#include <span>
#include <string>

namespace tinykv::cli {

// 执行一条 CLI 调用；out/err 注入以便测试捕获，缺省由 main 传 std::cout/std::cerr
int run_cli(std::span<const std::string> args, std::ostream& out, std::ostream& err);

}  // namespace tinykv::cli
