// 文件用途：进程级崩溃恢复——子进程写完即 _Exit(0)，父进程重开必须一条不丢（REL-02）。
// 归属：TinyKV-CPP（M2 崩溃恢复，integration）。
// 使用场景理解：这是 REL-02 真正的证据物。此前的"关闭重建"走正常析构会 flush，
// 验证不到 WAL 重放；只有跳过析构（_Exit）才是"被强杀"。

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "storage_test_base.h"

#ifndef CRASH_WRITER_PATH
#error "需要定义 CRASH_WRITER_PATH（见 tests/integration/CMakeLists.txt）"
#endif

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

constexpr int kRecords = 200;

TEST(CrashRecovery, ChildKilledWithoutCleanupLosesNothing) {
    TempDir tmp("tinykv_crash");
    const auto dir = tmp.path() / "db";

    // CRASH_WRITER_PATH 已展开为带引号的字符串字面量，此处不要再补引号
    const std::string cmd =
        std::string(CRASH_WRITER_PATH) + " \"" + dir.generic_string() + "\" " +
        std::to_string(kRecords);
    const int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "崩溃写入器执行失败 rc=" << rc << " cmd=" << cmd;

    // 前置条件：子进程没有落 SST（否则就不是"只靠 WAL 恢复"了）
    std::size_t sst_files = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir / "sst", ec)) {
        if (e.is_regular_file()) {
            ++sst_files;
        }
    }
    EXPECT_EQ(sst_files, 0U) << "子进程竟然落了 SST，说明它走了正常关闭路径";

    // 重开：只可能从 WAL 重放
    auto engine = tinykv::Engine::open({.dir = dir});
    ASSERT_TRUE(engine.has_value());
    int missing = 0;
    for (int i = 0; i < kRecords; ++i) {
        const std::string key = "crash:" + std::to_string(i);
        auto got = (*engine)->get(tinykv::Cf::Default, bytes(key));
        if (!got.has_value() || !got->value.has_value() ||
            *got->value != "v" + std::to_string(i)) {
            ++missing;
        }
    }
    EXPECT_EQ(missing, 0) << "崩溃后丢失 " << missing << " 条（WAL 重放不完整）";
}

}  // namespace
