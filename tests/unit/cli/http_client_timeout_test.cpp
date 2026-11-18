// 文件用途：BUG-0024（code-review r2 N-1 客户端面）回归——HTTP 客户端必须有界等待。
// 归属：TinyKV-CPP（M8 后还债批，LEVEL=unit → fast）。
//
// 使用场景理解：旧实现的头注释声称「超时靠 socket 的 native 选项设置」，实际零超时——
// 服务端只要「收了请求但不应答也不关连接」，CLI 与全部走 http_get/http_post 的测试就
// 永久挂死（M8 后还债批实测：CI 全程挂 25 分钟）。修复后 connect 非阻塞 + select（5s）、
// send/recv 带 SO_SNDTIMEO/SO_RCVTIMEO（10s，本用例注入 300ms）——对端沉默必须以
// IoFailed「响应超时」快速收场，而不是挂死进程。

#include "cli/http_client.h"

#include <asio.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <variant>

#include "server/server_app.h"
#include "temp_dir.h"

namespace {

using tinykv::cli::request;
using tinykv::testlib::TempDir;

// 一个只 accept、从不响应也不关连接的裸监听器（制造"服务端沉默"现场）
struct SilentListener {
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc{ctx};
    std::shared_ptr<asio::ip::tcp::socket> held;  // 保活被接受连接（lambda 亡即 RST）
    std::thread runner;
    std::uint16_t port{0};

    SilentListener() {
        asio::error_code ec;
        acc.open(asio::ip::tcp::v4(), ec);
        EXPECT_FALSE(ec);
        acc.set_option(asio::socket_base::reuse_address(true), ec);
        acc.bind({asio::ip::make_address("127.0.0.1"), 0}, ec);
        EXPECT_FALSE(ec);
        acc.listen(asio::socket_base::max_listen_connections, ec);
        EXPECT_FALSE(ec);
        port = acc.local_endpoint(ec).port();
        // accept 一个连接后什么都不做：既不应答也不 close——正是挂死现场
        held = std::make_shared<asio::ip::tcp::socket>(ctx);
        acc.async_accept(*held, [this](const asio::error_code&) {});
        runner = std::thread([this] { ctx.run(); });
    }
    ~SilentListener() {
        asio::error_code ig;
        acc.close(ig);
        ctx.stop();
        if (runner.joinable()) {
            runner.join();
        }
        held.reset();
    }
};

TEST(HttpClientTimeout, SilentServerTimesOutInsteadOfHangingForever) {
    TempDir tmp("tinykv_http_client_timeout");
    SilentListener listener;
    const auto started = std::chrono::steady_clock::now();
    auto r = request("GET", "127.0.0.1", listener.port, "/status", {}, {}, 300);
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(r.has_value()) << "沉默服务端竟返回成功响应";
    const auto* io = std::get_if<tinykv::IoFailed>(&r.error());
    ASSERT_NE(io, nullptr) << "必须结构化超时报错";
    EXPECT_NE(io->reason.find("超时"), std::string::npos)
        << "失败原因应指向超时（而非连接失败）: " << io->reason;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(waited).count(), 5)
        << "超时兜底必须生效：不能等满旧实现的无限阻塞";
}

TEST(HttpClientTimeout, ConnectionRefusedFailsFastWithIoFailed) {
    // 没有任何监听者的端口：connect 必须快速失败（不是挂死、不是超时等待）
    auto r = request("GET", "127.0.0.1", 1 /* 保留端口，无监听 */, "/status", {}, 500, 500);
    ASSERT_FALSE(r.has_value());
    const auto* io = std::get_if<tinykv::IoFailed>(&r.error());
    ASSERT_NE(io, nullptr);
    EXPECT_NE(io->reason.find("连接"), std::string::npos) << io->reason;
}

TEST(HttpClientTimeout, HealthyRoundtripStillWorks) {
    // 防过修守卫：有界等待不能伤正常路径——真 ServerApp 往返照常成功
    TempDir tmp("tinykv_http_client_ok");
    tinykv::server::ServerOptions opt{.data_dir = tmp.path() / "db",
                                      .status_port = 0,
                                      .worker_interval = std::chrono::milliseconds(2),
                                      .log_sink = {},
                                      .auth_token = {}};
    tinykv::server::ServerApp app{opt};
    ASSERT_TRUE(app.start().has_value());
    auto r = request("GET", "127.0.0.1", app.status_port(), "/status", {}, 1000, 1000);
    ASSERT_TRUE(r.has_value()) << "正常服务端必须照常应答";
    EXPECT_NE(r->find("worker_cycles"), std::string::npos) << *r;
    (void)app.stop();
}

}  // namespace
