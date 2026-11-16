// 文件用途：写吞吐基准——batch=1 与 batch=64 两档 QPS / P99（PERF-01/02/05 采数）。
// 归属：TinyKV-CPP（M1 单机存储，LEVEL=bench → 标签 bench+nightly，不做硬失败）。
// 使用场景理解：基准的职责是给出可复跑的数字，而不是"跑一次就写进文档"。断言只保留
// "确实写出了动量"这类底线，阈值比较留给人工读数——不同机器绝对值不可比。

#include "storage/engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "storage_test_base.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

struct Sample {
    double qps{0.0};
    double p99_ms{0.0};
};

// 一次批量写：n 条 records_per_batch 大小的批次
Sample run_batch(Engine& engine, int records, int batch_size) {
    std::vector<double> latency_ms;
    latency_ms.reserve(static_cast<std::size_t>(records));
    std::string value(static_cast<std::size_t>(64), 'v');
    const auto start = Clock::now();
    for (int i = 0; i < records; i += batch_size) {
        ModifyVec batch;
        batch.reserve(static_cast<std::size_t>(batch_size));
        for (int j = 0; j < batch_size; ++j) {
            batch.push_back(Modify{Put{.cf = Cf::Default,
                                       .key = "bench:" + std::to_string(i + j),
                                       .value = value}});
        }
        const auto t0 = Clock::now();
        EXPECT_TRUE(engine.write(batch).has_value());
        const auto t1 = Clock::now();
        latency_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const auto total = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(latency_ms.begin(), latency_ms.end());
    const auto idx = static_cast<std::size_t>(static_cast<double>(latency_ms.size()) * 0.99);
    return Sample{
        .qps = static_cast<double>(records) / total,
        .p99_ms = latency_ms[std::min(idx, latency_ms.size() - 1)],
    };
}

TEST(WriteBench, BatchOneVersusBatch64) {
    TempDir tmp("tinykv_bench");
    auto engine = Engine::open(EngineOptions{.dir = tmp.path() / "db",
                                             .fsync_on_commit = true,
                                             .memtable_limit_bytes = 1ULL << 30});
    ASSERT_TRUE(engine.has_value());

    const auto one = run_batch(**engine, 2000, 1);
    const auto bulk = run_batch(**engine, 2000, 64);
    const double speedup = bulk.qps / one.qps;

    std::printf("[bench] batch=1  : QPS=%.0f P99=%.3fms\n", one.qps, one.p99_ms);
    std::printf("[bench] batch=64 : QPS=%.0f P99=%.3fms 加速比=%.2fx\n", bulk.qps, bulk.p99_ms,
                speedup);
    std::printf("[note ] 本机数字仅供趋势参考；S0 口径（≥2k QPS / ≥8x / P99≤10ms）"
                "以参考环境实测为准\n");

    EXPECT_GT(one.qps, 0.0);
    EXPECT_GT(bulk.qps, 0.0);
}

}  // namespace
