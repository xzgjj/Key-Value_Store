// 文件用途：raft.h 的实现——选举、复制、commit 推进与四条红线分支。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03/04）。

#include "raft/raft.h"

#include <algorithm>
#include <span>
#include <utility>

namespace tinykv::raft {
namespace {

bool same(const PeerId& a, const PeerId& b) { return a == b; }

}  // namespace

Raft::Raft(Config cfg, RaftLog& log) : cfg_(std::move(cfg)), log_(log) {
    term_ = log_.current_term().value;
    voted_for_ = log_.voted_for();
    applied_ = log_.committed().value;
    // 选举超时错峰：真实 Raft 用 [T,2T] 随机值，这里用 id 派生的确定性偏移。
    // 目的相同（避免所有节点同拍竞选导致无限分裂），但可复现——模拟器的前提。
    const std::uint64_t base = cfg_.election_timeout == 0 ? 1 : cfg_.election_timeout;
    election_timeout_ = base + (cfg_.id.value * 3) % base;
}

bool Raft::is_voter(PeerId p) const {
    return std::any_of(cfg_.voters.begin(), cfg_.voters.end(),
                       [&](const PeerId& v) { return same(v, p); });
}

raftpb::Message Raft::make(raftpb::MessageId id, PeerId to) const {
    raftpb::Message msg;
    msg.set_id(id);
    msg.set_term(term_);
    msg.set_from(cfg_.id.value);
    msg.set_to(to.value);
    return msg;
}

void Raft::become_follower(std::uint64_t term, std::optional<PeerId> vote) {
    if (term > term_) {
        term_ = term;
        voted_for_ = vote;
        (void)log_.set_hard_state(Term{term_}, voted_for_);   // 先持久化 term/vote，再对外发消息
    }
    role_ = Role::Follower;
    leader_ = 0;
    election_elapsed_ = 0;
    votes_.clear();
}

void Raft::campaign() {
    role_ = Role::Candidate;
    ++term_;
    voted_for_ = cfg_.id;
    leader_ = 0;
    election_elapsed_ = 0;
    votes_.assign(1, cfg_.id);
    (void)log_.set_hard_state(Term{term_}, voted_for_);

    const auto last = log_.last_index();
    const auto last_term = log_.term(last);
    for (const PeerId& p : cfg_.voters) {
        if (same(p, cfg_.id)) {
            continue;
        }
        auto msg = make(raftpb::MSG_REQUEST_VOTE, p);
        msg.set_index(last.value);
        msg.set_log_term(last_term ? last_term->value : 0);
        outbox_.push_back(std::move(msg));
    }
    // 单副本集群：自己一票即过多数派，不必等外部回执（否则永远选不出 leader）
    if (votes_.size() >= majority()) {
        become_leader();
    }
}

void Raft::become_leader() {
    role_ = Role::Leader;
    leader_ = cfg_.id.value;
    heartbeat_elapsed_ = 0;
    const auto last = log_.last_index().value;
    for (const PeerId& p : cfg_.voters) {
        next_index_[p.value] = last + 1;
        match_index_[p.value] = same(p, cfg_.id) ? last : 0;
    }
    broadcast_append();
}

void Raft::tick() {
    ++now_ticks_;   // 单调 tick 计数：租约用 tick 而非墙钟，保证模拟可复现
    if (role_ == Role::Leader) {
        if (++heartbeat_elapsed_ >= cfg_.heartbeat_interval) {
            heartbeat_elapsed_ = 0;
            broadcast_heartbeat();
        }
        return;
    }
    if (++election_elapsed_ >= election_timeout_) {
        campaign();
    }
}

result<void> Raft::step(const raftpb::Message& msg) {
    if (msg.term() > term_) {
        become_follower(msg.term(), std::nullopt);
    }
    switch (msg.id()) {
        case raftpb::MSG_HUP:
            campaign();
            return {};
        case raftpb::MSG_APPEND:
            return handle_append(msg);
        case raftpb::MSG_APPEND_RESPONSE:
            handle_append_response(msg);
            return {};
        case raftpb::MSG_REQUEST_VOTE:
            handle_vote_request(msg);
            return {};
        case raftpb::MSG_REQUEST_VOTE_RESPONSE:
            handle_vote_response(msg);
            return {};
        case raftpb::MSG_HEARTBEAT:
            handle_heartbeat(msg);
            return {};
        case raftpb::MSG_HEARTBEAT_RESPONSE:
            handle_heartbeat_response(msg);
            return {};
        case raftpb::MSG_BEAT:
        case raftpb::MSG_PROPOSE:
        case raftpb::MSG_SNAPSHOT:
            return {};
        default:
            break;   // proto 枚举带哨兵值，必须显式兜底
    }
    return std::unexpected(InvalidArgument{"未知消息类型"});
}

result<void> Raft::propose(std::string data) {
    if (role_ != Role::Leader) {
        return std::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    append_entry_and_replicate(data);
    return {};
}

void Raft::append_entry_and_replicate(const std::string& data) {
    Entry entry;
    entry.set_index(log_.last_index().value + 1);
    entry.set_term(term_);
    entry.set_data(data);
    (void)log_.append(std::span<const Entry>(&entry, 1));
    broadcast_append();
    // 单副本集群没有 follower 回执，commit 只能靠自己推进；多副本时此调用不影响
    // （match 值仍为 0，多数派位置不会前移）。
    advance_commit();
}

void Raft::send_append(PeerId to) {
    auto it = next_index_.find(to.value);
    const std::uint64_t next = it == next_index_.end() ? log_.last_index().value + 1 : it->second;
    const std::uint64_t prev = next > 0 ? next - 1 : 0;

    auto msg = make(raftpb::MSG_APPEND, to);
    msg.set_index(prev);
    auto prev_term = log_.term(LogIndex{prev});
    msg.set_log_term(prev_term ? prev_term->value : 0);
    msg.set_commit(log_.committed().value);

    const auto last = log_.last_index().value;
    if (next <= last) {
        auto entries = log_.slice(LogIndex{next}, LogIndex{last + 1});
        if (entries) {
            for (const Entry& e : *entries) {
                *msg.add_entries() = e;
            }
        }
    }
    outbox_.push_back(std::move(msg));
}

void Raft::broadcast_append() {
    for (const PeerId& p : cfg_.voters) {
        if (!same(p, cfg_.id)) {
            send_append(p);
        }
    }
}

void Raft::broadcast_heartbeat() {
    // 红线 6：心跳只带 index/commit 元数据，绝不携带日志（PERF-08 的直接约束点）
    for (const PeerId& p : cfg_.voters) {
        if (same(p, cfg_.id)) {
            continue;
        }
        // 落后于 last_index 的 follower 用 AppendEntries 补发增量；追平后才退化为纯元数据心跳。
        // 否则"分区恢复后 leader 只发心跳、落后节点永远追不上"——丢包恢复就卡在这里。
        const auto it = next_index_.find(p.value);
        if (it != next_index_.end() && it->second <= log_.last_index().value) {
            send_append(p);
            continue;
        }
        auto msg = make(raftpb::MSG_HEARTBEAT, p);
        msg.set_index(log_.last_index().value);
        msg.set_commit(log_.committed().value);
        outbox_.push_back(std::move(msg));
    }
}

void Raft::collect_apply() {
    const auto committed = log_.committed().value;
    for (std::uint64_t i = applied_ + 1; i <= committed; ++i) {
        auto entry = log_.at(LogIndex{i});
        if (entry) {
            to_apply_.push_back(std::move(*entry));
        }
    }
    applied_ = committed;
}

Ready Raft::take_ready() {
    Ready ready;
    ready.messages = std::move(outbox_);
    ready.to_apply = std::move(to_apply_);
    ready.confirmed_reads = std::move(confirmed_reads_);
    outbox_.clear();
    to_apply_.clear();
    confirmed_reads_.clear();
    return ready;
}

result<std::uint64_t> Raft::read_index() {
    if (role_ != Role::Leader) {
        return std::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    const std::uint64_t ctx = ++read_ctx_seq_;
    // read_index 必须取"发起时已提交位点"：这是 ReadIndex 的核心——只要多数派确认过
    // 这个位点，后来者就不可能在不含该位点的日志上当选（选举安全）。
    pending_reads_[ctx] = PendingRead{.index = log_.committed().value, .acks = 1};

    if (pending_reads_[ctx].acks >= majority()) {
        confirmed_reads_.push_back(log_.committed().value);
        pending_reads_.erase(ctx);
        return ctx;
    }
    // 广播一轮带轮次标识的心跳（仍不带日志，PERF-08 约束保持）
    for (const PeerId& p : cfg_.voters) {
        if (same(p, cfg_.id)) {
            continue;
        }
        auto msg = make(raftpb::MSG_HEARTBEAT, p);
        msg.set_index(log_.last_index().value);
        msg.set_commit(log_.committed().value);
        msg.set_read_ctx(ctx);
        outbox_.push_back(std::move(msg));
    }
    return ctx;
}

void Raft::handle_vote_request(const raftpb::Message& msg) {
    const PeerId from{msg.from()};
    bool reject = false;
    if (msg.term() < term_) {
        reject = true;
    } else {
        if (voted_for_.has_value() && !same(*voted_for_, from)) {
            reject = true;
        } else {
            const auto my_last = log_.last_index();
            const auto my_term = log_.term(my_last);
            const std::uint64_t my_term_v = my_term ? my_term->value : 0;
            const bool up_to_date = msg.log_term() > my_term_v ||
                                    (msg.log_term() == my_term_v && msg.index() >= my_last.value);
            if (!up_to_date) {
                reject = true;
            } else {
                voted_for_ = from;
                (void)log_.set_hard_state(Term{term_}, voted_for_);
                election_elapsed_ = 0;   // 只在**同意**时重置；拒绝不重置（红线 1）
            }
        }
    }
    auto reply = make(raftpb::MSG_REQUEST_VOTE_RESPONSE, from);
    reply.set_reject(reject);
    outbox_.push_back(std::move(reply));
}

void Raft::handle_vote_response(const raftpb::Message& msg) {
    if (role_ != Role::Candidate || msg.term() != term_ || msg.reject()) {
        return;
    }
    const PeerId from{msg.from()};
    if (std::none_of(votes_.begin(), votes_.end(), [&](const PeerId& v) { return same(v, from); })) {
        votes_.push_back(from);
    }
    if (votes_.size() >= majority()) {
        become_leader();
    }
}

void Raft::reject_append(PeerId to, std::uint64_t index, std::uint64_t log_term) {
    auto reply = make(raftpb::MSG_APPEND_RESPONSE, to);
    reply.set_reject(true);
    reply.set_index(index);
    reply.set_log_term(log_term);
    outbox_.push_back(std::move(reply));
}

void Raft::reply_append_ok(PeerId to) {
    auto reply = make(raftpb::MSG_APPEND_RESPONSE, to);
    reply.set_index(log_.last_index().value);
    reply.set_commit(log_.committed().value);
    outbox_.push_back(std::move(reply));
}

// 逐条比对：同 term 跳过；不同 term 从此处截断后再追加（防旧消息误截断）
result<void> Raft::merge_entries(const raftpb::Message& msg) {
    for (int i = 0; i < msg.entries_size(); ++i) {
        const Entry& e = msg.entries(i);
        if (e.index() > log_.last_index().value) {
            break;
        }
        auto existing = log_.term(LogIndex{e.index()});
        if (existing && existing->value != e.term()) {
            (void)log_.truncate_from(LogIndex{e.index()});
            break;
        }
    }
    std::vector<Entry> to_append;
    for (int i = 0; i < msg.entries_size(); ++i) {
        if (msg.entries(i).index() > log_.last_index().value) {
            to_append.push_back(msg.entries(i));
        }
    }
    if (to_append.empty()) {
        return {};
    }
    return log_.append(to_append);
}

result<void> Raft::handle_append(const raftpb::Message& msg) {
    const PeerId from{msg.from()};
    if (msg.term() < term_) {
        reject_append(from, log_.last_index().value + 1, 0);
        return {};
    }
    role_ = Role::Follower;
    leader_ = msg.from();
    election_elapsed_ = 0;

    const std::uint64_t prev = msg.index();
    const auto first = log_.first_index().value;
    const auto last = log_.last_index().value;

    // 红线 3：prev 已被压缩（快照覆盖）→ 回复 committed，而不是 reject
    if (prev + 1 < first) {
        auto reply = make(raftpb::MSG_APPEND_RESPONSE, from);
        reply.set_index(log_.committed().value);
        reply.set_commit(log_.committed().value);
        outbox_.push_back(std::move(reply));
        return {};
    }
    if (prev > last) {
        reject_append(from, last + 1, 0);
        return {};
    }
    if (prev >= first) {
        auto prev_term = log_.term(LogIndex{prev});
        if (prev_term && prev_term->value != msg.log_term()) {
            // 红线 2：拒绝时带回冲突位置与该位置任期，供 leader 整段回退
            reject_append(from, prev, prev_term->value);
            return {};
        }
    }

    if (auto r = merge_entries(msg); !r) {
        return r;
    }

    const std::uint64_t new_commit = std::min(msg.commit(), log_.last_index().value);
    if (new_commit > log_.committed().value) {
        if (auto r = log_.set_committed(LogIndex{new_commit}); !r) {
            return r;
        }
        collect_apply();
    }

    reply_append_ok(from);
    return {};
}

void Raft::handle_append_response(const raftpb::Message& msg) {
    if (role_ != Role::Leader || msg.term() != term_) {
        return;
    }
    const PeerId from{msg.from()};
    if (!is_voter(from)) {
        return;
    }
    if (msg.reject()) {
        // 红线 2：按冲突 term 整段回退——跳到本地该 term 的最后一条之后，而不是逐条退一格
        std::uint64_t next = 0;
        if (msg.log_term() > 0) {
            for (std::uint64_t i = log_.last_index().value;; --i) {
                auto t = log_.term(LogIndex{i});
                if (t && t->value == msg.log_term()) {
                    next = i + 1;
                    break;
                }
                if (i <= log_.first_index().value) {
                    break;
                }
            }
        }
        if (next == 0) {
            next = std::max<std::uint64_t>(msg.index(), log_.first_index().value);
        }
        next_index_[from.value] = next;
        send_append(from);
        return;
    }
    match_index_[from.value] = msg.index();
    next_index_[from.value] = msg.index() + 1;
    advance_commit();
}

void Raft::handle_heartbeat_response(const raftpb::Message& msg) {
    if (msg.term() != term_) {
        return;
    }
    // 租约刷新：收到任期内的心跳回执即认为该 follower 仍认自己为 leader
    lease_deadline_ = now_ticks_ + (election_timeout_ * 7) / 10;

    const std::uint64_t ctx = msg.read_ctx();
    if (ctx == 0) {
        return;   // 普通心跳回执：只刷新租约
    }
    const auto it = pending_reads_.find(ctx);
    if (it == pending_reads_.end()) {
        return;   // 已确认或已失效
    }
    if (msg.index() < it->second.index) {
        return;   // follower 日志还没到这个位点，不能计入 quorum
    }
    if (++it->second.acks >= majority()) {
        confirmed_reads_.push_back(it->second.index);
        pending_reads_.erase(it);
    }
}

void Raft::handle_heartbeat(const raftpb::Message& msg) {
    const PeerId from{msg.from()};
    if (msg.term() < term_) {
        auto reply = make(raftpb::MSG_HEARTBEAT_RESPONSE, from);
        reply.set_index(log_.last_index().value);
        outbox_.push_back(std::move(reply));
        return;
    }
    role_ = Role::Follower;
    leader_ = msg.from();
    election_elapsed_ = 0;

    const std::uint64_t new_commit = std::min(msg.commit(), log_.last_index().value);
    if (new_commit > log_.committed().value) {
        (void)log_.set_committed(LogIndex{new_commit});
        collect_apply();
    }
    auto reply = make(raftpb::MSG_HEARTBEAT_RESPONSE, from);
    reply.set_index(log_.last_index().value);
    reply.set_commit(log_.committed().value);
    reply.set_read_ctx(msg.read_ctx());   // 原样带回，leader 据此做 quorum 确认
    outbox_.push_back(std::move(reply));
}

void Raft::advance_commit() {
    // 红线 4：只统计**当前 term** 被多数派复制的 index；旧 term 条目不因多数派复制而推进
    std::vector<std::uint64_t> matches;
    matches.reserve(cfg_.voters.size());
    for (const PeerId& p : cfg_.voters) {
        matches.push_back(same(p, cfg_.id) ? log_.last_index().value : match_index_[p.value]);
    }
    std::sort(matches.begin(), matches.end(), std::greater<>());
    const std::uint64_t candidate = matches[majority() - 1];
    if (candidate <= log_.committed().value) {
        return;
    }
    auto t = log_.term(LogIndex{candidate});
    if (!t || t->value != term_) {
        return;
    }
    (void)log_.set_committed(LogIndex{candidate});
    collect_apply();
}

}  // namespace tinykv::raft
