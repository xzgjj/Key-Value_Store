// 文件用途：Raft 纯状态机——Step/Tick 入、Ready 出，内部零锁零 IO 线程（SPEC-2 §2.3.1/§2.4.1）。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03/04、REL-01/02）。
//
// 设计原则（继承 TinyKV/etcd 路线）：一端输入（tick / step）、另一端输出（Ready），
// 状态机内部不持锁、不起线程、不碰网络——所有并发与 IO 推到外层 Worker。
// 这样同一份状态机可以用确定性消息序列直接单测，也可以在模拟器里跑随机注入。
//
// 持久化边界：日志与 hard state 由 RaftLog 立即落盘（本设计里 log 架在 M1 引擎上），
// 因此 Ready 只承载"要发出去的消息"与"要 apply 的已提交条目"，不再有"待持久化"队列。
// 安全前提不变：**先持久化 term/vote 再对外发消息**，由 campaign/投票分支内联完成。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/error.h"
#include "common/ids.h"
#include "raft/raftlog.h"

namespace tinykv::raft {

enum class Role { Follower, Candidate, Leader };

// 单个 peer 的配置
struct Config {
    PeerId id{0};
    std::vector<PeerId> voters;              // 含自己
    std::uint64_t election_timeout{10};      // tick 数（模拟器可缩到几十）
    std::uint64_t heartbeat_interval{3};     // tick 数，须 ≤ election_timeout/2
};

// 一次输出：消息 + 待 apply 的已提交条目
struct Ready {
    std::vector<raftpb::Message> messages;
    std::vector<Entry> to_apply;
};

class Raft {
public:
    Raft(Config cfg, RaftLog& log);

    // 时钟驱动：leader 到点发心跳（只带元数据）；非 leader 到点发起选举
    void tick();

    [[nodiscard]] result<void> step(const raftpb::Message& msg);
    // leader 提案：追加一条 entry 并广播（非 leader 返回 NotLeader）
    [[nodiscard]] result<void> propose(std::string data);

    // 取走本轮输出（消息 + 待 apply），并推进 applied 指针
    [[nodiscard]] Ready take_ready();

    [[nodiscard]] Role role() const { return role_; }
    [[nodiscard]] bool is_leader() const { return role_ == Role::Leader; }
    [[nodiscard]] Term term() const { return Term{term_}; }
    [[nodiscard]] PeerId leader_id() const { return PeerId{leader_}; }
    [[nodiscard]] std::uint64_t election_elapsed() const { return election_elapsed_; }
    [[nodiscard]] LogIndex committed() const { return log_.committed(); }
    [[nodiscard]] const std::vector<PeerId>& voters() const { return cfg_.voters; }

private:
    [[nodiscard]] bool is_voter(PeerId p) const;
    [[nodiscard]] std::size_t majority() const { return cfg_.voters.size() / 2 + 1; }
    [[nodiscard]] raftpb::Message make(raftpb::MessageId id, PeerId to) const;

    void become_follower(std::uint64_t term, std::optional<PeerId> vote);
    void campaign();
    void become_leader();
    void broadcast_append();
    void broadcast_heartbeat();
    void send_append(PeerId to);
    void advance_commit();

    [[nodiscard]] result<void> handle_append(const raftpb::Message& msg);
    [[nodiscard]] result<void> merge_entries(const raftpb::Message& msg);
    void reject_append(PeerId to, std::uint64_t index, std::uint64_t log_term);
    void reply_append_ok(PeerId to);
    void handle_append_response(const raftpb::Message& msg);
    void handle_vote_request(const raftpb::Message& msg);
    void handle_vote_response(const raftpb::Message& msg);
    void handle_heartbeat(const raftpb::Message& msg);
    void append_entry_and_replicate(const std::string& data);

    // 把 [applied+1, committed] 的条目收进待 apply 队列
    void collect_apply();

    Config cfg_;
    RaftLog& log_;
    Role role_{Role::Follower};
    std::uint64_t term_{0};
    std::optional<PeerId> voted_for_;
    std::uint64_t leader_{0};                 // 0 = 未知
    std::uint64_t election_timeout_{0};   // 节点专属超时（确定性错峰，避免同拍分裂）
    std::uint64_t election_elapsed_{0};
    std::uint64_t heartbeat_elapsed_{0};
    std::uint64_t applied_{0};
    std::vector<PeerId> votes_;               // 本轮已收到的赞成票
    std::map<std::uint64_t, std::uint64_t> next_index_;   // per follower
    std::map<std::uint64_t, std::uint64_t> match_index_;  // per follower
    std::vector<raftpb::Message> outbox_;
    std::vector<Entry> to_apply_;
};

}  // namespace tinykv::raft
