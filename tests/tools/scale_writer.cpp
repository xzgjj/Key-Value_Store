// 文件用途：规模档崩溃写入器——按"记录数 × value 字节数"构造指定体量的日志后 _Exit(0)。
// 归属：TinyKV-CPP（M8 ⑬ PERF-06 采数工具，SPEC-7 §7.2.3；不是测试本体）。
//
// 与 crash_writer 的差别：crash_writer 验证"一条不丢"（REL-02 正确性面），本工具验证
// "重放多久"（PERF-06 规模面）——①批量化写入（逐条 fsync 在 1GB 体量下不可行）；
// ②内存表上限抬高到 8GB：写入全程不落 SST，恢复时走纯 WAL 重放路径，规模真相不被
// flush 掩盖；③value 定长（默认 1KB）实现"写放大构造 ~1GB raft log"。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "storage/engine.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "用法: scale_writer <数据目录> <记录数> <value字节数>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const long long count = std::atoll(argv[2]);
    const int value_bytes = std::atoi(argv[3]);
    if (count <= 0 || value_bytes <= 0) {
        std::fprintf(stderr, "参数非法：count=%lld value_bytes=%d\n", count, value_bytes);
        return 2;
    }

    tinykv::EngineOptions opt;
    opt.dir = dir;
    opt.fsync_on_commit = true;
    opt.memtable_limit_bytes = 8ULL << 30U;  // 8GB：写入全程不落 SST（纯 WAL 恢复路径）
    auto engine = tinykv::Engine::open(opt);
    if (!engine.has_value()) {
        std::fprintf(stderr, "打开引擎失败\n");
        return 3;
    }

    // 定长 value：'v' 填充（内容无关紧要，体量才是考察点）；批量 512 条/次对齐 Envelope 上限
    const std::string value(static_cast<std::size_t>(value_bytes), 'v');
    constexpr long long kBatch = 512;
    for (long long base = 0; base < count; base += kBatch) {
        tinykv::ModifyVec batch;
        batch.reserve(kBatch);
        for (long long i = base; i < base + kBatch && i < count; ++i) {
            batch.push_back(tinykv::Modify{tinykv::Put{
                .cf = tinykv::Cf::Default,
                .key = "scale:" + std::to_string(i),
                .value = value}});
        }
        if (!(*engine)->write(batch).has_value()) {
            std::fprintf(stderr, "写入失败 base=%lld\n", base);
            return 4;
        }
    }
    std::printf("WROTE %lld records × %d B\n", count, value_bytes);
    std::fflush(stdout);

    // 关键：跳过所有析构与清理，内存表不落 SST，WAL 保留全部体量（等价 kill -9）
    std::_Exit(0);
}
