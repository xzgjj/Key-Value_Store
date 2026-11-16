// 文件用途：common/log（spdlog 封装）单元测试——装配可用性（SPEC-0 §0.7 ENG-03 验收行）。
// 归属：TinyKV-CPP（M0 基建，ENG-03）。
// 使用场景理解：日志统一落根 log/ 目录、轮转 10MB×5（CLAUDE.md §4-7）；测试用临时目录
// 验证 init/写盘/级别过滤/关停再启（服务重启场景），不验证 spdlog 本身的轮转实现。

#include "common/log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// 测试沙箱：临时目录 RAII（构造创建、析构清理，互不污染真实 log/）
class LogSandbox {
public:
    LogSandbox() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("tinykv_log_test_" + std::to_string(tick));
        fs::create_directories(dir_);
    }
    ~LogSandbox() {
        std::error_code ec;
        fs::remove_all(dir_, ec);  // 清理失败不影响测试结果判定
    }
    LogSandbox(const LogSandbox&) = delete;
    LogSandbox& operator=(const LogSandbox&) = delete;
    const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

// 全局 logger 是进程级资源：每个用例结束统一 shutdown（释放文件句柄与注册名），
// Windows 上文件锁未释放会导致后续用例 init 失败——测试卫生即正确性。
class CommonLog : public ::testing::Test {
protected:
    void TearDown() override { tinykv::log::shutdown(); }
};

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST_F(CommonLog, InitCreatesFileAndWrites) {
    LogSandbox sandbox;
    auto r = tinykv::log::init(sandbox.dir(), "tinykv");
    ASSERT_TRUE(r.has_value()) << "init 失败：环境问题不应让单元测试假绿";
    EXPECT_TRUE(tinykv::log::initialized());

    tinykv::log::info("hello-m0");  // flush_on(info)：写盘即时可见
    const std::string content = read_all(sandbox.dir() / "tinykv.log");
    EXPECT_NE(content.find("hello-m0"), std::string::npos);
}

TEST_F(CommonLog, LevelFilter) {
    LogSandbox sandbox;
    ASSERT_TRUE(tinykv::log::init(sandbox.dir(), "tinykv").has_value());

    tinykv::log::set_level(tinykv::log::Level::warn);
    tinykv::log::info("below-threshold");  // 应被过滤
    tinykv::log::warn("above-threshold");  // 应落盘

    const std::string content = read_all(sandbox.dir() / "tinykv.log");
    EXPECT_EQ(content.find("below-threshold"), std::string::npos);
    EXPECT_NE(content.find("above-threshold"), std::string::npos);
}

TEST_F(CommonLog, ShutdownThenReinit) {
    // 服务重启场景：shutdown 后允许再次 init（新日志段）
    LogSandbox sandbox;
    ASSERT_TRUE(tinykv::log::init(sandbox.dir(), "tinykv").has_value());
    tinykv::log::shutdown();
    EXPECT_FALSE(tinykv::log::initialized());

    auto r = tinykv::log::init(sandbox.dir(), "tinykv");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(tinykv::log::initialized());
}

}  // namespace
