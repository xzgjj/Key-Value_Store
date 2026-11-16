// 文件用途：common/log（spdlog 封装）单元测试——装配可用性（SPEC-0 §0.7 ENG-03 验收行）。
// 归属：TinyKV-CPP（M0 基建，ENG-03）。
// 使用场景理解：日志统一落根 log/ 目录、轮转 10MB×5（CLAUDE.md §4-7）；测试用临时目录
// 验证 init/写盘/级别过滤/关停再启（服务重启场景），不验证 spdlog 本身的轮转实现。

#include "common/log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

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

TEST_F(CommonLog, UninitializedCallsAreSafe) {
    // 头文件承诺：未 init 前调用任何级别都安全丢弃（进程早期/测试态直调）。
    // 此前只有注释、没有用例——承诺没被测试锁定就等于没有。
    ASSERT_FALSE(tinykv::log::initialized());
    EXPECT_FALSE(tinykv::log::enabled(tinykv::log::Level::critical));  // 最低门槛也不放行

    tinykv::log::info("no-logger");
    tinykv::log::error("no-logger {} {}", 1, 2);  // 惰性入口同样不得崩
    SUCCEED();
}

TEST_F(CommonLog, InitToUnwritablePathReturnsIoFailed) {
    // 结构化错误而不是抛异常：路径被普通文件占住 → create_directories 失败 → IoFailed
    LogSandbox sandbox;
    const auto blocker = sandbox.dir() / "blocker";
    {
        std::ofstream out(blocker);
        out << "x";
    }
    auto r = tinykv::log::init(blocker / "sub", "tinykv", false);
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::IoFailed>(r.error()));
    EXPECT_FALSE(tinykv::log::initialized());
}

TEST_F(CommonLog, VariadicEntryFormatsOnWrite) {
    // 惰性入口必须真的格式化（否则等于把参数吞掉）
    LogSandbox sandbox;
    ASSERT_TRUE(tinykv::log::init(sandbox.dir(), "tinykv").has_value());
    tinykv::log::info("region={} index={}", 7, 42);

    const std::string content = read_all(sandbox.dir() / "tinykv.log");
    EXPECT_NE(content.find("region=7 index=42"), std::string::npos);
}

TEST_F(CommonLog, FilteredLevelIsNotWritten) {
    // 惰性入口的边界：级别关闭时格式化与落盘都不发生（但实参已在调用点求值，
    // 这是 C++ 的语义——函数实参永远先于函数体求值，模板无法改变）。
    LogSandbox sandbox;
    ASSERT_TRUE(tinykv::log::init(sandbox.dir(), "tinykv").has_value());
    tinykv::log::set_level(tinykv::log::Level::warn);

    tinykv::log::info("filtered={}", 1);
    tinykv::log::warn("passed={}", 2);

    const std::string content = read_all(sandbox.dir() / "tinykv.log");
    EXPECT_EQ(content.find("filtered="), std::string::npos);
    EXPECT_NE(content.find("passed=2"), std::string::npos);
}

TEST_F(CommonLog, EnabledGuardSkipsExpensiveArguments) {
    // 昂贵实参的正确写法：进函数前先问 enabled()。这是惰性入口唯一能给的保证，
    // 头文件已写明；这里把"没有别的办法"钉成用例，防止后来者误以为模板能省实参。
    LogSandbox sandbox;
    ASSERT_TRUE(tinykv::log::init(sandbox.dir(), "tinykv").has_value());
    tinykv::log::set_level(tinykv::log::Level::warn);

    int evaluated = 0;
    auto expensive = [&evaluated] {
        ++evaluated;
        return 42;
    };

    if (tinykv::log::enabled(tinykv::log::Level::debug)) {
        tinykv::log::debug("debug={}", expensive());
    }
    EXPECT_EQ(evaluated, 0);  // 被 enabled() 挡住，实参根本没求值

    if (tinykv::log::enabled(tinykv::log::Level::warn)) {
        tinykv::log::warn("warn={}", expensive());
    }
    EXPECT_EQ(evaluated, 1);
    EXPECT_NE(read_all(sandbox.dir() / "tinykv.log").find("warn=42"), std::string::npos);
}

}  // namespace
