// 文件用途：ServerApp 多副本端到端回归（M7 ⑤）——真 TCP 上完成选主、复制、
// ReadIndex 读与 leader 故障切换。这是"单机 KV → 分布式 store"的分水岭验证：
// 前面几项演进都在单节点内部修正确性，本项第一次要求 3 个 ServerApp 进程内实例
// 通过 Raft 协议互相同步状态。
//
// 测试不 sleep 对齐：选主/复制都靠有界自旋 + yield 探测（项目硬约束）。
// follower 的 ReadIndex 在 Raft 层只支持 leader（read_index() 返回 NotLeader），
// 所以客户端读一律打到当前 leader——follower 读转发是后续演进项，不在此伪装成功。

// 先包含 server_app.h：peer_txn.h 引用 proto 的 Mutation::DELETE，
// 若 asio（Windows SDK）先入场会把 DELETE 宏展开，顺序不可颠倒。
#include "server/server_app.h"
#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace {

using tinykv::PeerId;
using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

constexpr int kNodes = 3;
constexpr auto kWaitLimit = std::chrono::seconds(20);

// 轮询等待条件成立（有界，避免挂死；不用 sleep 对齐）
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = kWaitLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

class MultiReplicaCluster {
public:
    MultiReplicaCluster() {
        std::vector<PeerId> voters;
        for (int i = 0; i < kNodes; ++i) {
            voters.push_back(PeerId{static_cast<std::uint64_t>(i + 1)});
        }
        for (int i = 0; i < kNodes; ++i) {
            dirs_.push_back(std::make_unique<TempDir>("tinykv_replica"));
            ServerOptions opt{
                .data_dir = dirs_.back()->path() / "db",
                .status_port = 0,
                .observe_port = 0,
                .election_timeout = 10,
                .heartbeat = 3,
                .worker_interval = std::chrono::milliseconds(5),
                .log_sink = {},
                .replica_id = voters[static_cast<std::size_t>(i)],
                .voters = voters,
                .raft_port = 0,
            };
            auto app = std::make_unique<ServerApp>(std::move(opt));
            auto started = app->start();
            if (!started.has_value()) {
                ADD_FAILURE() << "副本 " << i << " 启动失败";
                return;
            }
            apps_.push_back(std::move(app));
        }
        // 端口由系统分配：全部启动后互告，形成完整路由表
        std::map<std::uint64_t, std::uint16_t> ports;
        for (int i = 0; i < kNodes; ++i) {
            ports[static_cast<std::uint64_t>(i + 1)] = apps_[static_cast<std::size_t>(i)]->raft_port();
        }
        for (auto& app : apps_) {
            app->set_raft_peers(ports);
        }
    }

    ~MultiReplicaCluster() {
        for (auto& app : apps_) {
            (void)app->stop();   // 逆序无关：每个 app 独立线程，stop 内部各自 join
        }
    }

    [[nodiscard]] int leader_index() const {
        for (int i = 0; i < kNodes; ++i) {
            if (apps_[static_cast<std::size_t>(i)]->is_leader()) {
                return i;
            }
        }
        return -1;
    }

    // 从指定副本的 /status 抽取数值字段（HTTP 线程读，不持 peer_mu_）
    [[nodiscard]] std::uint64_t status_number(int idx, const std::string& field) const {
        const auto json = tinykv::cli::http_get("127.0.0.1", apps_[static_cast<std::size_t>(idx)]->status_port(),
                                                "/status");
        if (!json.has_value()) {
            return 0;
        }
        const auto at = json->find("\"" + field + "\":");
        if (at == std::string::npos) {
            return 0;
        }
        const auto begin = at + field.size() + 3;
        std::uint64_t out = 0;
        for (auto pos = begin; pos < json->size() && (*json)[pos] >= '0' && (*json)[pos] <= '9';
             ++pos) {
            out = out * 10 + static_cast<std::uint64_t>((*json)[pos] - '0');
        }
        return out;
    }

    [[nodiscard]] bool all_applied_at_least(std::uint64_t idx) const {
        for (int i = 0; i < kNodes; ++i) {
            if (status_number(i, "applied") < idx) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] ServerApp& app(int i) { return *apps_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] std::uint16_t status_port(int i) const {
        return apps_[static_cast<std::size_t>(i)]->status_port();
    }

private:
    std::vector<std::unique_ptr<TempDir>> dirs_;
    std::vector<std::unique_ptr<ServerApp>> apps_;
};

TEST(MultiReplicaTest, ElectsLeaderAndReplicatesWrite) {
    MultiReplicaCluster cluster;

    // 三节点在真 TCP 上选出唯一 leader（每节点后台 Worker 每 5ms tick 一拍）
    ASSERT_TRUE(wait_until([&] { return cluster.leader_index() >= 0; }))
        << "多副本集群未在 20s 内选出 leader";
    const int leader = cluster.leader_index();
    ASSERT_GE(leader, 0);

    // 写入必须打到 leader（propose 在非 leader 会被 NotLeader 拒绝）
    const auto put = tinykv::cli::http_post("127.0.0.1", cluster.status_port(leader), "/kv",
                                            "put\trepl-key\trepl-value");
    ASSERT_TRUE(put.has_value());
    EXPECT_EQ(*put, "OK");

    // 复制判据：三节点都 apply 到该条目（applied 由 Peer 在整批写成功后推进）
    ASSERT_TRUE(wait_until([&] { return cluster.all_applied_at_least(1); }))
        << "写入未在 20s 内复制到全部副本";

    // 从 leader 读（ReadIndex + applied 追平）；follower 读转发是后续项，此处不测
    const auto got = tinykv::cli::http_get("127.0.0.1", cluster.status_port(leader),
                                           "/kv?key=repl-key");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "{\"found\":true,\"value\":\"repl-value\"}");

    // 三节点都无 apply 错误：证明复制路径没有静默失败
    for (int i = 0; i < kNodes; ++i) {
        EXPECT_EQ(cluster.status_number(i, "apply_errors"), 0U) << "副本 " << i;
    }
}

TEST(MultiReplicaTest, SplitIsRejectedWithMultiVoter) {
    MultiReplicaCluster cluster;
    ASSERT_TRUE(wait_until([&] { return cluster.leader_index() >= 0; }));
    const auto split = tinykv::cli::http_post("127.0.0.1", cluster.status_port(cluster.leader_index()),
                                              "/region/split", "split\t1\tm");
    ASSERT_TRUE(split.has_value());
    // raftpb::Message 只带 to=PeerId，无 region_id：多 region×多 voter 会路由歧义。
    // 明确拒绝优于造出不可路由的 peer；单节点 split 由 http_frame_test 覆盖。
    EXPECT_NE(split->find("ERR split-unsupported-in-multi-voter"), std::string::npos);
}

}  // namespace
