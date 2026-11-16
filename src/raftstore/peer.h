// 文件用途：Peer——Raft 之上的状态机与提案入口：提案走共识、apply 落存储、按 client+seq 去重。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-04、UX-03）。
//
// 职责边界（与 SPEC-2 §2.2 对应）：
//   * 提案：把 RaftCmdRequest 序列化进日志条目（不是本地直接写库）；
//   * apply：只由 committed 条目驱动写存储——"写走共识"这条红线落在这一层；
//   * 去重：同 (client_id, seq) 的重试不得二次 apply，否则重试会变成重复写（删了又写回）。
//     M2 采用"每 client 记录已 apply 的最大 seq"，比缓存逐条回执省内存，且对顺序重试足够。
//   * 读：M2 为 leader 本地读（未做 ReadIndex，读可能略旧），M3 用 ReadIndex 收口——这是已知限制。

#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <raft_cmdpb.pb.h>

#include "common/error.h"
#include "raft/raft.h"
#include "storage/storage.h"

namespace tinykv::raftstore {

class Peer {
public:
    // cfg 的 voters 决定集群规模；storage 同时承载 raft log 与业务数据（M2 简化）
    [[nodiscard]] static result<std::unique_ptr<Peer>> open(const raft::Config& cfg, Storage& storage);

    // 提交一条 RaftCmdRequest；重复的 (client_id, seq) 直接返回成功而不产生新提案
    [[nodiscard]] result<void> propose(const cmdpb::RaftCmdRequest& req);

    // leader 本地读（M2 非线性一致，M3 由 ReadIndex 修正）
    [[nodiscard]] result<GetResult> get(Cf cf, std::string_view key) const;

    void tick();

    // 把本轮 Ready 里的已提交条目全部 apply；返回真正 apply 的条目数。
    // 出站消息不在此丢弃——收进 outbox 由 take_messages() 取走交给传输层。
    [[nodiscard]] result<std::size_t> on_ready();

    // 取走待发送消息（接网络前 on_ready 会把消息丢掉，那是 bug）
    [[nodiscard]] std::vector<raftpb::Message> take_messages();

    // 从传输层收到的消息交给状态机
    [[nodiscard]] result<void> step_message(const raftpb::Message& msg);

    // ---- M3 一致读（ReadIndex 必做 / LeaseRead 默认关） ----
    // 两阶段而非阻塞式：Peer 不持有传输层，不能自己泵消息；由调用方（Worker/测试驱动）
    // 在 begin 之后泵若干轮，再取 confirmed 位点，等 applied 追平后读状态机。
    [[nodiscard]] result<std::uint64_t> begin_read_index();

    // 取走已获 quorum 确认的读位点（可能为空）
    [[nodiscard]] std::vector<std::uint64_t> take_confirmed_reads();

    // 租约：默认关；开启且有效时可跳过 ReadIndex 直接读（零 RPC）
    void enable_lease(bool on);
    [[nodiscard]] bool lease_valid() const;

    [[nodiscard]] bool applied_at_least(std::uint64_t index) const { return applied_ >= index; }

    // 直接读状态机（调用方保证 applied ≥ read_index，否则读到的可能不是线性一致点）
    [[nodiscard]] result<GetResult> read_directly(Cf cf, std::string_view key) const;

    [[nodiscard]] bool is_leader() const { return raft_->is_leader(); }
    [[nodiscard]] raft::Raft& raft() { return *raft_; }
    [[nodiscard]] std::uint64_t applied_seq(std::uint64_t client_id) const;

private:
    Peer(std::unique_ptr<raft::RaftLog> log, Storage& storage, const raft::Config& cfg);

    [[nodiscard]] result<void> apply_entry(const raftpb::Entry& entry);
    [[nodiscard]] result<void> execute(const cmdpb::RaftCmdRequest& req);

    std::unique_ptr<raft::RaftLog> log_;
    std::unique_ptr<raft::Raft> raft_;
    Storage& storage_;
    std::map<std::uint64_t, std::uint64_t> last_seq_;   // client_id → 已 apply 的最大 seq
    std::vector<raftpb::Message> outbox_;
    std::vector<std::uint64_t> confirmed_reads_;
    std::uint64_t applied_{0};   // 已 apply 到的日志位点
};

}  // namespace tinykv::raftstore
