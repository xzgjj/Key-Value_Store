// 文件用途：三节点 Peer 集群夹具（进程内总线）——供读基准与线性一致检查共用。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 与 raft_cluster.h 的区别：那个只连 Raft 状态机，这个连完整 Peer（提案→apply→存储），
// 因此能做"真读真写"的基准与一致性检查。消息仍在进程内路由，避免把网络抖动混进读延迟。

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <algorithm>
#include "common/error_format.h"
#include "raft/raft.h"
#include "raft_test_base.h"

#include "raftstore/peer.h"
#include "storage/engine.h"
#include "temp_dir.h"

namespace tinykv::testlib {

using tinykv::raft::ReadMode;
using tinykv::raftstore::Peer;

// ---- M4 共享 region 断言助手（⑦c/⑦d 与 bench 共用，集中于此避免复制粘贴） ----

// REL-05 的可判定形式：区间排序后首尾相接（首 start=""、尾 end=""、相邻共享边界）
[[nodiscard]] inline bool regions_contiguous(std::vector<tinykv::raftstore::Region> regions) {
    std::sort(regions.begin(), regions.end(),
              [](const tinykv::raftstore::Region& a, const tinykv::raftstore::Region& b) {
                  return a.start_key < b.start_key;
              });
    if (regions.empty() || !regions.front().start_key.empty() || !regions.back().end_key.empty()) {
        return false;
    }
    for (std::size_t i = 1; i < regions.size(); ++i) {
        if (regions[i - 1].end_key != regions[i].start_key) {
            return false;
        }
    }
    return true;
}

// region 视图指纹（跨副本一致性比对用）
[[nodiscard]] inline std::string regions_fingerprint(
    std::vector<tinykv::raftstore::Region> regions) {
    std::sort(regions.begin(), regions.end(),
              [](const tinykv::raftstore::Region& a, const tinykv::raftstore::Region& b) {
                  return a.start_key < b.start_key;
              });
    std::string out;
    for (const auto& r : regions) {
        out += std::to_string(r.id.value) + ":" + r.start_key + ".." + r.end_key + ";";
    }
    return out;
}

// Split admin 提案构造（split_test/mr_chaos/bench 共用）
[[nodiscard]] inline tinykv::cmdpb::RaftCmdRequest make_split_admin(std::uint64_t client,
                                                                    std::uint64_t seq,
                                                                    const std::string& key,
                                                                    std::uint64_t new_region_id) {
    tinykv::cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    auto* admin = req.mutable_admin();
    admin->mutable_split()->set_split_key(key);
    admin->mutable_split()->set_new_region_id(new_region_id);
    return req;
}

class PeerCluster {
public:
    explicit PeerCluster(int nodes, std::uint64_t election_timeout = 8,
                         std::uint64_t heartbeat = 3) {
        const std::vector<PeerId> voters = make_voters(nodes);
        for (int i = 0; i < nodes; ++i) {
            dirs_.push_back(std::make_unique<TempDir>("tinykv_peers"));
            auto engine = tinykv::Engine::open({.dir = dirs_.back()->path() / "db"});
            engines_.push_back(std::move(*engine));
            tinykv::raft::Config cfg{.id = voters[static_cast<std::size_t>(i)],
                                     .voters = voters,
                                     .election_timeout = election_timeout,
                                     .heartbeat_interval = heartbeat};
            auto peer = Peer::open(cfg, *engines_.back());
            peers_.push_back(std::move(*peer));
        }
    }

    [[nodiscard]] int size() const { return static_cast<int>(peers_.size()); }
    [[nodiscard]] Peer& peer(int i) { return *peers_[static_cast<std::size_t>(i)]; }

    // M4 ⑦c：装配 Split 元数据——每个 peer 一份独立 StoreMeta（真实拓扑里每个 store 各持
    // 各自的 region 视图，各自 apply 各自更新；共享单视图会让后 apply 的 peer 撞"region 不存在"）。
    void enable_split(const tinykv::raftstore::Region& region) {
        for (auto& p : peers_) {
            auto meta = std::make_unique<tinykv::raftstore::StoreMeta>();
            (void)meta->add_region(region);
            p->set_region(region);
            p->set_store_meta(meta.get());
            metas_.push_back(std::move(meta));
        }
    }

    [[nodiscard]] tinykv::raftstore::StoreMeta& meta(int i) {
        return *metas_[static_cast<std::size_t>(i)];
    }

    // 一轮：逐节点"发完即投递、随后 apply"。单趟而非两趟——真实 Worker 也是按 region
    // 各自 drain，同轮消息不保证互相可见（两趟会让同轮消息立刻可见，比真机更"顺"）。
    // 分区：集合内节点与外界的一切消息都被丢弃（模拟网络隔离）
    void set_partition(std::set<int> nodes) { partition_ = std::move(nodes); }

    void pump() {
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            const bool from_isolated = partition_.count(static_cast<int>(i)) > 0;
            for (auto& m : peers_[i]->take_messages()) {
                const int to = static_cast<int>(m.to()) - 1;
                if (from_isolated || partition_.count(to) > 0) {
                    continue;  // 隔离节点发不出、也收不到
                }
                if (to >= 0 && to < size()) {
                    (void)peers_[static_cast<std::size_t>(to)]->step_message(m);
                }
            }
            if (!from_isolated) {
                (void)peers_[i]->on_ready();
            }
        }
    }

    void pump(int rounds) {
        for (int i = 0; i < rounds; ++i) {
            pump();
        }
    }

    bool elect(int max_ticks = 400) {
        for (int i = 0; i < max_ticks; ++i) {
            for (auto& p : peers_) {
                p->tick();
            }
            pump();
            if (leader() >= 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int leader() const {
        for (int i = 0; i < size(); ++i) {
            if (peers_[static_cast<std::size_t>(i)]->is_leader()) {
                return i;
            }
        }
        return -1;
    }

    // 带等待的写：提案后泵到回执出现（返回 false 表示超时，测试里按失败处理）
    bool write(int node, std::uint64_t client, std::uint64_t seq, const std::string& key,
               const std::string& value, int max_pumps = 200) {
        auto req = make_put_request(client, seq, key, value);
        if (!peers_[static_cast<std::size_t>(node)]->propose(req).has_value()) {
            return false;
        }
        for (int i = 0; i < max_pumps; ++i) {
            pump();
            if (peers_[static_cast<std::size_t>(node)]->take_reply(client, seq).has_value()) {
                return true;
            }
        }
        return false;
    }

    // 三种读模式：Lease / ReadIndex / Log（Log 走共识，是 PERF-03 的对比基线）
    [[nodiscard]] tinykv::result<std::optional<std::string>> read(int node, const std::string& key,
                                                                  ReadMode mode,
                                                                  std::uint64_t client,
                                                                  std::uint64_t seq) {
        Peer& p = peer(node);
        if (mode == ReadMode::Lease && p.lease_valid()) {
            return direct(p, key);
        }
        if (mode == ReadMode::Log) {
            return read_via_log(p, key, client, seq);
        }
        return read_index_mode(p, key);
    }

private:
    [[nodiscard]] static tinykv::result<std::optional<std::string>> direct(Peer& p,
                                                                           const std::string& key) {
        auto got = p.read_directly(tinykv::Cf::Default, key);
        if (!got) {
            return tinykv::unexpected(got.error());
        }
        return got->value;
    }

    [[nodiscard]] tinykv::result<std::optional<std::string>> read_via_log(Peer& p,
                                                                          const std::string& key,
                                                                          std::uint64_t client,
                                                                          std::uint64_t seq) {
        tinykv::cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(client);
        req.mutable_header()->set_sequence_id(seq);
        req.add_requests()->mutable_get()->set_key(key);
        if (!p.propose(req).has_value()) {
            return tinykv::unexpected(
                tinykv::NotLeader{.region_id = tinykv::RegionId{0}, .leader_hint = std::nullopt});
        }
        for (int i = 0; i < 200; ++i) {
            pump();
            auto reply = p.take_read_reply(client, seq);
            if (!reply.has_value()) {
                continue;
            }
            if (reply->cmd_responses_size() == 0 || !reply->cmd_responses(0).get().found()) {
                return std::optional<std::string>{};
            }
            return std::optional<std::string>{reply->cmd_responses(0).get().value()};
        }
        return tinykv::unexpected(tinykv::IoFailed{.path = "peer_cluster", .reason = "log 读超时"});
    }

    [[nodiscard]] tinykv::result<std::optional<std::string>> read_index_mode(
        Peer& p, const std::string& key) {
        if (!p.begin_read_index().has_value()) {
            return tinykv::unexpected(
                tinykv::NotLeader{.region_id = tinykv::RegionId{0}, .leader_hint = std::nullopt});
        }
        std::uint64_t confirmed = 0;
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            pump();
            for (const auto idx : p.take_confirmed_reads()) {
                confirmed = idx;
                have = true;
            }
        }
        if (!have) {
            return tinykv::unexpected(
                tinykv::IoFailed{.path = "peer_cluster", .reason = "ReadIndex 未确认"});
        }
        for (int i = 0; i < 200 && !p.applied_at_least(confirmed); ++i) {
            pump();
        }
        return direct(p, key);
    }

    std::vector<std::unique_ptr<TempDir>> dirs_;
    std::vector<std::unique_ptr<tinykv::Engine>> engines_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<std::unique_ptr<tinykv::raftstore::StoreMeta>> metas_;
    std::set<int> partition_;
};

// 唯一 leader 计数（收敛判据共用）
[[nodiscard]] inline int count_leaders(PeerCluster& c) {
    int n = 0;
    for (int i = 0; i < c.size(); ++i) {
        n += c.peer(i).is_leader() ? 1 : 0;
    }
    return n;
}

// ---- 单 peer 规模基准共用装置（M8 ⑬：scale_split_bench 与 split_bench 消除复制粘贴，
//      同 server_daemon.h 的提取先例——CI 的 dup_code 检查判复制即提取） ----

// 定宽补零键：字典序 == 数值序（split_bench 首跑的真实踩坑："k125000" < "k62500"，
// 字典序与数值序相反——不定宽补零，split 点的"落在左区间"判断会被字符串序打乱）
[[nodiscard]] inline std::string make_padded_key(std::int64_t i, int width = 7) {
    auto digits = std::to_string(i);
    return "k" + std::string(static_cast<std::size_t>(width) - digits.size(), '0') + digits;
}

// 区间批量 put 提案（键 = make_padded_key(i)，值 = "v"；规模写入只考察体量与顺序）
[[nodiscard]] inline tinykv::cmdpb::RaftCmdRequest make_put_batch(std::uint64_t client,
                                                                  std::uint64_t seq,
                                                                  std::int64_t begin,
                                                                  std::int64_t end) {
    tinykv::cmdpb::RaftCmdRequest req;
    req.mutable_header()->set_client_id(client);
    req.mutable_header()->set_sequence_id(seq);
    for (std::int64_t i = begin; i < end; ++i) {
        auto* one = req.add_requests();
        one->mutable_put()->set_key(make_padded_key(i));
        one->mutable_put()->set_value("v");
    }
    return req;
}

// 单 peer 组装配：region 1 注册进 meta → 打开 peer → 接线 → 有界 tick 推成 leader。
// 返回 nullptr 表示装配失败（调用方 ASSERT 断言）。
[[nodiscard]] inline std::unique_ptr<tinykv::raftstore::Peer> open_single_leader_peer(
    tinykv::Engine& engine, tinykv::raftstore::StoreMeta& meta, std::int64_t keys_hint = 0) {
    (void)keys_hint;
    tinykv::raft::Config cfg{.id = PeerId{1}, .voters = {PeerId{1}},
                             .election_timeout = 4, .heartbeat_interval = 2};
    auto peer_result = tinykv::raftstore::Peer::open(cfg, engine);
    if (!peer_result.has_value()) {
        return nullptr;
    }
    auto peer = std::move(*peer_result);
    tinykv::raftstore::Region region{.id = tinykv::RegionId{1}, .start_key = "", .end_key = "",
                                     .peers = {}, .epoch = {1, 0}, .size_hint = 0};
    if (!meta.add_region(region).has_value()) {
        return nullptr;
    }
    peer->set_region(region);
    peer->set_store_meta(&meta);
    for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
        peer->tick();
    }
    if (!peer->is_leader()) {
        return nullptr;
    }
    return peer;
}

// 有界轮询 on_ready 直到 client/seq 的回执出现（ENG-04：禁 sleep 对齐，轮询有界）。
// apply 拒绝的原因记入 *apply_err（非空时），供失败断言定位。
inline bool wait_reply(tinykv::raftstore::Peer& peer, std::uint64_t client, std::uint64_t seq,
                       int max_rounds, std::string* apply_err = nullptr) {
    for (int round = 0; round < max_rounds; ++round) {
        auto r = peer.on_ready();
        if (!r) {
            if (apply_err != nullptr && apply_err->empty()) {
                *apply_err = std::string(tinykv::describe(r.error()));
            }
            continue;
        }
        if (peer.take_reply(client, seq).has_value()) {
            return true;
        }
        peer.tick();
    }
    return false;
}

}  // namespace tinykv::testlib
