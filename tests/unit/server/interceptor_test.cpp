// 文件用途：Interceptor 链用例（M6 挂账收口）：顺序/开关/指标/追踪可判定。
// 归属：TinyKV-CPP（M6 运行时）。

#include "server/interceptor.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace tinykv::server;

class Recorder final : public Interceptor {
public:
    Recorder(const std::string& tag, std::vector<std::string>& log) : tag_(tag), log_(log) {}
    void before(const RequestEvent& ev) override { log_.push_back("before:" + tag_ + ":" + ev.path); }
    void after(const RequestEvent& ev) override { log_.push_back("after:" + tag_ + ":" + ev.path); }

private:
    std::string tag_;
    std::vector<std::string>& log_;
};

TEST(InterceptorTest, BeforeInOrderAfterReversed) {
    std::vector<std::string> log;
    InterceptorChain chain;
    chain.add(std::make_unique<Recorder>("a", log));
    chain.add(std::make_unique<Recorder>("b", log));
    auto ev = chain.begin("POST", "/kv", "k");
    chain.finish(ev, true, "OK");
    EXPECT_EQ((std::vector<std::string>{"before:a:/kv", "before:b:/kv", "after:b:/kv",
                                        "after:a:/kv"}),
              log);
    EXPECT_TRUE(ev.ok);
    EXPECT_GE(ev.elapsed_us, 0U);
}

TEST(InterceptorTest, EmptyChainIsPassThrough) {
    InterceptorChain chain;
    auto ev = chain.begin("GET", "/status", "");
    chain.finish(ev, false, "ERR x");
    EXPECT_FALSE(ev.ok);
    EXPECT_EQ(ev.outcome, "ERR x");
    EXPECT_EQ(chain.size(), 0U);
}

TEST(InterceptorTest, MetricsCountsByPathAndOutcome) {
    InterceptorChain chain;
    auto* metrics = new MetricsInterceptor();
    chain.add(std::unique_ptr<Interceptor>(metrics));
    for (int i = 0; i < 3; ++i) {
        auto ev = chain.begin("POST", "/kv", "k");
        chain.finish(ev, i < 2, i < 2 ? "OK" : "ERR conflict");
    }
    auto ev2 = chain.begin("GET", "/status", "");
    chain.finish(ev2, true, "OK");

    const auto kv = metrics->get("/kv");
    EXPECT_EQ(kv.calls, 3U);
    EXPECT_EQ(kv.ok, 2U);
    EXPECT_EQ(kv.err, 1U);
    EXPECT_EQ(metrics->get("/status").calls, 1U);
    EXPECT_EQ(metrics->get("/missing").calls, 0U);
    EXPECT_EQ(metrics->total_calls(), 4U);
    EXPECT_NE(metrics->snapshot().find("\"path\":\"/kv\""), std::string::npos);
}

TEST(InterceptorTest, LogSinkReceivesFormattedLine) {
    std::vector<std::string> lines;
    InterceptorChain chain;
    chain.add(std::make_unique<LogInterceptor>(
        [&](const std::string& line) { lines.push_back(line); }));
    auto ev = chain.begin("POST", "/txn/commit", "s1");
    chain.finish(ev, true, "OK commit_ts=7");
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("POST /txn/commit OK OK commit_ts=7"), std::string::npos);
}

TEST(InterceptorTest, TraceKeepsMostRecentBounded) {
    TraceInterceptor trace(3);
    InterceptorChain chain;
    chain.add(std::unique_ptr<Interceptor>(new TraceInterceptor(3)));
    // 直接对独立 trace 实例驱动，验证环形缓冲
    for (int i = 0; i < 5; ++i) {
        RequestEvent ev;
        ev.method = "GET";
        ev.path = "/p" + std::to_string(i);
        ev.ok = true;
        trace.after(ev);
    }
    EXPECT_EQ(trace.count(), 3U);
    const auto snap = trace.snapshot();
    EXPECT_EQ(snap.find("/p0"), std::string::npos);
    EXPECT_NE(snap.find("/p4"), std::string::npos);
}

}  // namespace
