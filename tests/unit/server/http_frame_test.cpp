// 文件用途：HTTP 请求边界回归（P0-1 畸形参数 / P0-2 大帧截断）。
// 归属：TinyKV-CPP（M6 运行时修复）。
//
// 使用场景理解：这两个缺陷都只在"常驻进程 + 真实 socket"下暴露——进程内直调 handler 既
// 看不到异常穿出 io_context 后的 terminate，也看不到单次 read_some 的截断。故本用例
// 进程内起 ServerApp（真实监听），再用真实 HTTP 客户端打过去。

// 先包含 server_app.h：它内部的 peer_txn.h 引用 proto 的 Mutation::DELETE，
// 若 asio（Windows SDK）先入场会把 DELETE 宏展开，故顺序不可颠倒。
#include "cli/http_client.h"
#include "server/server_app.h"
#include "bounded_io.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

struct HttpFixture {
    TempDir tmp{"tinykv_http_frame"};
    ServerApp app{ServerOptions{.data_dir = tmp.path() / "db",
                                .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {},
                                .auth_token = {}}};

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

    f.app.advance_clock(31'000);  // 超过 30s 空闲线
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

TEST(HttpFrameTest, ObserveEndpointIsIndependentAndReportsApplyErrors) {
    HttpFixture f;
    // 观测端点独立端口 + 独立 io_context：业务端口仍正常，观测口不暴露写操作
    ASSERT_NE(f.app.observe_port(), 0);
    ASSERT_NE(f.app.observe_port(), f.app.status_port());
    const auto read = [&](const std::string& path) {
        return tinykv::cli::http_get("127.0.0.1", f.app.observe_port(), path);
    };
    const auto initial = read("/status");
    ASSERT_TRUE(initial.has_value());
    EXPECT_NE(initial->find("\"apply_errors\":0"), std::string::npos);
    EXPECT_NE(initial->find("\"txn_errors\":0"), std::string::npos);

    const auto put = f.post("/kv", "put\tobs-key\tobs-value");
    ASSERT_TRUE(put.has_value());
    ASSERT_EQ(*put, "OK");
    const auto biz_read = f.get("/kv?key=obs-key");
    ASSERT_TRUE(biz_read.has_value());
    EXPECT_NE(biz_read->find("\"found\":true"), std::string::npos);

    bool saw_update = false;
    std::string last_status;
    for (int i = 0; i < 20 && !saw_update; ++i) {
        const auto updated = read("/status");
        if (updated.has_value()) {
            last_status = *updated;
        }
        saw_update = updated.has_value() && updated->find("\"kv_writes\":1") != std::string::npos &&
                     updated->find("\"read_index_reads\":1") != std::string::npos;
    }
    EXPECT_TRUE(saw_update) << "观测端点应最終看到业务写入: " << last_status;

    const auto rejected =
        tinykv::cli::http_post("127.0.0.1", f.app.observe_port(), "/txn/begin", "begin");
    ASSERT_TRUE(rejected.has_value());
    EXPECT_TRUE(rejected->empty()) << "观测端口不得暴露事务写端点";
}

// BUG-0018: code-review P1-12——JSON 转义必须覆盖控制字符。
// 使用场景：value 里带换行/制表符（日志、多行文本都是这种值）。旧 escape 只转义 " 与 \，
// 于是响应体变成非法 JSON，调用方的解析器直接失败——数据没丢，但"读成功"变成了"读不了"。
TEST(HttpFrameTest, ControlCharsInValueProduceValidJson) {
    HttpFixture f;
    // 注意：帧协议用制表符分隔字段，因此 value 里不能含制表符（这是协议的边界，
    // 已在 docs/cli.md 的请求体格式一节写明）；换行与控制字符则必须能安全往返。
    const std::string value = "line1\nline2\"quoted\"";
    ASSERT_TRUE(f.post("/kv", "put\tck\t" + value).has_value());
    const auto body = f.get("/kv?key=ck");
    ASSERT_TRUE(body.has_value());
    // 必须转义：响应里不能出现裸换行/制表符，且 " 以 \" 形式出现
    EXPECT_EQ(body->find('\n'), std::string::npos) << "响应体含裸换行（非法 JSON）: " << *body;
    EXPECT_NE(body->find("\\n"), std::string::npos) << *body;
    EXPECT_NE(body->find("\\\""), std::string::npos) << *body;
    EXPECT_NE(body->find("line1"), std::string::npos) << "值必须能读回: " << *body;
}

// M7 验证清单第 1 条：同一条 TCP 上连续多个请求必须都得到响应（keep-alive）。
// 旧实现每请求新建连接并在响应后 close，"串行 vs 并发"的差距主要来自建连开销；
// 这里用裸 socket 在一个连接上连发两条请求（pipeline），断言收到两条 200。
TEST(HttpFrameTest, KeepsConnectionAliveAcrossMultipleRequests) {
    HttpFixture f;
    asio::io_context ctx;
    asio::ip::tcp::socket sock(ctx);
    asio::error_code ec;
    sock.connect({asio::ip::make_address("127.0.0.1"), f.app.status_port()}, ec);
    ASSERT_FALSE(ec) << "连接失败: " << ec.message();

    const std::string request = "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    asio::write(sock, asio::buffer(request + request), ec);  // 两条一起发：pipeline
    ASSERT_FALSE(ec) << ec.message();

    // BUG-0024 配套（r2 N-5 测试面）：读必须有界——旧写法阻塞在 read_some 里时
    // deadline 永远检查不到，服务端不应答就把测试挂死。改非阻塞 + 轮询。
    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::array<char, 4096> buf{};
    sock.non_blocking(true, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto count_responses = [](const std::string& text) {
        std::size_t n = 0;
        for (std::size_t at = text.find("HTTP/1.1 200"); at != std::string::npos;
             at = text.find("HTTP/1.1 200", at + 1)) {
            ++n;
        }
        return n;
    };
    while (count_responses(raw) < 2 && std::chrono::steady_clock::now() < deadline) {
        if (!tinykv::testlib::wait_readable(sock, std::chrono::milliseconds(500))) {
            break;  // 分片无数据：由外层 deadline 收口（ENG-04 无 sleep_for）
        }
        asio::error_code poll_ec;
        const std::size_t n = sock.read_some(asio::buffer(buf), poll_ec);
        if (poll_ec && poll_ec != asio::error::would_block) {
            ec = poll_ec;
            break;
        }
        raw.append(buf.data(), n);
    }
    EXPECT_EQ(count_responses(raw), 2U) << "同一连接上的第二个请求没有响应（未启用 keep-alive）";
    EXPECT_NE(raw.find("Connection: keep-alive"), std::string::npos)
        << "响应未声明 keep-alive: " << raw.substr(0, 120);
    // 服务端在 HTTP/1.1 + 无 Connection: close 时不得主动断开（否则第二条无从谈起）
    EXPECT_FALSE(ec) << "连接被提前关闭: " << ec.message();
}

}  // namespace
