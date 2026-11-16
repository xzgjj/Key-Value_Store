// 文件用途：tinykv-cli 进程入口——参数转交 cli::run_cli，不在此做任何业务判断。
// 归属：TinyKV-CPP（M1 单机存储，UX-02）。

#include <iostream>
#include <string>
#include <vector>

#include "cli/cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return tinykv::cli::run_cli(args, std::cout, std::cerr);
}
