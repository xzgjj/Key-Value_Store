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
#include <string_view>
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
    // ReadIndex 已获 quorum 确认的位点（升序）。调用方据此放行等待中的读请求：
    // 等 applied >= 该位点后读状态机，才算线性一致。
    std::vector<std::uint64_t> confirmed_reads;
    // 需要用快照追赶的 follower（其 next_index 已落到 first_index 之前）。
    // 数据由 Peer 层填充（状态机不碰存储），这里只负责"发现谁需要"。
    std::vector<PeerId> snapshot_targets;
};

// 读模式：ReadIndex 必做，LeaseRead 默认关（SPEC-2 D2-4：开启需声明时钟漂移假设）
enum class ReadMode { ReadIndex, Lease, Log };

class Raft {
public:
    Raft(Config cfg, RaftLog& log);

    // 时钟驱动：leader 到点发心跳（只带元数据）；非 leader 到点发起选举
    void tick();

    [[nodiscard]] result<void> step(const raftpb::Message& msg);
    // leader 提案：追加一条 entry 并广播（非 leader 返回 NotLeader）
    [[nodiscard]] result<void> propose(std::string data);

    // M4 第④层：单步成员变更提案（仅 leader）。Entry 以 EntryConfChange 落盘，
    // apply 时才更新 membership（保证崩溃后可从日志重建）。
    [[nodiscard]] result<void> propose_conf_change(const raftpb::ConfChange& cc);

    // M4 第⑦层：把 leadership 交给 target（红线6）。目标须是 voter 且已追平才发 TimeoutNow；
    // 转移期拒新提案；一个 election timeout 内未完成即静默放弃（意图易失、不落日志）。
    [[nodiscard]] result<void> transfer_leader(PeerId target);
    [[nodiscard]] bool is_transferring() const { return transfer_target_.has_value(); }
    [[nodiscard]] std::optional<PeerId> transfer_target() const { return transfer_target_; }

    // 取走本轮输出（消息 + 待 apply）。这里推进的是**内存 dispatch 位点**；
    // 真正"已完成"的持久化位点由下面的 mark_applied 在状态机写完后确认。
    [[nodiscard]] Ready take_ready();

    // apply 成功后确认位点：先校验不超过 committed，再持久化到 RaftLog。
    // 失败必须让调用方停止后续确认——否则崩溃后会误认为这段已 apply。
    [[nodiscard]] result<void> mark_applied(LogIndex idx);

    // 发起一次 ReadIndex：返回轮次标识。获 quorum 确认后位点出现在 Ready.confirmed_reads。
    // 非 leader 返回 NotLeader；LeaseRead 开启且租约有效时调用方可以跳过本方法直接读。
    [[nodiscard]] result<std::uint64_t> read_index();

    // 租约：默认关。开启后每次 quorum 心跳回执刷新 deadline = now + election_timeout×0.7
    void enable_lease(bool on) { lease_enabled_ = on; }
    [[nodiscard]] bool lease_valid() const { return lease_enabled_ && now_ticks_ < lease_deadline_; }

    [[nodiscard]] LogIndex last_index() const { return log_.last_index(); }

    // 安装快照后调用：日志重置到 (index, term)，applied 前移，退回 follower。
    // 落盘失败返回错误——调用方不得在日志指针未推进时安装快照（P0-2）。
    [[nodiscard]] result<void> restore_snapshot(LogIndex index, Term term);

    // 持久化失败后的粘性失败态：状态机不再投票/提案/推进 commit，调用方必须停止接客。
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] const std::string& failure() const { return fail_reason_; }

    // 只有所有 voter 的 match_index 都到达 upto 才允许压缩日志，否则落后节点只能走快照。
    // 由 leader 判定；follower 不持有全量 match 视图，返回 false 保守不压缩。
    [[nodiscard]] bool can_compact(LogIndex upto) const;

    [[nodiscard]] Role role() const { return role_; }
    [[nodiscard]] bool is_leader() const { return role_ == Role::Leader; }
    [[nodiscard]] Term term() const { return Term{term_}; }
    [[nodiscard]] PeerId leader_id() const { return PeerId{leader_}; }
    [[nodiscard]] std::uint64_t election_elapsed() const { return election_elapsed_; }
    [[nodiscard]] LogIndex committed() const { return log_.committed(); }
    [[nodiscard]] const std::vector<PeerId>& voters() const { return cfg_.voters; }
    [[nodiscard]] const std::vector<PeerId>& learners() const { return learners_; }

private:
    [[nodiscard]] bool is_voter(PeerId p) const;
    // M4 ④b：是否 learner（只收日志、不投票不计 majority）
    [[nodiscard]] bool is_learner(PeerId p) const;
    // M4 ④b：learner 是否已追平——日志差 <100 且该状态持续 >=5s（now_ticks 单调计数）。
    // 这是 AddVoter 的前置门槛（D3-5）：未追平就提升会把落后节点计入多数派、拉低可用性。
    [[nodiscard]] bool is_learner_caught_up(PeerId p) const;
    // 每拍刷新追平计时：差首次 <100 记起点，退回 >=100 清零（仅 leader 需要）
    void refresh_learner_catchup();
    // ⑦a：目标追平后发一次 TimeoutNow（未追平先等 append 回执追上来）
    void maybe_send_timeout_now();
    // ⑦a：清除转移状态（成功、放弃、降级三处共用）
    void clear_transfer();
    // ⑦a：某 peer 的 match 前移后的转移钩子（把分支挡在 handle_append_response 之外）
    void on_match_advanced(PeerId from);
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
    void handle_heartbeat_response(const raftpb::Message& msg);
    // ⑦a：收到 TimeoutNow → 立即竞选（learner 忽略；已是 leader 忽略）
    void handle_timeout_now(const raftpb::Message& msg);
    [[nodiscard]] result<void> append_entry_and_replicate(const std::string& data);

    // 把 [applied+1, committed] 的条目收进待 apply 队列；apply 时处理 ConfChange。
    // 任一条目读不出来 → 返回 Corrupted 且**不推进** applied_（P0-3：静默跳过 = 丢写）。
    [[nodiscard]] result<void> collect_apply();
    // apply 一条 ConfChange：更新 voters/learners（幂等，支撑日志重放重建）
    void apply_conf_change(const raftpb::ConfChange& cc);

    // 持久化失败即进入粘性失败态：不再投票/提案/推进 commit，错误经 step/propose 上抛。
    void fail(std::string_view why);
    [[nodiscard]] result<void> check_failed() const;

    Config cfg_;
    std::vector<PeerId> learners_;   // M4 第④层：只收日志、不投票不计入 majority
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
    struct PendingRead {
        std::uint64_t index{0};   // read_index = 发起时的 committed 位点
        std::size_t acks{0};      // 已确认的副本数（含自己）
    };

    std::vector<raftpb::Message> outbox_;
    std::vector<Entry> to_apply_;
    std::vector<std::uint64_t> confirmed_reads_;
    std::vector<PeerId> snapshot_targets_;
    std::map<std::uint64_t, PendingRead> pending_reads_;
    std::uint64_t read_ctx_seq_{0};
    std::uint64_t now_ticks_{0};        // 单调 tick 计数（租约/追平判定用，不读墙钟保证可复现）
    std::uint64_t lease_deadline_{0};
    bool lease_enabled_{false};
    // M4 ④b：learner 追平计时——记录"日志差首次进入 <100"的 tick，持续够 5s 才算追平。
    // 差重新 >=100 时清零重计；用 tick 而非墙钟，模拟器可复现。
    std::map<std::uint64_t, std::uint64_t> learner_catch_since_;  // peer_id → tick
    // M4 ⑦a：leadership 转移意图（易失，不落日志）。deadline 到点即放弃；
    // notified 保证一次转移只发一个 TimeoutNow（重复发只会让目标反复抬 term 打断服务）。
    std::optional<PeerId> transfer_target_;
    std::uint64_t transfer_deadline_{0};
    bool transfer_notified_{false};
    bool failed_{false};        // 粘性失败：持久化链路已不可信（P0-2）
    std::string fail_reason_;   // 首次失败的诊断信息（不覆盖，保留根因）
};

}  // namespace tinykv::raft
