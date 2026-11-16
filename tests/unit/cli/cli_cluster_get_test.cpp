// 文件用途：集群 get + 三种读模式端到端——CLI → HTTP /kv → Peer 读（FUNC-05/06、UX-02）。
// 归属：TinyKV-CPP（M3 一致读）。
// 使用场景理解：读模式是**服务端语义**，客户端只能声明意图。这个用例把三者串起来验证：
// CLI 解析 --read-mode → HTTP 查询串 → 服务端按模式读 → 返回 JSON。单节点集群多数派为 1，
// ReadIndex 可立即确认，因此能在一进程内验证全链路而不引入多节点时序。

#include "cli/cli.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "peer_cluster.h"
#include "server/status_server.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::server::StatusServer;

// 极简查询串解析：key=..&mode=..
std::string query_value(const std::string& query, const std::string& name) {
    const auto at = query.find(name + "=");
    if (at == std::string::npos) {
        return {};
    }
    const auto begin = at + name.size() + 1;
    const auto end = query.find('&', begin);
    return query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

int run(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    return tinykv::cli::run_cli(args, out, err);
}

// 解析制表符文本：put\tkey\tvalue / del\tkey
struct WriteOp {
    std::string op;
    std::string key;
    std::string value;
    bool ok{false};
};

WriteOp parse_write(const std::string& body) {
    WriteOp out;
    const auto first = body.find('\t');
    if (first == std::string::npos) {
        return out;
    }
    out.op = body.substr(0, first);
    const auto second = body.find('\t', first + 1);
    if (second == std::string::npos) {
        out.key = body.substr(first + 1);
    } else {
        out.key = body.substr(first + 1, second - first - 1);
        out.value = body.substr(second + 1);
    }
    out.ok = out.op == "put" || out.op == "del";
    return out;
}

// 执行一次集群写：提案 → 泵到回执 → 文本响应
std::string do_write(PeerCluster& c, std::uint64_t& seq, const std::string& body) {
    const WriteOp op = parse_write(body);
    if (!op.ok || c.leader() < 0) {
        return "ERR bad-request";
    }
    Peer& p = c.peer(c.leader());
    tinykv::cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(11);
    req.mutable_header()->set_sequence_id(++seq);
    auto* one = req.add_requests();
    if (op.op == "put") {
        one->mutable_put()->set_key(op.key);
        one->mutable_put()->set_value(op.value);
    } else {
        one->mutable_delete_()->set_key(op.key);
    }
    if (!p.propose(req).has_value()) {
        return "ERR not-leader";
    }
    const auto want = seq;
    for (int i = 0; i < 50; ++i) {
        c.pump();
        if (p.take_reply(11, want).has_value()) {
            return "OK";
        }
    }
    return "ERR timeout";
}

// 单节点集群 + 状态端点（handler 直接调 Peer）
struct Fixture {
    std::unique_ptr<PeerCluster> cluster;
    std::unique_ptr<StatusServer> server;
    std::uint64_t seq{0};

    Fixture() {
        cluster = std::make_unique<PeerCluster>(1);
        EXPECT_TRUE(cluster->elect());
        const int leader = cluster->leader();
        EXPECT_GE(leader, 0);
        EXPECT_TRUE(cluster->write(leader, 1, ++seq, "k1", "v1"));
        Peer& peer = cluster->peer(leader);
        peer.enable_lease(true);

        auto handler = [this](const std::string& query) {
            Peer& p = cluster->peer(cluster->leader());
            const std::string key = query_value(query, "key");
            const std::string mode = query_value(query, "mode");
            std::string value;
            bool found = false;
            if (mode == "log") {
                tinykv::cmdpb::RaftCmdRequest req;
                req.mutable_header()->set_client_id(9);
                req.mutable_header()->set_sequence_id(++seq);
                req.add_requests()->mutable_get()->set_key(key);
                if (p.propose(req).has_value()) {
                    const auto want = seq;
                    for (int i = 0; i < 50; ++i) {
                        cluster->pump();
                        auto reply = p.take_read_reply(9, want);
                        if (reply.has_value()) {
                            found = reply->cmd_responses_size() > 0 &&
                                    reply->cmd_responses(0).get().found();
                            if (found) {
                                value = reply->cmd_responses(0).get().value();
                            }
                            break;
                        }
                    }
                }
            } else {
                // read_index / lease：单节点多数派为 1，确认后直接读状态机
                if (mode == "read_index") {
                    (void)p.begin_read_index();
                    for (int i = 0; i < 5; ++i) {
                        cluster->pump();
                        if (!p.take_confirmed_reads().empty()) {
                            break;
                        }
                    }
                }
                auto got = p.read_directly(tinykv::Cf::Default, key);
                if (got.has_value() && got->value.has_value()) {
                    found = true;
                    value = *got->value;
                }
            }
            return std::string("{\"found\":") + (found ? "true" : "false") +
                   ",\"value\":\"" + value + "\"}";
        };

        auto writer = [this](const std::string& body) {
            return do_write(*cluster, seq, body);
        };

        auto started = StatusServer::start(
            "127.0.0.1", 0, [] { return std::string("{\"leader\":1}"); }, handler, writer);
        EXPECT_TRUE(started.has_value());
        server = std::move(*started);
    }

    [[nodiscard]] std::string endpoint() const {
        return "127.0.0.1:" + std::to_string(server->port());
    }
};

TEST(CliClusterGet, ReadIndexModeReturnsValue) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "--read-mode", "read_index", "get", "k1"}, out, err), 0)
        << err.str();
    EXPECT_NE(out.str().find("\"found\":true"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("v1"), std::string::npos) << out.str();
}

TEST(CliClusterGet, LeaseModeReturnsValue) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "--read-mode", "lease", "get", "k1"}, out, err), 0)
        << err.str();
    EXPECT_NE(out.str().find("v1"), std::string::npos) << out.str();
}

TEST(CliClusterGet, LogModeReturnsValue) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "--read-mode", "log", "get", "k1"}, out, err), 0)
        << err.str();
    EXPECT_NE(out.str().find("v1"), std::string::npos) << out.str();
}

TEST(CliClusterGet, MissingKeyExitCodeThree) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "get", "absent"}, out, err), 3);
}

TEST(CliClusterGet, PutThenGetSeesValue) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "put", "k2", "v2"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("OK"), std::string::npos) << out.str();

    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "--read-mode", "read_index", "get", "k2"}, out2, err2), 0)
        << err2.str();
    EXPECT_NE(out2.str().find("v2"), std::string::npos) << out2.str();
}

TEST(CliClusterGet, DeleteThenGetNotFound) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "del", "k1"}, out, err), 0) << err.str();
    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "get", "k1"}, out2, err2), 3);
}

TEST(CliClusterGet, InvalidReadModeRejected) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "--read-mode", "bogus", "get", "k1"}, out, err), 1);
}

}  // namespace
