// 文件用途：真 TCP 三节点写压测——用连续泵线程替代"一次写一轮泵"，量化 fsync 与批量收益（PERF-01/02）。
// 归属：TinyKV-CPP（M2 集群性能，LEVEL=bench → nightly）。
//
// 为什么需要专门的驱动：此前的进程内基准是"每次写都把消息泵到回执"，泵的粒度是整轮，
// 一轮只推进一跳 RTT，于是 251 ops/s 这个数字里混着测试装置的固定开销，不能当产品结论。
// 这里把泵与压测拆成两个线程：泵线程全速轮转（poll→step→on_ready→send），
// 客户端线程只管提交并等回执——这才是"节点在跑，客户端在压"的形状。
//
// 三个阶段刻意分开，用来**定位瓶颈**而不是只报一个总数：
//   A. fsync=on  batch=1   → 端到端真实数字
//   B. fsync=off batch=1   → 与 A 的差值就是 fsync 成本
//   C. fsync=off batch=64  → 批量收益（共识摊薄）

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "raft_test_base.h"
#include "raftstore/peer.h"
#include "tcp_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)

// 提交项：客户端 id/seq 兼作回执键
struct Submission {
    std::uint64_t client{0};
    std::uint64_t seq{0};
    cmdpb::RaftCmdRequest req;
};

// 一个简化版 Worker：泵线程独占 Peer，客户端通过队列提交、通过回执表取结果
class NodeGroup {
public:
    explicit NodeGroup(bool fsync_on) {
        (void)start_transports(kNodes, transports_);   // 端口自动分配并互相 set_peers

        const std::vector<PeerId> voters = make_voters(kNodes);
        for (int i = 0; i < kNodes; ++i) {
            dirs_.push_back(std::make_unique<TempDir>("tinykv_tcp_perf"));
            auto engine = tinykv::Engine::open(
                {.dir = dirs_.back()->path() / "db", .fsync_on_commit = fsync_on});
            EXPECT_TRUE(engine.has_value());
            engines_.push_back(std::move(*engine));
            tinykv::raft::Config cfg{.id = voters[static_cast<std::size_t>(i)],
                                     .voters = voters,
                                     .election_timeout = 40,
                                     .heartbeat_interval = 10};
            auto peer = Peer::open(cfg, *engines_.back());
            EXPECT_TRUE(peer.has_value());
            peers_.push_back(std::move(*peer));
        }
        pump_thread_ = std::thread([this] { pump_loop(); });
    }

    ~NodeGroup() {
        stop_.store(true);
        if (pump_thread_.joinable()) {
            pump_thread_.join();   // 绝不 detach
        }
    }

    [[nodiscard]] int leader() {
        std::lock_guard lock(leader_mu_);
        return leader_;
    }

    void submit(const Submission& s) {
        std::lock_guard lock(queue_mu_);
        queue_.push_back(s);
    }

    [[nodiscard]] bool take_reply(std::uint64_t client, std::uint64_t seq) {
        std::lock_guard lock(reply_mu_);
        return replies_.erase({client, seq}) > 0;
    }

    // 自旋等待 leader 选出（不 sleep：sleep 对齐启动是违禁模式）
    bool wait_leader(int spins = 2000000) {
        for (int i = 0; i < spins; ++i) {
            if (leader() >= 0) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

private:
    void pump_loop() {
        while (!stop_.load()) {
            // 时钟必须由泵线程推进：否则既没有选举也没有心跳（这是"泵"的职责之一）
            if (++spin_ % 200 == 0) {
                for (auto& p : peers_) {
                    p->tick();
                }
            }
            for (std::size_t i = 0; i < peers_.size(); ++i) {
                for (auto& msg : transports_[i]->poll()) {
                    (void)peers_[i]->step_message(msg);
                }
                (void)peers_[i]->on_ready();
                for (auto& out : peers_[i]->take_messages()) {
                    (void)transports_[i]->send(PeerId{out.to()}, out);
                }
            }
            drain_queue();
            update_leader();
            std::this_thread::yield();
        }
    }

    void drain_queue() {
        std::vector<Submission> pending;
        {
            std::lock_guard lock(queue_mu_);
            pending.swap(queue_);
        }
        for (const auto& s : pending) {
            for (auto& p : peers_) {
                if (p->is_leader()) {
                    (void)p->propose(s.req);
                    break;
                }
            }
        }
    }

    void update_leader() {
        int found = -1;
        for (int i = 0; i < static_cast<int>(peers_.size()); ++i) {
            if (peers_[static_cast<std::size_t>(i)]->is_leader()) {
                found = i;
                break;
            }
        }
        std::lock_guard lock(leader_mu_);
        leader_ = found;
        // 回执统一收集到共享表，供客户端线程取
        std::lock_guard rlock(reply_mu_);
        for (auto& p : peers_) {
            for (std::uint64_t c = 1; c <= 4; ++c) {
                for (std::uint64_t s = last_seq_[c] + 1; s <= s + 64; ++s) {
                    auto r = p->take_reply(c, s);
                    if (!r.has_value()) {
                        break;
                    }
                    replies_[{c, s}] = true;
                    last_seq_[c] = s;
                }
            }
        }
    }

    std::vector<std::unique_ptr<TcpTransport>> transports_;
    std::vector<std::unique_ptr<TempDir>> dirs_;
    std::vector<std::unique_ptr<tinykv::Engine>> engines_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::thread pump_thread_;
    std::atomic<bool> stop_{false};

    std::mutex queue_mu_;
    std::vector<Submission> queue_;
    std::mutex reply_mu_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, bool> replies_;
    std::mutex leader_mu_;
    std::atomic<int> leader_{-1};
    std::uint64_t last_seq_[5]{};
    std::uint64_t spin_{0};
};

// 压测一轮：ops 次提交，逐条等回执（客户端视角的真实时延）
double run_batch(NodeGroup& g, std::uint64_t client, int ops, int per_request) {
    const auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    for (int i = 0; i < ops; ++i) {
        const auto seq = static_cast<std::uint64_t>(i) + 1;
        cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(client);
        req.mutable_header()->set_sequence_id(seq);
        for (int k = 0; k < per_request; ++k) {
            auto* one = req.add_requests();
            one->mutable_put()->set_key("p:" + std::to_string(i) + ":" + std::to_string(k));
            one->mutable_put()->set_value("v");
        }
        g.submit(Submission{.client = client, .seq = seq, .req = req});
        for (int spin = 0; spin < 5000000; ++spin) {
            if (g.take_reply(client, seq)) {
                ++done;
                break;
            }
            std::this_thread::yield();
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double records = static_cast<double>(done) * static_cast<double>(per_request);
    return records / secs;
}

TEST(TcpPerf, FsyncCostAndBatchAmplification) {
    // A：fsync 开启，单条写——端到端真实数字
    double a = 0.0;
    {
        NodeGroup g(true);
        ASSERT_TRUE(g.wait_leader()) << "未选出 leader";
        a = run_batch(g, 1, 30, 1);
    }
    // B：fsync 关闭，单条写——与 A 的差值即 fsync 成本
    double b = 0.0;
    {
        NodeGroup g(false);
        ASSERT_TRUE(g.wait_leader());
        b = run_batch(g, 2, 200, 1);
    }
    // C：fsync 关闭，批量 64——共识摊薄后的上限
    double c = 0.0;
    {
        NodeGroup g(false);
        ASSERT_TRUE(g.wait_leader());
        c = run_batch(g, 3, 60, 64);
    }

    std::printf("[perf] A 真TCP fsync=on  batch=1  : %.0f records/s\n", a);
    std::printf("[perf] B 真TCP fsync=off batch=1  : %.0f records/s\n", b);
    std::printf("[perf] C 真TCP fsync=off batch=64 : %.0f records/s\n", c);
    if (a > 0) {
        std::printf("[perf] fsync 成本倍数 = %.1fx（B/A）\n", b / a);
    }
    if (b > 0) {
        std::printf("[perf] 批量收益 = %.1fx（C/B）\n", c / b);
    }

    EXPECT_GT(a, 0.0);
    // fsync 是单条写的决定性成本：关掉它必须显著更快，否则说明瓶颈在别处（值得追）
    EXPECT_GT(b, a) << "关掉 fsync 没有变快，瓶颈不在落盘——需重新分析";
    EXPECT_GT(c, b) << "批量没有收益，共识摊薄失效——需重新分析";
}

}  // namespace
