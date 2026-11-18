// 文件用途：M7 观测/管理端点收口用例——CLI 三个"悬空命令"现在真的有人接。
// 归属：TinyKV-CPP（M7：ARCH-1 P1-1 的收口，`server_app_test` 同层的快速验证）。
//
// 使用场景理解：这三个命令（`region peers`、`scheduler status`、`trace region ... on|off`）
// 曾经在 CLI 里存在、服务端没人接，于是恒 404。契约层面"发了但没人接"比"没有这个命令"更糟：
// 使用者会以为功能坏了。本文件逐条钉住"命令能拿到结构化响应"。

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

struct Fixture {
    TempDir tmp{"tinykv_admin_endpoints"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db",
                                .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {},
                                .auth_token = {}}};

    Fixture() { EXPECT_TRUE(app.start().has_value()); }
    ~Fixture() { (void)app.stop(); }

    [[nodiscard]] int cli(const std::vector<std::string>& args, std::string* out = nullptr,
                          std::string* err = nullptr) {
        std::vector<std::string> full{"--endpoint",
                                      "127.0.0.1:" + std::to_string(app.status_port())};
        full.insert(full.end(), args.begin(), args.end());
        std::ostringstream o;
        std::ostringstream e;
        const int rc = tinykv::cli::run_cli(full, o, e);
        if (out != nullptr) {
            *out = o.str();
        }
        if (err != nullptr) {
            *err = e.str();
        }
        return rc;
    }
};

TEST(AdminEndpointsTest, RegionPeersReturnsPeerList) {
    Fixture f;
    std::string out;
    std::string err;
    ASSERT_EQ(f.cli({"region", "peers", "1"}, &out, &err), 0) << err;
    EXPECT_NE(out.find("\"peer_id\""), std::string::npos) << "副本分布应返回结构化列表: " << out;
    EXPECT_NE(out.find("\"leader\""), std::string::npos) << out;
    // 不存在的 region 仍是 404（返回空体 → CLI 非 0）
    std::string missing;
    EXPECT_NE(f.cli({"region", "peers", "999"}, &missing), 0);
}

TEST(AdminEndpointsTest, SchedulerStatusReportsFacts) {
    Fixture f;
    std::string out;
    ASSERT_EQ(f.cli({"scheduler", "status"}, &out), 0) << out;
    EXPECT_NE(out.find("\"regions\":1"), std::string::npos) << out;
    EXPECT_NE(out.find("\"leaders\":1"), std::string::npos) << out;
    EXPECT_NE(out.find("pd-lite"), std::string::npos) << out;
}

TEST(AdminEndpointsTest, TraceToggleIsAcceptedAndVisible) {
    Fixture f;
    std::string out;
    ASSERT_EQ(f.cli({"trace", "region", "1", "off"}, &out), 0) << out;
    EXPECT_NE(out.find("OK trace off"), std::string::npos) << out;
    ASSERT_EQ(f.cli({"trace", "region", "1", "on"}, &out), 0) << out;
    EXPECT_NE(out.find("OK trace on"), std::string::npos) << out;
    // 非法开关值：明确报错，不静默接受
    EXPECT_NE(f.cli({"trace", "region", "1", "maybe"}, &out), 0);
}

}  // namespace
