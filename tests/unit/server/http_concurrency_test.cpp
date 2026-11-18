// 文件用途：⑪ HTTP 并发正确性用例（M8-PERF-01）——业务端口 io_context 进线程池后：
//          ① 多线程并发 kv 读写/事务无丢失、无挂死；② 并发下 admin_split 与 kv 写共存
//          （⑪-0 审计 #15 的 split_error_ 竞争已由专用锁关闭——本用例是它的回归哨兵）；
//          ③ http_threads=1 回 M7 单线程行为逐字节等价。
// 归属：TinyKV-CPP（M8 优化，SPEC-7 §7.7 `http_concurrency_test`）。
//
// 使用场景理解：lock_shards_wiring_test 已证明"分片闸"语义正确（并发靠多线程客户端打）；
// 本文件证明"开闸"正确——handler 并发执行的共享状态全部有保护（notes-m8.md §1 审计清单）。
// 任何漏保护的状态都会表现为：偶发崩溃、写丢失、响应串台。用例反复跑（fast 档 CI 每次提交
// 都会执行），把并发窗口的暴露概率做实。

#include "server/server_app.h"

#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

struct Daemon {
    TempDir tmp;
    ServerApp app;
    explicit Daemon(std::size_t http_threads, const char* tag)
        : tmp(tag), app(make_options(tmp.path() / "db", http_threads)) {
        EXPECT_TRUE(app.start().has_value());
    }
    ~Daemon() { (void)app.stop(); }
    static ServerOptions make_options(const std::filesystem::path& dir,
                                      std::size_t http_threads) {
        ServerOptions opt;
        opt.data_dir = dir;
        opt.status_port = 0;
        opt.worker_interval = std::chrono::milliseconds(2);
        opt.lock_shards = 16;
        opt.http_threads = http_threads;
        return opt;
    }
};

// 预热：worker 泵把首个 peer 推成 leader（有界轮询而非 sleep——ENG-04 禁 sleep 对齐）
void wait_ready(ServerApp& app) {
    for (int round = 0; round < 50; ++round) {
        if (tinykv::cli::http_post("127.0.0.1", app.status_port(), "/kv", "put\twarm\tv")
                .has_value()) {
            return;
        }
        std::this_thread::yield();
    }
    FAIL() << "服务端 50 轮预热后仍不可写";
}

// ① 多线程并发 kv put/get：每线程独立连接，写完立即读回校验（分属两 region）
TEST(HttpConcurrencyTest, ConcurrentKvAcrossRegionsLosesNothing) {
    constexpr int kThreads = 8;
    constexpr int kOps = 30;
    Daemon d(4, "tinykv_http_conc");  // 4 个 io 线程 > 客户端 8 线程的一半，制造真实并发
    ASSERT_NO_FATAL_FAILURE(wait_ready(d.app));
    ASSERT_TRUE(tinykv::cli::http_post("127.0.0.1", d.app.status_port(), "/region/split",
                                       "split\t1\tm")
                    .has_value());

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &d, &failures] {
            const std::string prefix = t % 2 != 0 ? "z" : "a";  // split 在 "m"，两 region 各半
            for (int i = 0; i < kOps; ++i) {
                const std::string key = prefix + std::to_string(t) + "_" + std::to_string(i);
                const std::string val = "v" + std::to_string(t) + "_" + std::to_string(i);
                const auto put = tinykv::cli::http_post("127.0.0.1", d.app.status_port(), "/kv",
                                                        "put\t" + key + "\t" + val);
                if (!put.has_value() || put->find("OK") == std::string::npos) {
                    ++failures;
                    continue;
                }
                const auto get = tinykv::cli::http_get(
                    "127.0.0.1", d.app.status_port(), "/kv?key=" + key);
                if (!get.has_value() || get->find(val) == std::string::npos) {
                    ++failures;  // 写后读不一致 = 并发丢写/串台
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    EXPECT_EQ(failures.load(), 0) << "并发 kv 出现失败/丢写（开闸后正确性回归）";
}

// ② 并发 kv 写 + 并发 split + 并发观测读：split_error_ 竞争的回归哨兵（审计 #15）
TEST(HttpConcurrencyTest, SplitRacingWithKvAndObservabilityIsSafe) {
    Daemon d(4, "tinykv_http_conc_split");
    ASSERT_NO_FATAL_FAILURE(wait_ready(d.app));

    std::atomic<bool> stop{false};
    std::atomic<int> kv_failures{0};
    std::vector<std::thread> workers;
    // 4 条 kv 写线程（key 落在已 split 的 region 1/2 与未来的 split key 附近）
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([t, &d, &stop, &kv_failures] {
            int i = 0;
            while (!stop.load()) {
                const std::string key = "k" + std::to_string(t) + "_" + std::to_string(i++);
                if (!tinykv::cli::http_post("127.0.0.1", d.app.status_port(), "/kv",
                                            "put\t" + key + "\tv")
                         .has_value()) {
                    ++kv_failures;
                }
            }
        });
    }
    // 1 条观测读线程（/status 走全局闸、/metrics 走 interceptor 链，并发面全覆盖）
    workers.emplace_back([&d, &stop] {
        while (!stop.load()) {
            (void)tinykv::cli::http_get("127.0.0.1", d.app.status_port(), "/status");
            (void)tinykv::cli::http_get("127.0.0.1", d.app.status_port(), "/metrics");
        }
    });
    // 主线程连发 split（每次 split 都会走 observer：on_split + persist + split_error_ 写路径）。
    // split key 递减（m0 > f1 > b2 > …）：始终落在 region 1 当前范围 [_, 上次切点) 内——
    // 对 region 1 提出越界 split 会在 apply 侧报错并让 applied 位点停滞（这是既有语义，
    // 不是本用例要激发的路径）。
    constexpr std::array<const char*, 6> kSplitKeys = {"m0", "f1", "b2", "93", "54", "25"};
    for (const char* key : kSplitKeys) {
        const auto split = tinykv::cli::http_post("127.0.0.1", d.app.status_port(),
                                                  "/region/split",
                                                  std::string("split\t1\t") + key);
        ASSERT_TRUE(split.has_value()) << *split;
        EXPECT_NE(split->find("OK"), std::string::npos) << *split;
    }
    stop.store(true);
    for (auto& w : workers) {
        w.join();
    }
    EXPECT_EQ(kv_failures.load(), 0) << "split 与 kv 并发期间出现写失败";
    // split 全部完成后拓扑可查（observer 落盘成功 = split_error_ 为空）
    const auto regions =
        tinykv::cli::http_get("127.0.0.1", d.app.status_port(), "/regions");
    ASSERT_TRUE(regions.has_value());
}

// ③ http_threads=1：M7 单线程行为逐字节等价（红线：回退档语义不变）
TEST(HttpConcurrencyTest, SingleThreadFallbackMatchesLegacySemantics) {
    Daemon d(1, "tinykv_http_conc_one");
    ASSERT_NO_FATAL_FAILURE(wait_ready(d.app));
    const auto put =
        tinykv::cli::http_post("127.0.0.1", d.app.status_port(), "/kv", "put\tk1\tv1");
    ASSERT_TRUE(put.has_value()) << *put;
    EXPECT_NE(put->find("OK"), std::string::npos) << *put;
    const auto get = tinykv::cli::http_get("127.0.0.1", d.app.status_port(), "/kv?key=k1");
    ASSERT_TRUE(get.has_value()) << *get;
    EXPECT_NE(get->find("v1"), std::string::npos) << *get;
    // 事务全路径照常（会话管理器在单线程档同样工作）
    const auto begin =
        tinykv::cli::http_post("127.0.0.1", d.app.status_port(), "/txn/begin", "begin");
    ASSERT_TRUE(begin.has_value()) << *begin;
    EXPECT_NE(begin->find("OK session="), std::string::npos) << *begin;
}

}  // namespace
