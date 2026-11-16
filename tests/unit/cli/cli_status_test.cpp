// 文件用途：集群模式 status——从真实 HTTP 端点取 JSON，且写操作被明确拒绝（UX-04）。
// 归属：TinyKV-CPP（M2 RaftKV）。
// 使用场景理解：status 是只读的可观测通道，必须能跨进程取到；而写在集群模式需要提案回调，
// 当前没做，所以必须**明确拒绝**而不是假装成功——假成功会让脚本以为写进去了。

#include "cli/cli.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "server/status_server.h"

namespace {

using tinykv::server::StatusServer;

// span<const string> 不能直接从 {"a","b"} 推导，这里显式构造
int run(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    return tinykv::cli::run_cli(args, out, err);
}

std::string endpoint_of(const StatusServer& server) {
    return "127.0.0.1:" + std::to_string(server.port());
}

TEST(CliClusterStatus, FetchesJsonFromEndpoint) {
    auto server = StatusServer::start("127.0.0.1", 0,
                                      [] { return std::string(R"({"leader":1,"term":3})"); });
    ASSERT_TRUE(server.has_value()) << "状态端点启动失败";

    std::ostringstream out;
    std::ostringstream err;
    const int code = run({"--endpoint", endpoint_of(**server), "status"}, out, err);
    EXPECT_EQ(code, 0) << err.str();
    EXPECT_NE(out.str().find("\"leader\""), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"term\":3"), std::string::npos) << out.str();
}

TEST(CliClusterStatus, WriteRejectedInClusterMode) {
    // 集群模式写需要提案回调（apply 后回执），当前未实现——必须报用法错误而不是静默忽略
    std::ostringstream out;
    std::ostringstream err;
    const int code =
        run({"--endpoint", "127.0.0.1:1", "put", "k", "v"}, out, err);
    EXPECT_EQ(code, 1);
    EXPECT_NE(err.str().find("status"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("get"), std::string::npos) << err.str();
}

TEST(CliClusterStatus, UnknownEndpointReportsRuntimeError) {
    std::ostringstream out;
    std::ostringstream err;
    // 端口 1 几乎不可能有服务在听：应报运行错误（退出码 2），而不是崩或假成功
    const int code = run({"--endpoint", "127.0.0.1:1", "status"}, out, err);
    EXPECT_EQ(code, 2);
    EXPECT_FALSE(err.str().empty());
}

}  // namespace
