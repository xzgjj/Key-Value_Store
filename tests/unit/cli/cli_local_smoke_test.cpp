// 文件用途：本地 CLI 冒烟——五个子命令 + 配置错误处理（UX-02 本地模式、UX-05）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：CLI 是外部唯一入口，它的失败方式（找不到 key、配置写错）必须稳定可预期；
// 因此断言退出码而不只是输出文本，退出码是脚本化调用唯一能依赖的契约。

#include "cli/cli.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace {

namespace fs = std::filesystem;
using tinykv::testlib::TempDir;

// 一次调用的结果：退出码 + 两个输出流，便于逐项断言
struct Invocation {
    int code{0};
    std::string out;
    std::string err;
};

Invocation invoke(const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = tinykv::cli::run_cli(args, out, err);
    return Invocation{.code = code, .out = out.str(), .err = err.str()};
}

std::string write_config(const fs::path& dir, const fs::path& data_dir) {
    const fs::path cfg = dir / "demo.yaml";
    std::ofstream f(cfg, std::ios::trunc);
    f << "engine:\n"
      << "  dir: " << data_dir.generic_string() << "\n"
      << "  fsync_on_commit: true\n"
      << "  memtable_limit_mb: 64\n"
      << "  wal_segment_mb: 128\n"
      << "log:\n"
      << "  level: info\n"
      << "  dir: " << (dir / "log").generic_string() << "\n";
    return cfg.generic_string();
}

TEST(CliLocalSmoke, PutGetDeleteScanRoundtrip) {
    TempDir tmp("tinykv_cli");
    const std::string cfg = write_config(tmp.path(), tmp.path() / "db");

    EXPECT_EQ(invoke({"--config", cfg, "put", "k1", "v1"}).code, 0);
    const auto got = invoke({"--config", cfg, "get", "k1"});
    EXPECT_EQ(got.code, 0);
    EXPECT_EQ(got.out, "v1\n");

    const auto scanned = invoke({"--config", cfg, "scan"});
    EXPECT_EQ(scanned.code, 0);
    EXPECT_NE(scanned.out.find("k1"), std::string::npos);

    EXPECT_EQ(invoke({"--config", cfg, "del", "k1"}).code, 0);
    EXPECT_EQ(invoke({"--config", cfg, "get", "k1"}).code, 3);   // NOT_FOUND
}

TEST(CliLocalSmoke, HelpAndStatusSucceed) {
    TempDir tmp("tinykv_cli_help");
    const std::string cfg = write_config(tmp.path(), tmp.path() / "db");

    const auto help = invoke({"--help"});
    EXPECT_EQ(help.code, 0);
    EXPECT_NE(help.out.find("tinykv-cli"), std::string::npos);

    const auto status = invoke({"--config", cfg, "status"});
    EXPECT_EQ(status.code, 0);
    EXPECT_NE(status.out.find("fsync"), std::string::npos);
}

TEST(CliLocalSmoke, UsageErrorsReturnOne) {
    TempDir tmp("tinykv_cli_usage");
    const std::string cfg = write_config(tmp.path(), tmp.path() / "db");

    EXPECT_EQ(invoke({"--config", cfg, "put", "only-key"}).code, 1);   // 缺 value
    EXPECT_EQ(invoke({"--config", cfg, "nosuchcmd"}).code, 1);
    EXPECT_EQ(invoke({"--config", cfg, "get", "k", "--cf", "bogus"}).code, 1);
    // code-review：--limit 曾用 std::stoul，非数字会抛异常穿出 main（terminate）
    EXPECT_EQ(invoke({"--config", cfg, "scan", "--limit", "abc"}).code, 1);
    EXPECT_EQ(invoke({"--config", cfg, "scan", "--limit", "0"}).code, 1);
    EXPECT_EQ(invoke({"--config", cfg, "scan", "--limit", "-1"}).code, 1);
}

TEST(CliLocalSmoke, ConfigErrorsAreReported) {
    TempDir tmp("tinykv_cli_cfg");
    const fs::path bad = tmp.path() / "bad.yaml";
    {
        std::ofstream f(bad, std::ios::trunc);
        f << "engine:\n  unknown_key: 1\n";
    }
    // 未知键必须报错而不是静默忽略——配置静默失效比启动失败更难查
    EXPECT_EQ(invoke({"--config", bad.generic_string(), "status"}).code, 2);

    EXPECT_EQ(invoke({"--config", (tmp.path() / "absent.yaml").generic_string(), "status"}).code, 2);
}

TEST(CliLocalSmoke, DataPersistsAcrossInvocations) {
    TempDir tmp("tinykv_cli_persist");
    const std::string cfg = write_config(tmp.path(), tmp.path() / "db");
    EXPECT_EQ(invoke({"--config", cfg, "put", "persist", "yes"}).code, 0);

    // 每次调用都是新进程语义（新 Engine），数据必须从落盘路径读回来
    const auto again = invoke({"--config", cfg, "get", "persist"});
    EXPECT_EQ(again.code, 0);
    EXPECT_EQ(again.out, "yes\n");
}

}  // namespace
