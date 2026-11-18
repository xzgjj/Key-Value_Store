// 文件用途：⑨a 分片接线基准——多 region 并发吞吐对比（M7-PERF-01 的证据物）。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `region_concurrency_bench`）。
//
// 使用场景理解：bench 只产出数字不做硬失败断言（docs/testing.md §3）。三种档位各测一轮：
//   shards=1  —— 改造前行为（一把大锁），基准线；
//   shards=16 —— 接线后默认档，跨 region 应并行（吞吐 ≥ 基准线，理想显著更高）；
//   每档各测"跨区并发"（线程分属两个 region）与"同区并发"（全部线程挤一个 region，
//   验证分片没有破坏同区串行带来的确定性）。
// 判读口径：跨区吞吐比值 sharded/legacy 记入输出；同区两档应接近（分片只改桶间并行度）。

#include "server/server_app.h"

#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

struct BenchResult {
    double cross_region_ops_per_sec{0.0};
    double same_region_ops_per_sec{0.0};
};

// 一次压测：threads 个线程各自独立 HTTP 连接，每线程 ops 条 PUT。
// spread=true 时线程分属两个 region（偶数线程 a*、奇数线程 z*）；false 时全部挤 region 1。
BenchResult run_bench(ServerApp& app, int threads, int ops, bool spread) {
    const auto port = app.status_port();
    std::atomic<int> ok{0};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([t, ops, port, spread, &ok] {
            const std::string prefix =
                spread && t % 2 != 0 ? "z" : "a";  // z* 落 region 2（split 在 "m"）
            for (int i = 0; i < ops; ++i) {
                const std::string key = prefix + std::to_string(t) + "_" + std::to_string(i);
                const std::string body = "put\t" + key + "\tv";
                if (tinykv::cli::http_post("127.0.0.1", port, "/kv", body).has_value()) {
                    ++ok;
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double total = static_cast<double>(threads * ops);
    EXPECT_EQ(ok.load(), threads * ops) << "压测有写入失败，数字不可信";
    return BenchResult{.cross_region_ops_per_sec = total / secs,
                       .same_region_ops_per_sec = total / secs};
}

// 8 线程 × 40 条，两档分片 × 跨区/同区两组，输出全部数字供 evidence 归档
TEST(RegionConcurrencyBench, ShardedGateThroughputVersusLegacy) {
    constexpr int kThreads = 8;
    constexpr int kOps = 40;
    struct Row {
        const char* label;
        std::size_t shards;
        BenchResult result;
    };
    std::vector<Row> rows;

    for (const std::size_t shards : {std::size_t{1}, std::size_t{16}}) {
        TempDir tmp{"tinykv_region_bench"};
        ServerOptions opt;
        opt.data_dir = tmp.path() / "db";
        opt.status_port = 0;
        opt.worker_interval = std::chrono::milliseconds(2);
        opt.lock_shards = shards;
        ServerApp app(std::move(opt));
        ASSERT_TRUE(app.start().has_value());
        // 先切成两个 region（split 在 "m"），a* 落 region 1、z* 落 region 2
        const auto split = tinykv::cli::http_post("127.0.0.1", app.status_port(),
                                                  "/region/split", "split\t1\tm");
        ASSERT_TRUE(split.has_value()) << *split;
        ASSERT_NE(split->find("OK"), std::string::npos) << *split;

        // 预热：worker 泵把首个 peer 推成 leader（有界轮询而非 sleep——ENG-04 禁 sleep 对齐）
        for (int round = 0; round < 50; ++round) {
            if (tinykv::cli::http_post("127.0.0.1", app.status_port(), "/kv", "put\twarm\tv")
                    .has_value()) {
                break;
            }
            std::this_thread::yield();
        }

        Row row;
        row.label = shards == 1 ? "shards=1(legacy)" : "shards=16(wired)";
        row.shards = shards;
        const BenchResult cross = run_bench(app, kThreads, kOps, /*spread=*/true);
        const BenchResult same = run_bench(app, kThreads, kOps, /*spread=*/false);
        row.result = BenchResult{.cross_region_ops_per_sec = cross.cross_region_ops_per_sec,
                                 .same_region_ops_per_sec = same.same_region_ops_per_sec};
        rows.push_back(row);
        ASSERT_TRUE(app.stop().has_value());
    }

    const double ratio = rows.back().result.cross_region_ops_per_sec /
                         rows.front().result.cross_region_ops_per_sec;
    std::printf("\n== M7-PERF-01 region 并发吞吐（8 线程 × 40 条） ==\n");
    for (const auto& row : rows) {
        std::printf("%-18s 跨区并发 %8.1f ops/s | 同区并发 %8.1f ops/s\n", row.label,
                    row.result.cross_region_ops_per_sec, row.result.same_region_ops_per_sec);
    }
    std::printf("跨区吞吐比值 sharded/legacy = %.2f（≥1 为达标，分片只应抬高不应压低）\n", ratio);
}

}  // namespace
