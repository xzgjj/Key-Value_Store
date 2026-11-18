// 文件用途：⑭ TLS 配置语义用例（M8-SEC-01，SPEC-7 §7.7 `tls_config_test`）——
//          ① 半配置（只给 cert）启动即拒绝（错误矩阵：tls_cert 与 tls_key 必须同给）；
//          ② 未配置 TLS = M7 行为逐字节（明文往返照常，强制回环守卫不动，红线 3）。
// 归属：TinyKV-CPP（M8 优化，LEVEL=unit → fast）。

#include "server/server_app.h"

#include "cli/cli.h"

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

// ① 半配置：只给 tls_cert → ServerApp::start 返回 InvalidArgument 且信息含"同给"
TEST(TlsConfig, PartialTlsConfigIsRejectedAtStart) {
    TempDir tmp("tinykv_tls_config");
    ServerOptions opt;
    opt.data_dir = tmp.path() / "db";
    opt.status_port = 0;
    opt.worker_interval = std::chrono::milliseconds(2);
    opt.tls_cert = "whatever.pem";  // 只给 cert 不给 key
    ServerApp app(std::move(opt));
    const auto started = app.start();
    ASSERT_FALSE(started.has_value()) << "半配置竟然启动成功";
    const std::string why = tinykv::describe(started.error());
    EXPECT_NE(why.find("同给"), std::string::npos) << why;
    EXPECT_NE(tinykv::describe_kind(started.error()).find("InvalidArgument"),
              std::string::npos) << why;
}

// ② 未配置 TLS：明文往返照常 + 强制回环守卫逐字节不动（红线 3 的"未配置"半边）
TEST(TlsConfig, NoTlsKeepsM7PlaintextBehavior) {
    TempDir tmp("tinykv_tls_config_plain");
    ServerOptions opt;
    opt.data_dir = tmp.path() / "db";
    opt.status_port = 0;
    opt.worker_interval = std::chrono::milliseconds(2);
    // 显式要求对外监听且不配 token：无 TLS 时必须被强制回环（D6-3 守卫不动）
    opt.host = "192.0.2.1";  // TEST-NET-1：非回环
    ServerApp app(std::move(opt));
    ASSERT_TRUE(app.start().has_value());
    // /status 的 "bind" 字段可证绑定面被改写为回环（D6-3 语义，M7 起如此）
    const std::vector<std::string> args{"--endpoint",
                                        "127.0.0.1:" + std::to_string(app.status_port()),
                                        "status"};
    std::ostringstream o;
    std::ostringstream e;
    const int rc = tinykv::cli::run_cli(args, o, e);
    ASSERT_EQ(rc, 0) << e.str();
    EXPECT_NE(o.str().find("\"bind\":\"127.0.0.1\""), std::string::npos) << o.str();
    EXPECT_EQ(o.str().find("\"tls\""), std::string::npos)
        << "未配 TLS 时 /status 不得出现 tls 字段（红线 3：逐字节不变）";
}

}  // namespace
