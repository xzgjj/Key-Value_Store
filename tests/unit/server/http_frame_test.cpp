// 文件用途：HTTP 请求边界回归（P0-1 畸形参数 / P0-2 大帧截断）。
// 归属：TinyKV-CPP（M6 运行时修复）。
//
// 使用场景理解：这两个缺陷都只在"常驻进程 + 真实 socket"下暴露——进程内直调 handler 既
// 看不到异常穿出 io_context 后的 terminate，也看不到单次 read_some 的截断。故本用例
// 进程内起 ServerApp（真实监听），再用真实 HTTP 客户端打过去。

// 先包含 server_app.h：它内部的 peer_txn.h 引用 proto 的 Mutation::DELETE，
// 若 asio（Windows SDK）先入场会把 DELETE 宏展开，故顺序不可颠倒。
#include "server/server_app.h"
#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

struct HttpFixture {
    TempDir tmp{"tinykv_http_frame"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db", .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {}}};

    HttpFixture() {
        auto started = app.start();
        EXPECT_TRUE(started.has_value()) << "daemon 启动失败";
    }
    ~HttpFixture() { (void)app.stop(); }

    [[nodiscard]] auto get(const std::string& path) const {
        return tinykv::cli::http_get("127.0.0.1", app.status_port(), path);
    }
    [[nodiscard]] auto post(const std::string& path, const std::string& body) const {
        return tinykv::cli::http_post("127.0.0.1", app.status_port(), path, body);
    }
};

TEST(HttpFrameTest, MalformedQueryDoesNotKillDaemon) {
    HttpFixture f;
    // BUG-0002: GET /region?id=abc 曾让 std::stoull 抛出并 terminate 整个常驻进程
    const auto bad = f.get("/region?id=abc");
    ASSERT_TRUE(bad.has_value());
    const auto alive = f.get("/status");
    ASSERT_TRUE(alive.has_value()) << "畸形请求后进程必须仍在服务";
    EXPECT_NE(alive->find("worker_cycles"), std::string::npos);

    // 同族入口：非数字 region id / 非数字 GC safe point 都只回错误
    const auto split = f.post("/region/split", "split\tabc\tk");
    ASSERT_TRUE(split.has_value());
    EXPECT_NE(split->find("ERR bad-request"), std::string::npos);
    const auto gc = f.post("/txn/gc", "gc\tnot-a-number");
    ASSERT_TRUE(gc.has_value());
    EXPECT_NE(gc->find("ERR bad-request"), std::string::npos);
    const auto still = f.get("/status");
    ASSERT_TRUE(still.has_value());
    EXPECT_NE(still->find("worker_cycles"), std::string::npos);
}

TEST(HttpFrameTest, LargeBodyIsNotTruncated) {
    HttpFixture f;
    // BUG-0003: 2000 字节 value 曾被单次 1024 字节 read_some 静默截断为 863 字节却回 OK
    const std::string value(2000, 'X');
    const auto put = f.post("/kv", "put\tbigkey\t" + value);
    ASSERT_TRUE(put.has_value());
    EXPECT_EQ(*put, "OK");
    const auto got = f.get("/kv?key=bigkey");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "{\"found\":true,\"value\":\"" + value + "\"}");
}

TEST(HttpFrameTest, AdvancedClockIsObservable) {
    HttpFixture f;
    // 注入时钟是 TTL 决议的输入：推进后必须在 /status 可见（主循环每秒调用 advance_clock）
    const auto before = f.get("/status");
    ASSERT_TRUE(before.has_value());
    EXPECT_NE(before->find("\"now_ms\":0"), std::string::npos);
    f.app.advance_clock(5000);
    const auto after = f.get("/status");
    ASSERT_TRUE(after.has_value());
    EXPECT_NE(after->find("\"now_ms\":5000"), std::string::npos);
}

TEST(HttpFrameTest, IdleSessionsAndRepliesAreReclaimed) {
    HttpFixture f;
    const auto began = f.post("/txn/begin", "begin");
    ASSERT_TRUE(began.has_value());
    const auto busy = f.get("/status");
    ASSERT_TRUE(busy.has_value());
    EXPECT_NE(busy->find("\"sessions\":1"), std::string::npos);

    f.app.advance_clock(31'000);   // 超过 30s 空闲线
    f.app.sweep_sessions();
    const auto swept = f.get("/status");
    ASSERT_TRUE(swept.has_value());
    EXPECT_NE(swept->find("\"sessions\":0"), std::string::npos);

    for (int i = 0; i < 20; ++i) {
        const auto put = f.post("/kv", "put\tk" + std::to_string(i) + "\tv");
        ASSERT_TRUE(put.has_value());
        ASSERT_EQ(*put, "OK");
    }
    // 组合根轮询 applied 位点、不取回执；显式回收后回执表必须保持为空
    EXPECT_EQ(f.app.pending_replies(), 0U);
}

TEST(HttpFrameTest, RawKeysSurviveSplit) {
    HttpFixture f;
    // BUG-0004: raw 键与 MVCC 键共用 Default CF；旧实现把 ≥8 字节的 raw 键误剥 8B ts，
    // split 后留在左引擎而路由指向右引擎，读回 found:false（静默丢数据）。
    const std::vector<std::string> keys{"abc", "abcdefgh", "abcdefghijkl"};
    for (const auto& key : keys) {
        const auto put = f.post("/kv", "put\t" + key + "\tv-" + key);
        ASSERT_TRUE(put.has_value());
        ASSERT_EQ(*put, "OK");
    }
    const auto split = f.post("/region/split", "split\t1\tabcdefgh");
    ASSERT_TRUE(split.has_value());
    ASSERT_EQ(*split, "OK split");
    for (const auto& key : keys) {
        const auto got = f.get("/kv?key=" + key);
        ASSERT_TRUE(got.has_value()) << key;
        EXPECT_EQ(*got, "{\"found\":true,\"value\":\"v-" + key + "\"}") << key;
    }
}

}  // namespace
