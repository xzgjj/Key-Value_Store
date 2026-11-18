// 文件用途：规模档崩溃恢复实测（M8 ⑬ PERF-06，SPEC-7 §7.2.3）——~1GB 日志 kill -9 后
//          重开计时，对照 S0 ≤30s / S1 ≤5s / S2 ≤2s。
// 归属：TinyKV-CPP（M8 优化，LEVEL=bench → nightly;serial;timeout:3600s）。
//
// 使用场景理解：PERF-06 此前从未实测（SPEC-7 §7.0 事实 #3）。本用例只做结构性断言
// （恢复成功、抽查不丢）；计时通过 printf 留痕，由 perf_report.py（⑫）采数判读——
// bench 不做硬失败断言（testing.md §3 口径，D7-4），慢机器不应该红，只应该如实出数。
//
// D7-5 降档旋钮：环境变量 TINYKV_SCALE_MB 覆盖默认 1GB 档（如 256）。降档运行时
// 输出明确标注档位，绝不冒充 1GB 档（如实标注红线）。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "storage_test_base.h"

#ifndef SCALE_WRITER_PATH
#error "需要定义 SCALE_WRITER_PATH（见 tests/bench/CMakeLists.txt）"
#endif

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 默认档：1GB = 1M 记录 × 1KB（写放大构造）；S0 对照线 30s（设计文档 §3.4 PERF-06）
constexpr long long kDefaultRecords = 1'000'000;
constexpr int kValueBytes = 1024;

TEST(ScaleRecovery, KillNineAtGigabyteLogReplayTiming) {
    // D7-5 降档旋钮：TINYKV_SCALE_MB 按体量反推记录数（1KB/条）
    long long records = kDefaultRecords;
    long long scale_mb = records * kValueBytes / (1024 * 1024);
    bool downgraded = false;
    if (const char* env = std::getenv("TINYKV_SCALE_MB"); env != nullptr) {
        scale_mb = std::atoll(env);
        ASSERT_GT(scale_mb, 0) << "TINYKV_SCALE_MB 必须是正整数";
        records = scale_mb * 1024 * 1024 / kValueBytes;
        ASSERT_GT(records, 0) << "体量太小算不出记录数";
        downgraded = true;
    }

    TempDir tmp("tinykv_scale_recovery");
    const auto dir = tmp.path() / "db";

    // 子进程构造体量并 _Exit(0)（等价 kill -9；内存表 8GB 上限保证全程不落 SST）
    const std::string cmd = std::string(SCALE_WRITER_PATH) + " \"" +
                            dir.generic_string() + "\" " + std::to_string(records) + " " +
                            std::to_string(kValueBytes);
    const int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "规模写入器执行失败 rc=" << rc;

    // 重开计时：WAL 重放路径（子进程未落 SST、未清理 WAL）
    const auto t0 = std::chrono::steady_clock::now();
    auto engine = tinykv::Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value()) << "恢复失败（WAL 重放不完整或格式损坏）";
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // 结构性断言：首/中/末抽查必须完整（规模档的数据正确性面，REL-02 不因体量打折）
    const char* probes[] = {"scale:0", "scale:499999", "scale:999999"};
    for (const char* key : probes) {
        auto got = (*engine)->get(tinykv::Cf::Default, bytes(key));
        ASSERT_TRUE(got.has_value()) << key;
        EXPECT_TRUE(got->value.has_value()) << key << " 丢失";
    }

    const double s0_line = 30.0, s1_line = 5.0, s2_line = 2.0;
    std::printf("\n[PERF-06] %lld MB 日志（%lld 条 × %d B）kill -9 恢复: %.1f s\n",
                scale_mb, records, kValueBytes, secs);
    std::printf("[PERF-06] S0≤%.0fs %s | S1≤%.0fs %s | S2≤%.0fs %s\n",
                s0_line, secs <= s0_line ? "✔" : "✘",
                s1_line, secs <= s1_line ? "✔" : "✘",
                s2_line, secs <= s2_line ? "✔" : "✘");
    if (downgraded) {
        std::printf("[PERF-06] 注意：非默认 1GB 档（D7-5 降档），对照 S0 线时须如实标注档位\n");
    }
}

}  // namespace
