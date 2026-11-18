// 文件用途：BUG-0023（code-review r2 N-5）回归——keep-alive 连接的空闲读超时 +
// `Connection:close`（无空格）判定。
// 归属：TinyKV-CPP（M8 后还债，LEVEL=unit → fast）。
//
// 使用场景理解：keep-alive 连接一旦建立，旧实现无限期等下一个请求——客户端连上不发
// 请求即可占住 socket 与 4KB 读缓冲（slowloris 面）；且旧 wants_close 要求冒号后
// 必须有空格（" close"），HTTP 允许的 `Connection:close` 被误判成 keep-alive，
// 客户端关不掉连接。修复后：空闲 30s（可注入更小值）即断开；判定裁掉前导空白。

#include "server/status_server.h"

#include <asio.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "bounded_io.h"
#include "temp_dir.h"

namespace {

using tinykv::server::StatusServer;
using tinykv::testlib::TempDir;

asio::ip::tcp::socket connect_to(asio::io_context& ctx, std::uint16_t port) {
    asio::ip::tcp::socket sock(ctx);
    sock.connect({asio::ip::make_address("127.0.0.1"), port});
    return sock;
}

// 读一条完整响应（头部 + Content-Length 声明的正文）。
// BUG-0024 配套（r2 N-5 测试面）：读必须有界——非阻塞 + 轮询 + 3s 上限，
// 服务端不应答时测试以失败收场而不是挂死。
std::string read_one_response(asio::ip::tcp::socket& sock) {
    std::string resp;
    char buf[4096];
    asio::error_code ec;
    sock.non_blocking(true, ec);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto header_end = resp.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            std::size_t need = 0;
            const auto cl = resp.find("Content-Length: ");
            if (cl != std::string::npos) {
                const auto eol = resp.find("\r\n", cl);
                need = std::stoull(resp.substr(cl + 16, eol - cl - 16));
            }
            if (resp.size() >= header_end + 4 + need) {
                break;
            }
        }
        if (std::chrono::steady_clock::now() > deadline) {
            EXPECT_FALSE(true) << "3s 内未收到完整响应（服务端不应答也不关连接？）: "
                               << resp.substr(0, 120);
            return resp;
        }
        if (!tinykv::testlib::wait_readable(sock, std::chrono::milliseconds(500))) {
            continue;  // 分片无数据：由 deadline 收口（ENG-04 无 sleep_for）
        }
        const std::size_t n = sock.read_some(asio::buffer(buf), ec);
        if (ec && ec != asio::error::would_block) {
            break;  // EOF：响应已完整或连接被关
        }
        resp.append(buf, n);
    }
    return resp;
}

// N-5 判定面：`Connection:close`（冒号后无空格）必须被识别——响应关连接
TEST(HttpKeepAlive, ConnectionCloseWithoutSpaceIsHonored) {
    TempDir tmp("tinykv_http_close");
    auto started = StatusServer::start("127.0.0.1", 0, [] { return std::string("{\"ok\":1}"); });
    ASSERT_TRUE(started.has_value());
    const auto server = std::move(*started);

    asio::io_context ctx;
    auto sock = connect_to(ctx, server->port());
    const std::string req = "GET /status HTTP/1.1\r\nHost: x\r\nConnection:close\r\n\r\n";
    asio::write(sock, asio::buffer(req));
    const std::string resp = read_one_response(sock);
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("Connection: close"), std::string::npos)
        << "无空格的 Connection:close 必须被识别（回归：旧判定要求冒号后有空格）" << resp;
}

// N-5 超时面：连上不发请求 → 空闲超时后服务端必须断开（注入 100ms，客户端限时等 EOF）
TEST(HttpKeepAlive, IdleConnectionIsClosedAfterTimeout) {
    TempDir tmp("tinykv_http_idle");
    auto started = StatusServer::start("127.0.0.1", 0, [] { return std::string("{}"); }, {}, {},
                                       {}, {}, 1, {},
                                       std::chrono::milliseconds{100});
    ASSERT_TRUE(started.has_value());
    const auto server = std::move(*started);

    asio::io_context ctx;
    auto sock = connect_to(ctx, server->port());
    sock.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool closed = false;
    char buf[64];
    while (std::chrono::steady_clock::now() < deadline) {
        asio::error_code ec;
        (void)sock.read_some(asio::buffer(buf), ec);
        if (ec && ec != asio::error::would_block) {
            closed = true;  // eof / connection_reset：服务端空闲超时断连
            break;
        }
        (void)tinykv::testlib::wait_readable(sock, std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(closed) << "空闲连接 100ms 后必须被服务端关闭（回归：旧实现无限期等待）";
}

// 防过修守卫：活跃 keep-alive 连接在超时注入值内连续请求必须照常工作（超时只管空闲）
TEST(HttpKeepAlive, ActiveKeepAliveConnectionSurvivesTimeout) {
    TempDir tmp("tinykv_http_active");
    auto started = StatusServer::start("127.0.0.1", 0, [] { return std::string("{\"n\":1}"); },
                                       {}, {}, {}, {}, 1, {},
                                       std::chrono::milliseconds{200});
    ASSERT_TRUE(started.has_value());
    const auto server = std::move(*started);

    asio::io_context ctx;
    auto sock = connect_to(ctx, server->port());
    const std::string req = "GET /status HTTP/1.1\r\nHost: x\r\n\r\n";  // HTTP/1.1 默认 keep-alive
    for (int i = 0; i < 3; ++i) {
        asio::write(sock, asio::buffer(req));
        const std::string resp = read_one_response(sock);
        ASSERT_NE(resp.find("Connection: keep-alive"), std::string::npos)
            << "第 " << i << " 次：活跃连接不得被空闲超时误杀";
    }
}

}  // namespace
