// 文件用途：确定性 Raft 集群夹具——N 个 peer 各持独立引擎与日志，消息经内存总线投递。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 为什么不用真网络：M2 的验收要的是"消息序列 → 状态断言"，真实 socket 只会引入时序噪声。
// 这也是 SPEC-3 反复强调的"子特性先在确定性模拟器跑通再上真网络"的最小版本。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "raft/raft.h"
#include "raft/raftlog.h"
#include "storage/engine.h"
#include "temp_dir.h"

namespace tinykv::testlib {

using tinykv::PeerId;
using tinykv::raft::Config;
using tinykv::raft::Raft;
using tinykv::raft::RaftLog;
using tinykv::raft::Ready;

// 构造一条日志条目（两个用例文件都要用，集中于此避免被判重复）
inline tinykv::raftpb::Entry make_entry(std::uint64_t index, std::uint64_t term,
                                        const std::string& data) {
    tinykv::raftpb::Entry e;
    e.set_index(index);
    e.set_term(term);
    e.set_data(data);
    return e;
}

// 构造一条 Raft 消息
inline tinykv::raftpb::Message make_msg(tinykv::raftpb::MessageId id, std::uint64_t term,
                                        std::uint64_t from, std::uint64_t to) {
    tinykv::raftpb::Message m;
    m.set_id(id);
    m.set_term(term);
    m.set_from(from);
    m.set_to(to);
    return m;
}

class Cluster {
public:
    explicit Cluster(int nodes, std::uint64_t election_timeout = 8,
                     std::uint64_t heartbeat = 3) {
        std::vector<PeerId> voters;
        for (int i = 0; i < nodes; ++i) {
            voters.push_back(PeerId{static_cast<std::uint64_t>(i) + 1});
        }
        for (int i = 0; i < nodes; ++i) {
            dirs_.push_back(std::make_unique<TempDir>("tinykv_cluster"));
            auto engine = tinykv::Engine::open({.dir = dirs_.back()->path() / "db"});
            engines_.push_back(std::move(*engine));
            auto log = RaftLog::open(*engines_.back());
            logs_.push_back(std::move(*log));
            Config cfg{.id = voters[static_cast<std::size_t>(i)],
                       .voters = voters,
                       .election_timeout = election_timeout,
                       .heartbeat_interval = heartbeat};
            nodes_.push_back(std::make_unique<Raft>(cfg, *logs_.back()));
        }
    }

    [[nodiscard]] int size() const { return static_cast<int>(nodes_.size()); }
    [[nodiscard]] Raft& node(int i) { return *nodes_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] RaftLog& log(int i) { return *logs_[static_cast<std::size_t>(i)]; }

    // 一轮投递：先取全部 Ready，再统一下发（保证"同轮消息互不可见"，逼近真实时序）
    bool deliver_round() {
        std::vector<tinykv::raftpb::Message> pending;
        for (auto& n : nodes_) {
            for (auto& m : n->take_ready().messages) {
                pending.push_back(std::move(m));
            }
        }
        if (pending.empty()) {
            return false;
        }
        for (const auto& m : pending) {
            const int to = static_cast<int>(m.to()) - 1;
            if (to >= 0 && to < size()) {
                (void)nodes_[static_cast<std::size_t>(to)]->step(m);
            }
        }
        return true;
    }

    void flush(int max_rounds = 300) {
        for (int i = 0; i < max_rounds && deliver_round(); ++i) {
        }
    }

    void tick_all() {
        for (auto& n : nodes_) {
            n->tick();
        }
    }

    // 单节点 tick（用于让指定节点先超时）
    void tick_one(int i) { nodes_[static_cast<std::size_t>(i)]->tick(); }

    [[nodiscard]] int leader_count() {
        int count = 0;
        for (const auto& n : nodes_) {
            count += n->is_leader() ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] int leader_index() {
        for (int i = 0; i < size(); ++i) {
            if (node(i).is_leader()) {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] bool terms_agree() {
        for (int i = 1; i < size(); ++i) {
            if (node(i).term() != node(0).term()) {
                return false;
            }
        }
        return true;
    }

    // 反复 tick 直到选出唯一 leader；返回是否成功（超时即失败，避免测试挂死）
    bool elect(int max_ticks = 400) {
        for (int i = 0; i < max_ticks; ++i) {
            tick_all();
            flush();
            if (leader_count() == 1) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::unique_ptr<TempDir>> dirs_;
    std::vector<std::unique_ptr<tinykv::Engine>> engines_;
    std::vector<std::unique_ptr<RaftLog>> logs_;
    std::vector<std::unique_ptr<Raft>> nodes_;
};

}  // namespace tinykv::testlib
