// 文件用途：⑭ TLS 传输往返用例（M8-SEC-01，SPEC-7 §7.7 `tls_transport_test`）——
//          ① 配置证书后 TLS 握手 + HTTP 往返成功；② 明文连接打 TLS 端口握手失败断连
//          （Corrupted{where="tls-handshake"} 语义：不回应、不记业务错误）。
// 归属：TinyKV-CPP（M8 优化，LEVEL=unit → fast）。
//
// 证书来源：预置测试证书（tests/testdata/tls_test_{cert,key}.pem，自签 CN=localhost，
// 10 年期——SPEC-7 §7.2.4 明确"预置测试证书"为合法路径，不引入签发工具链）。
// 客户端用 asio::ssl::stream + verify_none（自签，无信任链）。

#include "server/status_server.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "raft_test_base.h"

namespace {

using tinykv::server::StatusServer;
using tinykv::server::TlsOptions;
using tinykv::testlib::TempDir;

// 与仓库根对齐的测试证书路径（测试工作目录随构建树，向上找 tests/testdata）
std::filesystem::path find_test_cert(const char* name) {
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    while (!dir.empty()) {
        const auto candidate = dir / "tests" / "testdata" / name;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
        dir = dir.parent_path();
    }
    return {};
}

// TLS 客户端一次 GET /status 往返：握手 → 发请求 → 收响应正文
std::string tls_get(std::uint16_t port) {
    asio::io_context ctx;
    asio::ssl::context client_ctx{asio::ssl::context::tls_client};
    client_ctx.set_verify_mode(asio::ssl::verify_none);  // 自签测试证书：无信任链
    asio::ssl::stream<asio::ip::tcp::socket> stream(ctx, client_ctx);
    stream.lowest_layer().connect(
        {asio::ip::make_address("127.0.0.1"), port});
    stream.handshake(asio::ssl::stream_base::client);
    const std::string req = "GET /status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(req));
    std::string resp;
    char buf[4096];
    asio::error_code ec;
    for (;;) {
        const std::size_t n = stream.read_some(asio::buffer(buf), ec);
        if (ec) {
            break;
        }
        resp.append(buf, n);
    }
    return resp;
}

TEST(TlsTransport, TlsRoundTripServesStatus) {
    const auto cert = find_test_cert("tls_test_cert.pem");
    const auto key = find_test_cert("tls_test_key.pem");
    ASSERT_FALSE(cert.empty()) << "预置测试证书缺失（tests/testdata/）";

    TempDir tmp("tinykv_tls_transport");
    auto started = StatusServer::start(
        "127.0.0.1", 0, [] { return std::string("{\"ok\":true}"); }, {}, {}, {}, {},
        1, TlsOptions{cert.generic_string(), key.generic_string()});
    ASSERT_TRUE(started.has_value()) << "TLS 状态端点启动失败";
    const auto server = std::move(*started);

    const std::string resp = tls_get(server->port());
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("\"ok\":true"), std::string::npos) << resp;
}

TEST(TlsTransport, PlaintextClientToTlsPortIsRejectedSilently) {
    // 明文请求打 TLS 端口：握手失败 → 服务端断连不回应（对端问题不记业务错误计数，
    // SPEC-7 §7.3.3 Corrupted{where="tls-handshake"}）——表现为连接被关闭、无响应体
    const auto cert = find_test_cert("tls_test_cert.pem");
    const auto key = find_test_cert("tls_test_key.pem");
    ASSERT_FALSE(cert.empty());

    TempDir tmp("tinykv_tls_mismatch");
    auto started = StatusServer::start(
        "127.0.0.1", 0, [] { return std::string("{}"); }, {}, {}, {}, {},
        1, TlsOptions{cert.generic_string(), key.generic_string()});
    ASSERT_TRUE(started.has_value());
    const auto server = std::move(*started);

    asio::io_context ctx;
    asio::ip::tcp::socket plain(ctx);
    plain.connect({asio::ip::make_address("127.0.0.1"), server->port()});
    const std::string req = "GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(plain, asio::buffer(req));
    // 服务端在等 TLS ClientHello，收到明文 HTTP 后握手必失败并断连——读端以错误收场
    char buf[1024];
    asio::error_code ec;
    (void)plain.read_some(asio::buffer(buf), ec);
    EXPECT_TRUE(ec) << "明文打 TLS 端口竟收到数据（" << std::string(buf, 32)
                    << "……），握手守卫失效";
}

}  // namespace
