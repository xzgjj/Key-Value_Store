// 文件用途：组合根端到端用例（M6 第③④层）：daemon 装配 + CLI 全链路。
// 归属：TinyKV-CPP（M6 运行时）。
// 使用场景理解：ServerApp 就是"生产形态的组装"——本用例在进程内启动它（真实 HTTP 监听），
// 再用 CLI 逻辑层（run_cli）经 127.0.0.1 打过去，验证：
//   (a) 装配与生命周期：start 后就绪、stop 后端口下线（幂等）；
//   (b) kv 面：put 经 raft 提案 → get 读到（"写走共识"不豁免）；
//   (c) 会话作用域：txn begin 拿 session → set → get（读己之写）→ commit → 集群 get 可见
//       ——这就是"CLI 交互式事务"解锁的可判定证据；
//   (d) GC 端点：/txn/gc 走共识返回 OK；
//   (e) /status 暴露可观测点（worker_cycles 增长、sessions 计数）。

#include "cli/cli.h"
#include "server/server_app.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;

// 有界等待（ENG-04 禁 sleep_for）
bool spin_until(const std::function<bool()>& pred, int ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

// 指定 endpoint 的 CLI 调用（不绑定夹具生命周期：跨重启的两段进程各自建 app）
int cli_at(const std::string& endpoint, const std::vector<std::string>& args,
           std::string* out_text = nullptr) {
    std::vector<std::string> full{"--endpoint", endpoint};
    full.insert(full.end(), args.begin(), args.end());
    std::ostringstream out;
    std::ostringstream err;
    const int rc = tinykv::cli::run_cli(full, out, err);
    if (out_text != nullptr) {
        *out_text = out.str();
    }
    return rc;
}

struct DaemonFixture {
    TempDir tmp{"tinykv_daemon"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {}}};
    std::string endpoint;

    DaemonFixture() {
        auto r = app.start();
        EXPECT_TRUE(r.has_value()) << "daemon 启动失败";
        endpoint = "127.0.0.1:" + std::to_string(app.status_port());
    }

    ~DaemonFixture() { (void)app.stop(); }

    // CLI 逻辑层调用（真实 HTTP 到本进程 daemon）
    int cli(const std::vector<std::string>& args, std::string* out_text = nullptr) {
        std::vector<std::string> full{"--endpoint", endpoint};
        full.insert(full.end(), args.begin(), args.end());
        std::ostringstream out;
        std::ostringstream err;
        const int rc = tinykv::cli::run_cli(full, out, err);
        last_err = err.str();
        if (out_text != nullptr) {
            *out_text = out.str();
        }
        return rc;
    }
    std::string last_err;
};

TEST(ServerAppTest, StartsStopsAndServesStatus) {
    DaemonFixture f;
    ASSERT_GT(f.app.status_port(), 0);
    std::string body;
    ASSERT_EQ(f.cli({"status"}, &body), 0) << f.last_err;
    EXPECT_NE(body.find("worker_cycles"), std::string::npos);
    EXPECT_NE(body.find("\"regions\":1"), std::string::npos);
    // worker 泵在后台推进
    EXPECT_TRUE(spin_until([&] {
        std::string b;
        (void)f.cli({"status"}, &b);
        const auto at = b.find("\"worker_cycles\":");
        return at != std::string::npos &&
               std::stoull(b.substr(at + 16)) > 0;
    })) << "Worker 未驱动泵循环";
}

TEST(ServerAppTest, KvWriteGoesThroughRaftAndReadsBack) {
    DaemonFixture f;
    ASSERT_EQ(f.cli({"put", "k1", "v1"}), 0) << f.last_err;
    std::string body;
    ASSERT_EQ(f.cli({"get", "k1"}, &body), 0) << f.last_err;
    EXPECT_NE(body.find("\"value\":\"v1\""), std::string::npos) << body;
}

// 会话作用域：交互式事务全链路（begin → set → get → commit → 集群读可见）
TEST(ServerAppTest, InteractiveTxnSessionEndToEnd) {
    DaemonFixture f;
    std::string begin;
    ASSERT_EQ(f.cli({"txn", "begin"}, &begin), 0) << f.last_err;
    const auto at = begin.find("session=");
    ASSERT_NE(at, std::string::npos) << begin;
    const std::string sid = begin.substr(at + 8);
    // 去掉可能的空白/换行
    std::string clean;
    for (const char ch : sid) {
        if (ch == ' ' || ch == '\n' || ch == '\r') {
            break;
        }
        clean.push_back(ch);
    }

    ASSERT_EQ(f.cli({"txn", "set", clean, "tk", "tv"}), 0) << f.last_err;
    std::string got;
    ASSERT_EQ(f.cli({"txn", "get", clean, "tk"}, &got), 0) << f.last_err;
    EXPECT_NE(got.find("tv"), std::string::npos) << "读己之写失败: " << got;

    std::string commit;
    ASSERT_EQ(f.cli({"txn", "commit", clean}, &commit), 0) << f.last_err;
    EXPECT_NE(commit.find("commit_ts="), std::string::npos) << commit;

    // 提交后对新事务可见（MVCC 数据只能经快照读看到——raw get 读的是用户键，不解释版本）
    std::string begin2;
    ASSERT_EQ(f.cli({"txn", "begin"}, &begin2), 0) << f.last_err;
    const auto at2 = begin2.find("session=");
    ASSERT_NE(at2, std::string::npos) << begin2;
    std::string sid2;
    for (std::size_t i = at2 + 8; i < begin2.size(); ++i) {
        if (begin2[i] == ' ' || begin2[i] == '\n' || begin2[i] == '\r') {
            break;
        }
        sid2.push_back(begin2[i]);
    }
    std::string body;
    ASSERT_EQ(f.cli({"txn", "get", sid2, "tk"}, &body), 0) << f.last_err;
    EXPECT_NE(body.find("tv"), std::string::npos) << body;
    ASSERT_EQ(f.cli({"txn", "rollback", sid2}), 0) << f.last_err;

    // 会话已关闭：再 set 必须失败（退出码非 0）
    EXPECT_NE(f.cli({"txn", "set", clean, "tk", "tv2"}), 0);
}

TEST(ServerAppTest, GcEndpointRunsThroughRaft) {
    DaemonFixture f;
    ASSERT_EQ(f.cli({"put", "g1", "a"}), 0) << f.last_err;
    ASSERT_EQ(f.cli({"put", "g1", "b"}), 0) << f.last_err;
    std::string body;
    ASSERT_EQ(f.cli({"txn", "gc"}, &body), 0) << f.last_err;
    EXPECT_NE(body.find("OK"), std::string::npos) << body;
    // GC 后最新值仍可读（活跃读不受影响——经快照读验证）
    std::string begin;
    ASSERT_EQ(f.cli({"txn", "begin"}, &begin), 0) << f.last_err;
    const auto at = begin.find("session=");
    ASSERT_NE(at, std::string::npos) << begin;
    std::string sid;
    for (std::size_t i = at + 8; i < begin.size(); ++i) {
        if (begin[i] == ' ' || begin[i] == '\n' || begin[i] == '\r') {
            break;
        }
        sid.push_back(begin[i]);
    }
    std::string got;
    ASSERT_EQ(f.cli({"txn", "get", sid, "g1"}, &got), 0) << f.last_err;
    EXPECT_NE(got.find("a"), std::string::npos) << "GC 后最新可读版本: " << got;
    ASSERT_EQ(f.cli({"txn", "rollback", sid}), 0) << f.last_err;
}

TEST(ServerAppTest, InterceptorCountsRequestsAndLogSinkReceives) {
    std::vector<std::string> lines;
    TempDir tmp{"tinykv_daemon_intercept"};
    ServerApp app(ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = [&](const std::string& l) { lines.push_back(l); }});
    ASSERT_TRUE(app.start().has_value());
    const std::string endpoint = "127.0.0.1:" + std::to_string(app.status_port());
    const std::vector<std::string> args{"--endpoint", endpoint, "put", "ik", "iv"};
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(tinykv::cli::run_cli(args, out, err), 0) << err.str();
    EXPECT_FALSE(lines.empty()) << "log 插拔位未收到请求";
    EXPECT_NE(lines.front().find("POST /kv"), std::string::npos) << lines.front();
    std::ostringstream st;
    std::ostringstream sterr;
    const std::vector<std::string> status_args{"--endpoint", endpoint, "status"};
    ASSERT_EQ(tinykv::cli::run_cli(status_args, st, sterr), 0);
    EXPECT_NE(st.str().find("\"intercept_calls\":"), std::string::npos) << st.str();
    EXPECT_TRUE(app.stop().has_value());
}

// 多 region（M6 挂账收口）：split 动态建 peer + 数据迁移 + 按 key 路由
TEST(ServerAppTest, SplitCreatesPeerAndRoutesByKey) {
    DaemonFixture f;
    // 1) split 前写两侧数据（都在 region 1）
    ASSERT_EQ(f.cli({"put", "a1", "left"}), 0) << f.last_err;
    ASSERT_EQ(f.cli({"put", "z1", "right"}), 0) << f.last_err;
    // 2) 在 "m" 切分 region 1 → 左 [ ,m) / 右 [m, )
    std::string split_out;
    ASSERT_EQ(f.cli({"region", "split", "1", "m"}, &split_out), 0)
        << "split 失败: " << f.last_err << " / " << split_out;
    // 3) 元数据与实例都增长：regions=2, peers=2, splits=1
    std::string status;
    ASSERT_EQ(f.cli({"status"}, &status), 0) << f.last_err;
    EXPECT_NE(status.find("\"regions\":2"), std::string::npos) << status;
    EXPECT_NE(status.find("\"peers\":2"), std::string::npos) << status;
    EXPECT_NE(status.find("\"splits\":1"), std::string::npos) << status;
    // 4) 迁移正确：左侧 key 仍可读，右侧 key 在新 peer 上可读
    std::string left;
    ASSERT_EQ(f.cli({"get", "a1"}, &left), 0) << f.last_err;
    EXPECT_NE(left.find("left"), std::string::npos) << left;
    std::string right;
    ASSERT_EQ(f.cli({"get", "z1"}, &right), 0) << f.last_err;
    EXPECT_NE(right.find("right"), std::string::npos) << right;
    // 5) 新写入按 key 路由：右半新 key 落到 region 2
    ASSERT_EQ(f.cli({"put", "n1", "new-right"}), 0) << f.last_err;
    std::string got;
    ASSERT_EQ(f.cli({"get", "n1"}, &got), 0) << f.last_err;
    EXPECT_NE(got.find("new-right"), std::string::npos) << got;
    // 6) region 列表可见两个 region
    std::string regions;
    ASSERT_EQ(f.cli({"regions"}, &regions), 0) << f.last_err;
    EXPECT_NE(regions.find("\"id\":2"), std::string::npos) << regions;
}

// 多级 split：新 peer 也挂了 observer，右半可再切分
TEST(ServerAppTest, RepeatedSplitCreatesThirdPeer) {
    DaemonFixture f;
    ASSERT_EQ(f.cli({"put", "b", "v"}), 0) << f.last_err;
    ASSERT_EQ(f.cli({"region", "split", "1", "m"}), 0) << f.last_err;
    ASSERT_EQ(f.cli({"region", "split", "2", "t"}), 0) << f.last_err;
    std::string status;
    ASSERT_EQ(f.cli({"status"}, &status), 0) << f.last_err;
    EXPECT_NE(status.find("\"regions\":3"), std::string::npos) << status;
    EXPECT_NE(status.find("\"peers\":3"), std::string::npos) << status;
    std::string got;
    ASSERT_EQ(f.cli({"get", "b"}, &got), 0) << f.last_err;
    EXPECT_NE(got.find("v"), std::string::npos) << got;
}

// BUG-0006: ARCH-1 P0-4——重启后没有挂起条目时，"不写先读"必须可用。
// 使用场景：进程正常关停（applied == committed，重放队列为空），重启后只读业务先到达。
// 旧实现里 Peer::applied_ 默认 0、只在有 apply 批次时赋值，于是 ReadIndex 永远等不到追平，
// 每次读都返回 IoFailed 直到有写入把管线踢活——典型的"测试绿、部署红"。
// 本用例走的正是 ServerApp 装配路径（既有 applied_recovery_test 不走这里）。
TEST(ServerAppTest, RestartWithoutPendingEntriesKeepsReadable) {
    TempDir tmp{"tinykv_daemon_restart"};
    const auto data_dir = tmp.path() / "db";
    std::uint16_t port = 0;
    {
        ServerApp app(ServerOptions{.data_dir = data_dir, .status_port = 0,
                                    .worker_interval = std::chrono::milliseconds(2),
                                    .log_sink = {}});
        ASSERT_TRUE(app.start().has_value());
        port = app.status_port();
        ASSERT_GT(port, 0);
        const std::string endpoint = "127.0.0.1:" + std::to_string(port);
        for (const auto& args : std::vector<std::vector<std::string>>{
                 {"put", "rk1", "rv1"}, {"put", "rk2", "rv2"}}) {
            std::vector<std::string> full{"--endpoint", endpoint};
            full.insert(full.end(), args.begin(), args.end());
            std::ostringstream out;
            std::ostringstream err;
            ASSERT_EQ(tinykv::cli::run_cli(full, out, err), 0) << err.str();
        }
        ASSERT_TRUE(app.stop().has_value());
    }
    // 重启：读写分离——先只读，不写任何东西
    ServerApp app(ServerOptions{.data_dir = data_dir, .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {}});
    ASSERT_TRUE(app.start().has_value());
    const std::string endpoint = "127.0.0.1:" + std::to_string(app.status_port());
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string> args{"--endpoint", endpoint, "get", "rk1"};
    ASSERT_EQ(tinykv::cli::run_cli(args, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("rv1"), std::string::npos)
        << "重启后未写入即读失败（applied 游标未播种）: " << out.str();
    ASSERT_TRUE(app.stop().has_value());
}

// BUG-0017: code-review P0-2——TSO 重启必须从"盘上最大 ts"之后继续分配。
// 使用场景：先提交一笔事务（commit_ts 落在盘上），重启守护进程，再开新事务读它。
// 旧实现 TSO 从 0 重新计数：新 start_ts=1 < 历史 commit_ts → 快照读返回空（静默读不到），
// 且任何写都会因 latest_commit_bound > start_ts 命中 WriteConflict（事务面实际不可写）。
TEST(ServerAppTest, TsoResumesAboveExistingCommitTsAfterRestart) {
    TempDir tmp{"tinykv_tso_restart"};
    const auto data_dir = tmp.path() / "db";
    {
        ServerApp app(ServerOptions{.data_dir = data_dir, .status_port = 0,
                                    .worker_interval = std::chrono::milliseconds(2),
                                    .log_sink = {}});
        ASSERT_TRUE(app.start().has_value());
        const std::string endpoint = "127.0.0.1:" + std::to_string(app.status_port());
        // begin 拿 session
        std::string begin_text;
        ASSERT_EQ(cli_at(endpoint, {"txn", "begin"}, &begin_text), 0);
        const auto at = begin_text.find("session=");
        ASSERT_NE(at, std::string::npos) << begin_text;
        std::string sid;
        for (std::size_t i = at + 8; i < begin_text.size(); ++i) {
            const char ch = begin_text[i];
            if (ch == '\n' || ch == '\r' || ch == ' ') {
                break;
            }
            sid.push_back(ch);
        }
        for (const auto& step : std::vector<std::vector<std::string>>{
                 {"txn", "set", sid, "tso_k", "v1"}, {"txn", "commit", sid}}) {
            std::vector<std::string> full{"--endpoint", endpoint};
            full.insert(full.end(), step.begin(), step.end());
            std::ostringstream out;
            std::ostringstream e;
            ASSERT_EQ(tinykv::cli::run_cli(full, out, e), 0) << e.str();
        }
        ASSERT_TRUE(app.stop().has_value());
    }
    // 重启：新事务必须看到重启前提交的数据
    ServerApp restarted(ServerOptions{.data_dir = data_dir, .status_port = 0,
                                      .worker_interval = std::chrono::milliseconds(2),
                                      .log_sink = {}});
    ASSERT_TRUE(restarted.start().has_value());
    const std::string endpoint = "127.0.0.1:" + std::to_string(restarted.status_port());
    std::string begin2_text;
    ASSERT_EQ(cli_at(endpoint, {"txn", "begin"}, &begin2_text), 0);
    const auto at2 = begin2_text.find("session=");
    ASSERT_NE(at2, std::string::npos) << begin2_text;
    std::string sid2;
    for (std::size_t i = at2 + 8; i < begin2_text.size(); ++i) {
        const char ch = begin2_text[i];
        if (ch == '\n' || ch == '\r' || ch == ' ') {
            break;
        }
        sid2.push_back(ch);
    }
    std::string got;
    ASSERT_EQ(cli_at(endpoint, {"txn", "get", sid2, "tso_k"}, &got), 0);
    EXPECT_NE(got.find("v1"), std::string::npos)
        << "重启后新事务读不到历史提交（TSO 从 0 重新计数）: " << got;
    ASSERT_TRUE(restarted.stop().has_value());
}

TEST(ServerAppTest, StopIsIdempotentAndPortGoesDown) {
    TempDir tmp{"tinykv_daemon_stop"};
    ServerApp app(ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                        .log_sink = {}});
    ASSERT_TRUE(app.start().has_value());
    const auto port = app.status_port();
    ASSERT_GT(port, 0);
    ASSERT_TRUE(app.stop().has_value());
    EXPECT_TRUE(app.stop().has_value());   // 幂等
    // 端口已下线：CLI 访问应失败（非 0 退出码）
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string> args{"--endpoint", "127.0.0.1:" + std::to_string(port),
                                        "status"};
    const int rc = tinykv::cli::run_cli(args, out, err);
    EXPECT_NE(rc, 0) << "stop 后端口仍可服务";
}

}  // namespace
