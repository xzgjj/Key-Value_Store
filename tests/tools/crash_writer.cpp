// 文件用途：崩溃写入器——写完即 _Exit(0)，不执行任何析构（等价于被 kill -9）。
// 归属：TinyKV-CPP（M2 REL-02 验证工具，不是测试本体）。
//
// 为什么需要独立进程：进程内"关闭再重开"走的是正常析构路径（会 flush 内存表），
// 根本验证不到"只靠 WAL 恢复"。只有跳过析构/退出清理，"写到一半被强杀"才成立。
// std::_Exit 不做静态析构、不 flush stdio，正是我们要的语义。

#include <cstdio>
#include <cstdlib>
#include <string>

#include "storage/engine.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "用法: crash_writer <数据目录> <记录数>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const int count = std::atoi(argv[2]);

    auto engine = tinykv::Engine::open({.dir = dir, .fsync_on_commit = true});
    if (!engine.has_value()) {
        std::fprintf(stderr, "打开引擎失败\n");
        return 3;
    }
    for (int i = 0; i < count; ++i) {
        tinykv::ModifyVec batch{tinykv::Modify{tinykv::Put{
            .cf = tinykv::Cf::Default,
            .key = "crash:" + std::to_string(i),
            .value = "v" + std::to_string(i)}}};
        if (!(*engine)->write(batch).has_value()) {
            std::fprintf(stderr, "写入失败 i=%d\n", i);
            return 4;
        }
    }
    std::printf("WROTE %d\n", count);
    std::fflush(stdout);

    // 关键：跳过所有析构与清理，内存表不会落 SST，WAL 也不会被 clear
    std::_Exit(0);
}
