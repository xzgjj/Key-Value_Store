// 文件用途：Service 生命周期用例（M6 第①层）。
// 归属：TinyKV-CPP（M6 运行时）。
// 使用场景理解：daemon 化的地基是"启停顺序可判定"——顺序 start / 逆序 stop /
// start 失败回滚已启动者 / stop 幂等。全部用桩服务记录调用序列来断言。

#include "server/service.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using tinykv::server::LifecycleManager;
using tinykv::server::Service;

// 桩服务：记录调用序列；可配置 start 失败
class Stub final : public Service {
public:
    Stub(std::string name, std::vector<std::string>* log, bool fail_start = false)
        : name_(std::move(name)), log_(log), fail_start_(fail_start) {}

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] tinykv::result<void> start() override {
        if (fail_start_) {
            return tinykv::unexpected(tinykv::InvalidArgument{"启动失败: " + name_});
        }
        log_->push_back("start:" + name_);
        started_ = true;
        return {};
    }
    [[nodiscard]] tinykv::result<void> stop() override {
        log_->push_back("stop:" + name_);
        started_ = false;
        return {};
    }
    [[nodiscard]] bool started() const { return started_; }

private:
    std::string name_;
    std::vector<std::string>* log_;
    bool fail_start_;
    bool started_{false};
};

TEST(LifecycleTest, StartsInOrderStopsInReverse) {
    std::vector<std::string> log;
    LifecycleManager lc;
    lc.register_service(std::make_unique<Stub>("a", &log));
    lc.register_service(std::make_unique<Stub>("b", &log));
    lc.register_service(std::make_unique<Stub>("c", &log));

    EXPECT_TRUE(lc.start_all().has_value());
    EXPECT_TRUE(lc.running());
    EXPECT_EQ((std::vector<std::string>{"start:a", "start:b", "start:c"}), log);

    EXPECT_TRUE(lc.stop_all().has_value());
    EXPECT_FALSE(lc.running());
    EXPECT_EQ((std::vector<std::string>{"stop:c", "stop:b", "stop:a"}),
              std::vector<std::string>(log.end() - 3, log.end()));
}

TEST(LifecycleTest, StartFailureRollsBackStarted) {
    std::vector<std::string> log;
    LifecycleManager lc;
    lc.register_service(std::make_unique<Stub>("a", &log));
    lc.register_service(std::make_unique<Stub>("bad", &log, true));  // start 必败
    lc.register_service(std::make_unique<Stub>("c", &log));

    auto r = lc.start_all();
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(r.error()));
    // 回滚：a 被启动过又被停掉；bad 的 start 失败未进日志（从未 started，无 stop）
    EXPECT_EQ((std::vector<std::string>{"start:a", "stop:a"}), log);
    EXPECT_FALSE(lc.running());
}

TEST(LifecycleTest, StopIsIdempotent) {
    std::vector<std::string> log;
    LifecycleManager lc;
    lc.register_service(std::make_unique<Stub>("a", &log));
    ASSERT_TRUE(lc.start_all().has_value());
    ASSERT_TRUE(lc.stop_all().has_value());
    const auto after_first = log.size();
    ASSERT_TRUE(lc.stop_all().has_value());  // 二次 stop：幂等
    EXPECT_EQ(log.size(), after_first);
}

}  // namespace
