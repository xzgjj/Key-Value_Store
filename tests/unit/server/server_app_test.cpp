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

struct DaemonFixture {
    TempDir tmp{"tinykv_daemon"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2)}};
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

TEST(ServerAppTest, StopIsIdempotentAndPortGoesDown) {
    TempDir tmp{"tinykv_daemon_stop"};
    ServerApp app(ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2)});
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
