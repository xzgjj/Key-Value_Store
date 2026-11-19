// 文件用途：raft.h 的实现——选举、复制、commit 推进与四条红线分支。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03/04）。

#include "raft/raft.h"
#include "common/compat.h"
#include "common/error_format.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tinykv::raft {
namespace {

bool same(const PeerId& a, const PeerId& b) {
    return a == b;
}

// M4 ④b / D3-5：learner 追平门槛。日志差 <100 entry 且该状态持续 5s。
// 模拟器按 100ms/tick 约定 → 5s = 50 tick；用 tick 而非墙钟保证可复现。
constexpr std::uint64_t kCatchUpLagLimit = 100;
constexpr std::uint64_t kCatchUpTicks = 50;
// 红线 5：两节点集群移除 leader 自己 → 剩余单节点无法形成多数派，写会永久卡死。
constexpr std::size_t kRemoveLeaderMinVoters = 2;
// 在途 ReadIndex 上限：分区期间 quorum 永远不齐，不设限会让 map 无界增长
constexpr std::size_t kMaxPendingReads = 1024;

}  // namespace

Raft::Raft(Config cfg, RaftLog& log) : cfg_(std::move(cfg)), log_(log) {
    // code-review：配置非法属于装配错误，早失败好过运行期越界（advance_commit 的 matches[majority-1]）。
    // voters 为空是合法的"纯 learner"配置（只收日志、不投票）：campaign 会直接拒绝，
    // advance_commit 另有显式空表守卫。心跳必须快于选举超时，否则 leader 保不住租约。
    if (cfg_.election_timeout == 0 ||
        cfg_.heartbeat_interval == 0 ||
        cfg_.heartbeat_interval >= cfg_.election_timeout) {
        throw std::invalid_argument(
            "Raft::Config 要求 0 < heartbeat_interval < election_timeout");
    }
    term_ = log_.current_term().value;
    voted_for_ = log_.voted_for();
    // 两个位点语义不同：applied_ 是"已发进 Ready"的内存 dispatch 游标，
    // RaftLog 里的 applied 是"状态机确实写完"的持久化游标。
    // 崩溃窗口内后者会落后；启动时从持久化位点重建，并把缺口灌入首个 Ready 重放。
    applied_ = std::min(log_.applied().value, log_.committed().value);
    if (auto r = collect_apply(); !r) {
        // 构造不能返回错误：进入粘性失败态，Peer::open 构造后检查并上抛
        fail("恢复待 apply 条目失败: " + std::string(describe_kind(r.error())));
    }
    // 选举超时错峰：真实 Raft 用 [T,2T] 随机值，这里用 id 派生的确定性偏移。
    // 目的相同（避免所有节点同拍竞选导致无限分裂），但可复现——模拟器的前提。
    const std::uint64_t base = cfg_.election_timeout == 0 ? 1 : cfg_.election_timeout;
    election_timeout_ = base + (cfg_.id.value * 3) % base;
}

bool Raft::is_voter(PeerId p) const {
    return std::any_of(cfg_.voters.begin(), cfg_.voters.end(),
                       [&](const PeerId& v) { return same(v, p); });
}

bool Raft::is_learner(PeerId p) const {
    return std::find(learners_.begin(), learners_.end(), p) != learners_.end();
}

bool Raft::is_learner_caught_up(PeerId p) const {
    if (!is_learner(p)) {
        return false;
    }
    // match_index_ 缺失视为 0（从未复制过任何日志）。leader 自己不在 learners_ 中。
    const auto it = match_index_.find(p.value);
    const std::uint64_t matched = it == match_index_.end() ? 0 : it->second;
    const std::uint64_t lag =
        log_.last_index().value > matched ? log_.last_index().value - matched : 0;
    if (lag >= kCatchUpLagLimit) {
        return false;  // 日志差过大，未达标
    }
    const auto since = learner_catch_since_.find(p.value);
    if (since == learner_catch_since_.end()) {
        return false;  // 差虽小，但还没连续保持 5s
    }
    return now_ticks_ >= since->second && now_ticks_ - since->second >= kCatchUpTicks;
}

raftpb::Message Raft::make(raftpb::MessageId id, PeerId to) const {
    raftpb::Message msg;
    msg.set_id(id);
    msg.set_term(term_);
    msg.set_from(cfg_.id.value);
    msg.set_to(to.value);
    return msg;
}

void Raft::fail(std::string_view why) {
    if (!failed_) {
        failed_ = true;
        fail_reason_ = std::string(why);
    }
}

result<void> Raft::check_failed() const {
    if (!failed_) {
        return {};
    }
    return tinykv::unexpected(
        IoFailed{.path = "raft", .reason = "状态机已进入失败态: " + fail_reason_});
}

void Raft::become_follower(std::uint64_t term, std::optional<PeerId> vote) {
    if (term > term_) {
        // P0-2：先持久化 term/vote，成功后才推进内存。落盘失败时宁可停下，
        // 也不能让"内存已投过票、盘上没记"的状态对外发消息。
        if (auto r = log_.set_hard_state(Term{term}, vote); !r) {
            fail((std::string("become_follower 持久化 term/vote 失败: ") + std::string(describe_kind(r.error()))));
            return;
        }
        term_ = term;
        voted_for_ = vote;
    }
    role_ = Role::Follower;
    leader_ = 0;
    election_elapsed_ = 0;
    votes_.clear();
    // 降级即在途 ReadIndex 全部作废：不清理会让 pending_reads_ 在 leader 反复切换时无界增长
    pending_reads_.clear();
    clear_transfer();  // 已降级：转移意图作废（目标自己赢了或别人上台都算转移结束）
}

void Raft::campaign() {
    if (cfg_.voters.empty()) {
        return;  // 纯 learner（无 voters）：永不自发竞选，只收日志
    }
    const Term next{term_ + 1};
    // P0-2：任期与投票先落盘，再竞选（否则重启可能在同一任期投两次票）
    if (auto r = log_.set_hard_state(next, cfg_.id); !r) {
        fail((std::string("campaign 持久化 hard state 失败: ") + std::string(describe_kind(r.error()))));
        return;
    }
    term_ = next.value;
    voted_for_ = cfg_.id;
    role_ = Role::Candidate;
    leader_ = 0;
    election_elapsed_ = 0;
    votes_.assign(1, cfg_.id);
    pending_reads_.clear();  // 新一轮任期：旧读请求的 quorum 语义已失效

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
    clear_transfer();  // 自己成了 leader：任何在途转移意图都已由本次选举决出结果
    const auto last = log_.last_index().value;
    for (const PeerId& p : cfg_.voters) {
        next_index_[p.value] = last + 1;
        match_index_[p.value] = same(p, cfg_.id) ? last : 0;
    }
    broadcast_append();
}

void Raft::refresh_learner_catchup() {
    // 追平计时只由 leader 维护：它掌握全体 match_index_。每拍刷新一次，
    // 让"持续 5s"用 now_ticks_ 度量而非墙钟（模拟器种子可复现）。
    for (const PeerId& p : learners_) {
        const auto it = match_index_.find(p.value);
        const std::uint64_t matched = it == match_index_.end() ? 0 : it->second;
        const std::uint64_t lag =
            log_.last_index().value > matched ? log_.last_index().value - matched : 0;
        if (lag >= kCatchUpLagLimit) {
            learner_catch_since_.erase(p.value);  // 掉队：重新计时
        } else {
            learner_catch_since_.try_emplace(p.value, now_ticks_);  // 首次达标记起点
        }
    }
    // 已被移除的 learner 不留残项（apply_conf_change 摘除后同步清理）
    for (auto it = learner_catch_since_.begin(); it != learner_catch_since_.end();) {
        if (!is_learner(PeerId{it->first})) {
            it = learner_catch_since_.erase(it);
        } else {
            ++it;
        }
    }
}

void Raft::clear_transfer() {
    transfer_target_.reset();
    transfer_deadline_ = 0;
    transfer_notified_ = false;
}

void Raft::maybe_send_timeout_now() {
    if (!transfer_target_.has_value() || transfer_notified_) {
        return;
    }
    // 目标必须先追平：日志落后的节点去竞选会被"日志新旧比较"拒掉，白抬一次 term。
    const auto it = match_index_.find(transfer_target_->value);
    const std::uint64_t matched = it == match_index_.end() ? 0 : it->second;
    if (matched < log_.last_index().value) {
        send_append(*transfer_target_);  // 先把它补上，回执到达时再送 TimeoutNow
        return;
    }
    outbox_.push_back(make(raftpb::MSG_TIMEOUT_NOW, *transfer_target_));
    transfer_notified_ = true;
}

result<void> Raft::transfer_leader(PeerId target) {
    if (role_ != Role::Leader) {
        return tinykv::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    if (same(target, cfg_.id)) {
        return tinykv::unexpected(InvalidArgument{"转移目标就是自己"});
    }
    if (!is_voter(target)) {
        // learner 不投票也不该当 leader：让它竞选会抬 term 但永远凑不够多数派
        return tinykv::unexpected(InvalidArgument{"转移目标不是 voter"});
    }
    transfer_target_ = target;
    transfer_deadline_ = now_ticks_ + cfg_.election_timeout;  // 红线6：只给一个 election timeout
    transfer_notified_ = false;
    maybe_send_timeout_now();
    return {};
}

void Raft::handle_timeout_now(const raftpb::Message& msg) {
    (void)msg;
    if (!is_voter(cfg_.id)) {
        return;  // learner 收到也不竞选（只收日志）
    }
    if (role_ == Role::Leader) {
        return;  // 已经是 leader：没有可接的对象
    }
    campaign();  // 不等 election timeout，立即竞选（带 term+1，原 leader 会投票并降级）
}

void Raft::tick() {
    ++now_ticks_;  // 单调 tick 计数：租约/追平判定用 tick 而非墙钟，保证模拟可复现
    if (failed_) {
        return;  // 持久化链路已不可信：不再竞选/发消息，等调用方停服
    }
    if (role_ == Role::Leader) {
        refresh_learner_catchup();
        // 红线6：一个 election timeout 内没完成即放弃（意图易失，不落日志、不通知客户端）
        if (transfer_target_.has_value() && now_ticks_ >= transfer_deadline_) {
            clear_transfer();
        }
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
    if (auto ok = check_failed(); !ok) {
        return ok;
    }
    if (msg.term() > term_) {
        become_follower(msg.term(), std::nullopt);
        // 持久化新 term 失败：本消息不再处理，错误上抛给 Peer
        if (auto ok = check_failed(); !ok) {
            return ok;
        }
    }
    if (auto r = dispatch(msg); !r) {
        return r;
    }
    // 分支内部的持久化失败统一在这里上抛（不把错误吞成"消息处理成功"）
    return check_failed();
}

result<void> Raft::dispatch(const raftpb::Message& msg) {
    switch (msg.id()) {
        case raftpb::MSG_HUP:
            campaign();
            break;
        case raftpb::MSG_APPEND:
            if (auto r = handle_append(msg); !r) {
                return r;
            }
            break;
        case raftpb::MSG_APPEND_RESPONSE:
            handle_append_response(msg);
            break;
        case raftpb::MSG_REQUEST_VOTE:
            handle_vote_request(msg);
            break;
        case raftpb::MSG_REQUEST_VOTE_RESPONSE:
            handle_vote_response(msg);
            break;
        case raftpb::MSG_HEARTBEAT:
            handle_heartbeat(msg);
            break;
        case raftpb::MSG_HEARTBEAT_RESPONSE:
            handle_heartbeat_response(msg);
            break;
        case raftpb::MSG_TIMEOUT_NOW:
            handle_timeout_now(msg);
            break;
        case raftpb::MSG_BEAT:
        case raftpb::MSG_PROPOSE:
        case raftpb::MSG_SNAPSHOT:
            break;
        default:
            // proto 枚举带哨兵值，必须显式兜底
            return tinykv::unexpected(InvalidArgument{"未知消息类型"});
    }
    return {};
}

result<void> Raft::propose(std::string data) {
    if (auto ok = check_failed(); !ok) {
        return ok;
    }
    if (role_ != Role::Leader) {
        return tinykv::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    if (transfer_target_.has_value()) {
        // 红线6：转移期拒新提案。若放行，目标可能带着"更多未提交条目"去竞选，
        // 转移就变成一次状态不确定的冒险；宁可让调用方重试。
        return tinykv::unexpected(InvalidArgument{"leader 转移中，暂不接受新提案"});
    }
    return append_entry_and_replicate(data);
}

result<void> Raft::propose_conf_change(const raftpb::ConfChange& cc) {
    if (auto ok = check_failed(); !ok) {
        return ok;
    }
    if (role_ != Role::Leader) {
        return tinykv::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    if (transfer_target_.has_value()) {
        return tinykv::unexpected(InvalidArgument{"leader 转移中，暂不接受成员变更"});
    }
    const PeerId target{cc.node_id()};
    // 红线 5 / D3-5 前置守卫（在落盘前拒绝，避免把注定卡死的变更写进日志）：
    if (cc.type() == raftpb::ConfAddVoter) {
        // AddVoter 门槛：目标须是 learner 且已追平（日志差 <100 且持续 5s）。
        // 已经是 voter 的重复提案放行（幂等，apply_conf_change 本就去重）。
        if (is_learner(target) && !is_learner_caught_up(target)) {
            return tinykv::unexpected(InvalidArgument{"learner 未追平，拒绝 AddVoter"});
        }
    } else if (cc.type() == raftpb::ConfRemoveNode) {
        // 两节点集群移除 leader 自己 → 剩余单节点无法形成多数派，写永久卡死。
        // TransferLeader 按既定拆到第⑦层；本层只守卫拒绝，不改 leader。
        if (same(target, cfg_.id) && cfg_.voters.size() <= kRemoveLeaderMinVoters) {
            return tinykv::unexpected(
                InvalidArgument{"两节点集群不可移除 leader 自己（需先 TransferLeader）"});
        }
    }
    // 成员变更作为 EntryConfChange 落盘：apply 时才生效，崩溃后可从日志重建
    std::string data;
    if (!cc.SerializeToString(&data)) {
        return tinykv::unexpected(Corrupted{.where = "raft:confchange", .reason = "序列化失败"});
    }
    Entry entry;
    entry.set_index(log_.last_index().value + 1);
    entry.set_term(term_);
    entry.set_data(std::move(data));
    entry.set_type(raftpb::EntryConfChange);
    if (auto r = log_.append(tinykv::span<const Entry>(&entry, 1)); !r) {
        fail("成员变更落盘失败: " + std::string(describe_kind(r.error())));
        return r;  // 未落盘的成员变更不得对外表现为"已提案"
    }
    broadcast_append();
    advance_commit();  // 单副本时靠自己推进；多副本由回执推进（无副作用）
    return {};
}

bool Raft::can_compact(LogIndex upto) const {
    if (role_ != Role::Leader) {
        return false;
    }
    for (const PeerId& p : cfg_.voters) {
        std::uint64_t matched = 0;
        if (same(p, cfg_.id)) {
            matched = log_.last_index().value;
        } else {
            const auto it = match_index_.find(p.value);
            if (it != match_index_.end()) {
                matched = it->second;
            }
        }
        if (matched < upto.value) {
            return false;
        }
    }
    return true;
}

void Raft::apply_conf_change(const raftpb::ConfChange& cc) {
    const auto pid = PeerId{cc.node_id()};
    auto erase_peer = [](std::vector<PeerId>& v, PeerId p) {
        v.erase(std::remove(v.begin(), v.end(), p), v.end());
    };
    switch (cc.type()) {
        case raftpb::ConfAddLearner:
            if (!is_voter(pid) && !is_learner(pid)) {
                learners_.push_back(pid);
                // 初始化复制指针，leader 才能开始向 learner 追日志（否则 match 恒为 0，永远追不平）
                next_index_[pid.value] = log_.last_index().value + 1;
                match_index_[pid.value] = 0;
            }
            break;
        case raftpb::ConfAddVoter:
            erase_peer(learners_, pid);  // learner → voter
            if (std::find(cfg_.voters.begin(), cfg_.voters.end(), pid) == cfg_.voters.end()) {
                cfg_.voters.push_back(pid);
                next_index_[pid.value] = log_.last_index().value + 1;
                match_index_[pid.value] = 0;
            }
            break;
        case raftpb::ConfRemoveNode:
            erase_peer(learners_, pid);
            erase_peer(cfg_.voters, pid);
            next_index_.erase(pid.value);
            match_index_.erase(pid.value);
            break;
        default:
            break;
    }
}

result<void> Raft::append_entry_and_replicate(const std::string& data) {
    Entry entry;
    entry.set_index(log_.last_index().value + 1);
    entry.set_term(term_);
    entry.set_data(data);
    // type 默认 EntryNormal（0），无需显式设置——旧 data/快照零改动兼容
    if (auto r = log_.append(tinykv::span<const Entry>(&entry, 1)); !r) {
        // P0-2：追加失败 = 日志链路不可信，进入粘性失败态并上抛，调用方不能以为"已提案"
        fail("提案落盘失败: " + std::string(describe_kind(r.error())));
        return r;
    }
    broadcast_append();
    // 单副本集群没有 follower 回执，commit 只能靠自己推进；多副本时此调用不影响
    // （match 值仍为 0，多数派位置不会前移）。
    advance_commit();
    return check_failed();
}

void Raft::send_append(PeerId to) {
    auto it = next_index_.find(to.value);
    const std::uint64_t next = it == next_index_.end() ? log_.last_index().value + 1 : it->second;
    const std::uint64_t prev = next > 0 ? next - 1 : 0;

    // leader 主动发现"该 follower 需要的条目已被压缩"：这条路径不靠 reject 驱动——
    // next_index 可能还停在被压缩之前（如 follower 长时间不可达），此时 slice 会失败、
    // 发出去的是空 AppendEntries，follower 永远追不上。
    if (next < log_.first_index().value) {
        if (std::none_of(snapshot_targets_.begin(), snapshot_targets_.end(),
                         [&](const PeerId& p) { return same(p, to); })) {
            snapshot_targets_.push_back(to);
        }
        return;
    }

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
    // M4 ④b：learner 也要收日志才能追平。它不在 voters 中不参与多数派，
    // 但复制链路与 voter 共用 send_append（由 next_index_ 驱动）。
    for (const PeerId& p : learners_) {
        send_append(p);
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

result<void> Raft::collect_apply() {
    const auto committed = log_.committed().value;
    for (std::uint64_t i = applied_ + 1; i <= committed; ++i) {
        auto entry = log_.at(LogIndex{i});
        if (!entry) {
            // P0-3：已提交条目读不出来是存储损坏，绝不能跳过。
            // 旧实现跳过该条仍执行 applied_ = committed，等于永久丢一条写且无法补回。
            return tinykv::unexpected(
                Corrupted{.where = "raft:collect_apply",
                          .reason = "已提交条目 index " + std::to_string(i) + " 读取失败: " +
                                    std::string(describe_kind(entry.error()))});
        }
        // M4 第④层：成员变更在 apply 时才生效——先更新 membership，再交给上层 apply。
        // 幂等设计：崩溃恢复重放日志会再次走到这里，结果一致。
        if (entry->type() == raftpb::EntryConfChange) {
            raftpb::ConfChange cc;
            if (cc.ParseFromString(entry->data())) {
                apply_conf_change(cc);
            }
        }
        to_apply_.push_back(std::move(*entry));
    }
    applied_ = committed;  // 只有全部条目成功入队才推进游标
    return {};
}

result<void> Raft::mark_applied(LogIndex idx) {
    if (idx.value > log_.committed().value) {
        return tinykv::unexpected(
            InvalidArgument{"applied 不得超过 committed：" + std::to_string(idx.value)});
    }
    if (auto r = log_.set_applied(idx); !r) {
        return r;
    }
    applied_ = std::max(applied_, idx.value);
    return {};
}

Ready Raft::take_ready() {
    Ready ready;
    ready.messages = std::move(outbox_);
    ready.to_apply = std::move(to_apply_);
    ready.confirmed_reads = std::move(confirmed_reads_);
    ready.snapshot_targets = std::move(snapshot_targets_);
    outbox_.clear();
    to_apply_.clear();
    confirmed_reads_.clear();
    snapshot_targets_.clear();
    return ready;
}

result<std::uint64_t> Raft::read_index() {
    if (role_ != Role::Leader) {
        return tinykv::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    // 无界增长的第二道闸：即使 quorum 一直不齐（分区），在途读请求也不能无限堆积
    if (pending_reads_.size() >= kMaxPendingReads) {
        return tinykv::unexpected(
            InvalidArgument{"在途 ReadIndex 过多（leader 长时间失去 quorum？）"});
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
                // P0-2：投票记录必须先落盘。落盘失败时拒绝投票（保守），并进入失败态。
                if (auto r = log_.set_hard_state(Term{term_}, from); !r) {
                    fail((std::string("投票持久化失败: ") + std::string(describe_kind(r.error()))));
                    reject = true;
                } else {
                    voted_for_ = from;
                    election_elapsed_ = 0;  // 只在**同意**时重置；拒绝不重置（红线 1）
                }
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
    if (std::none_of(votes_.begin(), votes_.end(),
                     [&](const PeerId& v) { return same(v, from); })) {
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
            if (auto r = log_.truncate_from(LogIndex{e.index()}); !r) {
                return r;  // 截断未落盘：不得继续追加（否则日志出现分叉）
            }
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
        if (auto r = collect_apply(); !r) {
            return r;
        }
    }

    reply_append_ok(from);
    return {};
}

void Raft::on_match_advanced(PeerId from) {
    if (transfer_target_.has_value() && transfer_target_->value == from.value) {
        maybe_send_timeout_now();
    }
}

void Raft::handle_append_response(const raftpb::Message& msg) {
    if (role_ != Role::Leader || msg.term() != term_) {
        return;
    }
    const PeerId from{msg.from()};
    // M4 ④b：voter 与 learner 的复制回执都要处理——learner 靠它推进 match_index_ 追平。
    const bool from_voter = is_voter(from);
    if (!from_voter && !is_learner(from)) {
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
        if (next < log_.first_index().value) {
            // 需要的条目已被压缩：标记该 follower 走快照，别再逐条退格（退格永远退不到）
            snapshot_targets_.push_back(from);
            return;
        }
        next_index_[from.value] = next;
        send_append(from);
        return;
    }
    match_index_[from.value] = msg.index();
    next_index_[from.value] = msg.index() + 1;
    on_match_advanced(from);  // ⑦a：转移目标可能刚追平，补发 TimeoutNow
    // 只有 voter 的回执能推进 commit：learner 不计入多数派（红线：learner 不投票）。
    // 若让 learner 回执也调 advance_commit，region 会出现"少数派写入已提交"的严重错误。
    if (from_voter) {
        advance_commit();
    }
}

void Raft::handle_heartbeat_response(const raftpb::Message& msg) {
    if (msg.term() != term_) {
        return;
    }
    // 租约刷新：收到任期内的心跳回执即认为该 follower 仍认自己为 leader
    lease_deadline_ = now_ticks_ + (election_timeout_ * 7) / 10;

    const std::uint64_t ctx = msg.read_ctx();
    if (ctx == 0) {
        return;  // 普通心跳回执：只刷新租约
    }
    const auto it = pending_reads_.find(ctx);
    if (it == pending_reads_.end()) {
        return;  // 已确认或已失效
    }
    if (msg.index() < it->second.index) {
        return;  // follower 日志还没到这个位点，不能计入 quorum
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
        if (auto r = log_.set_committed(LogIndex{new_commit}); !r) {
            fail((std::string("心跳推进 commit 落盘失败: ") + std::string(describe_kind(r.error()))));
            return;  // 不回复 OK：leader 不能据此认为该副本已接受提交位点
        }
        if (auto r = collect_apply(); !r) {
            fail((std::string("心跳推进 apply 失败: ") + std::string(describe_kind(r.error()))));
            return;
        }
    }
    auto reply = make(raftpb::MSG_HEARTBEAT_RESPONSE, from);
    reply.set_index(log_.last_index().value);
    reply.set_commit(log_.committed().value);
    reply.set_read_ctx(msg.read_ctx());  // 原样带回，leader 据此做 quorum 确认
    outbox_.push_back(std::move(reply));
}

result<void> Raft::restore_snapshot(LogIndex index, Term term) {
    if (auto r = log_.reset_snapshot(index, term); !r) {
        return r;
    }
    applied_ = index.value;
    if (term.value > term_) {
        if (auto r = log_.set_hard_state(Term{term.value}, std::nullopt); !r) {
            fail((std::string("快照恢复 hard state 落盘失败: ") + std::string(describe_kind(r.error()))));
            return tinykv::unexpected(r.error());
        }
        term_ = term.value;
        voted_for_.reset();
    }
    role_ = Role::Follower;
    election_elapsed_ = 0;
    votes_.clear();
    leader_ = 0;
    pending_reads_.clear();
    return {};
}

void Raft::advance_commit() {
    // 红线 4：只统计**当前 term** 被多数派复制的 index；旧 term 条目不因多数派复制而推进
    std::vector<std::uint64_t> matches;
    matches.reserve(cfg_.voters.size());
    for (const PeerId& p : cfg_.voters) {
        matches.push_back(same(p, cfg_.id) ? log_.last_index().value : match_index_[p.value]);
    }
    if (matches.empty()) {
        // 构造期已校验 voters 非空；这里仍显式防御，绝不越界取 matches[majority()-1]
        fail("advance_commit: voters 为空");
        return;
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
    if (auto r = log_.set_committed(LogIndex{candidate}); !r) {
        fail((std::string("leader 推进 commit 落盘失败: ") + std::string(describe_kind(r.error()))));
        return;
    }
    if (auto r = collect_apply(); !r) {
        fail((std::string("leader 推进 apply 失败: ") + std::string(describe_kind(r.error()))));
    }
}

}  // namespace tinykv::raft
