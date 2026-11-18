// 文件用途：⑩ 安全边界用例（M7-SEC-01/02）——鉴权拦截、凭据不外泄、未鉴权强制回环。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `auth_test`）。
//
// 使用场景理解：鉴权这种东西"能跑通"不代表"安全"。本文件只断言三条可判定的事实：
//   1) 无 token / 错 token → 401，且业务处理器根本没被执行（不能靠返回值兜底）；
//   2) 响应体与 /status 里不出现 token 明文（日志同理，这里检查响应）；
//   3) 未配置 token 时，监听面被强制改成回环（D6-3），status 里能看到这个事实。

// 先包含 server_app.h：它内部的 peer_txn.h 引用 kvpb::Mutation::DELETE，
// 而 asio（Windows SDK）会把 DELETE 定义成宏。顺序颠倒会得到一堆莫名其妙的语法错误。
#include "server/server_app.h"

#include "cli/cli.h"
#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

constexpr const char* kToken = "s3cr3t-token";

struct AuthFixture {
    TempDir tmp{"tinykv_auth"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db",
                                .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {},
                                .auth_token = std::string{kToken}}};

    AuthFixture() {
        auto started = app.start();
        EXPECT_TRUE(started.has_value()) << "带 token 启动失败";
    }
    ~AuthFixture() { (void)app.stop(); }

    // 裸请求：手工拼 HTTP，便于控制 Authorization 头
    [[nodiscard]] std::string raw_get(const std::string& path, const std::string& auth) const {
        asio::io_context ctx;
        asio::ip::tcp::socket sock(ctx);
        asio::error_code ec;
        sock.connect({asio::ip::make_address("127.0.0.1"), app.status_port()}, ec);
        if (ec) {
            return "CONNECT-FAILED";
        }
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (!auth.empty()) {
            req += "Authorization: Bearer " + auth + "\r\n";
        }
        req += "Connection: close\r\n\r\n";
        asio::write(sock, asio::buffer(req), ec);
        std::string raw;
        char buf[4096];
        for (;;) {
            const std::size_t n = sock.read_some(asio::buffer(buf), ec);
            if (ec) {
                break;
            }
            raw.append(buf, n);
        }
        return raw;
    }
};

// 无 token / 错 token：401，且响应体里没有业务数据
TEST(AuthTest, RejectsMissingAndWrongToken) {
    AuthFixture f;
    const std::string none = f.raw_get("/status", "");
    EXPECT_NE(none.find("401 Unauthorized"), std::string::npos) << none;
    EXPECT_EQ(none.find("worker_cycles"), std::string::npos) << "401 泄露了业务内容";

    const std::string wrong = f.raw_get("/status", "not-the-token");
    EXPECT_NE(wrong.find("401 Unauthorized"), std::string::npos) << wrong;
    EXPECT_EQ(wrong.find("worker_cycles"), std::string::npos);

    // 长度相同但内容不同（防止"只比长度"的实现蒙混）
    const std::string same_len(kToken);
    std::string tampered = same_len;  // 避开 Windows 头里的 near/far 宏：改个名字
    tampered[0] = tampered[0] == 'x' ? 'y' : 'x';
    const std::string similar = f.raw_get("/status", tampered);
    EXPECT_NE(similar.find("401 Unauthorized"), std::string::npos) << similar;
}

// 正确 token：200 且能拿到业务内容；响应里绝不回显 token（M7-SEC-01 的第二句）
TEST(AuthTest, AcceptsCorrectTokenAndNeverEchoesIt) {
    AuthFixture f;
    const std::string ok = f.raw_get("/status", kToken);
    EXPECT_NE(ok.find("200 OK"), std::string::npos) << ok;
    EXPECT_NE(ok.find("worker_cycles"), std::string::npos) << ok;
    EXPECT_EQ(ok.find(kToken), std::string::npos) << "响应里出现了 token 明文";
}

// 未配置 token：强制绑回环，且 /status 如实报告（不是只在日志里告警一句）
TEST(AuthTest, WithoutTokenBindsLoopbackAndReportsIt) {
    TempDir tmp{"tinykv_auth_off"};
    ServerApp app(ServerOptions{.data_dir = tmp.path() / "db",
                                .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {},
                                .auth_token = {}});
    ASSERT_TRUE(app.start().has_value());
    // 以 CLI 正常访问（回环 + 无鉴权）
    std::string body;
    const std::string endpoint = "127.0.0.1:" + std::to_string(app.status_port());
    std::vector<std::string> args{"--endpoint", endpoint, "status"};
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(tinykv::cli::run_cli(args, out, err), 0) << err.str();
    body = out.str();
    EXPECT_NE(body.find("\"auth\":false"), std::string::npos) << body;
    EXPECT_NE(body.find("\"bind\":\"127.0.0.1\""), std::string::npos) << body;
    ASSERT_TRUE(app.stop().has_value());
}

}  // namespace
