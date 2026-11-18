// 文件用途：Peer 端事务执行器（M6 第③层）——TxnExecutor 的经 raft 生产实现。
// 归属：TinyKV-CPP（M6 运行时；从 tests/testlib 上移：经 raft 执行是生产路径，不是测试装置）。
//
// 三个注入点让同一实现适配单机 daemon 与多副本测试：
//   peer()     —— 目标 Peer（多副本时动态找 leader；单机固定返回自己）
//   pump()     —— 推动共识前进（daemon=Worker 已在后台泵，这里只等；测试=主动 pump）
//   applied()  —— 收敛判定（单机=本 peer 到位；多副本=全部副本到位）
// 读路径（get/check_txn_status/locks_of）走 peer 本地 MVCC 直读（M2 leader 本地读同款偏差）。

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <kv.pb.h>

#include "common/error.h"
#include "raftstore/peer.h"
#include "txn/executor.h"

namespace tinykv::server {

class PeerTxnExecutor : public tinykv::txn::TxnExecutor {   // 非 final：测试适配器派生注入收敛策略
public:
    using PeerRef = std::function<tinykv::raftstore::Peer&(std::string_view key)>;
    using Pump = std::function<void(std::string_view key)>;
    using Applied = std::function<bool(std::string_view key, std::uint64_t index)>;
    using PeerList = std::function<std::vector<tinykv::raftstore::Peer*>()>;   // GC 广播用

    // gate：daemon 组合根注入的 Peer 串行闸（Worker 泵与 HTTP 共用；泵回调不得重复加锁）
    PeerTxnExecutor(PeerRef peer, Pump pump, Applied applied, std::mutex* gate,
                    int max_rounds = 400, PeerList peers = {})
        : peer_(std::move(peer)), pump_(std::move(pump)), applied_(std::move(applied)),
          gate_(gate), max_rounds_(max_rounds), peers_(std::move(peers)) {}

    [[nodiscard]] tinykv::result<std::optional<std::string>> get(
        std::string_view key, tinykv::Ts start_ts) override {
        std::optional<std::unique_lock<std::mutex>> gate = hold_gate();
        return peer_(key).mvcc().get(key, start_ts);
    }

    [[nodiscard]] tinykv::result<void> prewrite(std::span<const tinykv::txn::Mutation> mutations,
                                                std::string_view primary, tinykv::Ts start_ts,
                                                std::uint64_t ttl_ms,
                                                std::uint64_t /*now_ms*/) override {
        kvpb::TxnPrewriteRequest req;
        req.set_primary_lock(std::string(primary));
        req.set_start_ts(start_ts.value);
        req.set_lock_ttl(ttl_ms);
        for (const auto& m : mutations) {
            auto* one = req.add_mutations();
            one->set_op(m.op == tinykv::txn::Mutation::Op::kPut ? kvpb::Mutation::PUT
                                                               : kvpb::Mutation::DELETE);
            one->set_key(m.key);
            one->set_value(m.value);
        }
        return call_through_raft(primary, [&](tinykv::raftstore::Peer& p) {
            return p.propose_txn_prewrite(req);
        });
    }

    [[nodiscard]] tinykv::result<void> commit(std::span<const std::string> keys,
                                              tinykv::Ts start_ts, tinykv::Ts commit_ts) override {
        kvpb::TxnCommitRequest req;
        req.set_start_ts(start_ts.value);
        req.set_commit_ts(commit_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft(keys.empty() ? std::string_view{} : keys.front(),
                                 [&](tinykv::raftstore::Peer& p) { return p.propose_txn_commit(req); });
    }

    [[nodiscard]] tinykv::result<void> rollback(std::span<const std::string> keys,
                                                tinykv::Ts start_ts) override {
        kvpb::TxnBatchRollbackRequest req;
        req.set_start_ts(start_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft(keys.empty() ? std::string_view{} : keys.front(),
                                 [&](tinykv::raftstore::Peer& p) { return p.propose_txn_rollback(req); });
    }

    [[nodiscard]] tinykv::result<tinykv::txn::TxnStatus> check_txn_status(
        std::string_view primary, tinykv::Ts lock_start_ts, std::uint64_t now_ms) override {
        auto gate = hold_gate();
        return peer_(primary).mvcc().check_txn_status(primary, lock_start_ts, now_ms);
    }

    [[nodiscard]] tinykv::result<std::size_t> resolve_locks_of(
        std::string_view primary, tinykv::Ts lock_start_ts,
        std::optional<tinykv::Ts> commit_to) override {
        auto gate = hold_gate();
        auto victims = peer_(primary).mvcc().locks_of(primary, lock_start_ts);
        if (victims.empty()) {
            return std::size_t{0};
        }
        tinykv::result<void> outcome = std::unexpected(
            tinykv::InvalidArgument{"决议分支未执行"});
        if (commit_to.has_value()) {
            kvpb::TxnCommitRequest req;
            req.set_start_ts(lock_start_ts.value);
            req.set_commit_ts(commit_to->value);
            for (const auto& k : victims) {
                req.add_keys(k);
            }
            outcome = call_through_raft(
                primary, [&](tinykv::raftstore::Peer& p) { return p.propose_txn_commit(req); });
        } else {
            kvpb::TxnBatchRollbackRequest req;
            req.set_start_ts(lock_start_ts.value);
            for (const auto& k : victims) {
                req.add_keys(k);
            }
            outcome = call_through_raft(
                primary, [&](tinykv::raftstore::Peer& p) { return p.propose_txn_rollback(req); });
        }
        if (!outcome.has_value()) {
            return std::unexpected(outcome.error());
        }
        return victims.size();
    }

    // GC（FUNC-17）：Admin 命令走共识（红线：写走共识不豁免）
    [[nodiscard]] tinykv::result<void> gc(std::uint64_t safe_point) {
        kvpb::TxnGCRequest req;
        req.set_safe_point(safe_point);
        if (!peers_) {
            return call_through_raft(
                {}, [&](tinykv::raftstore::Peer& p) { return p.propose_txn_gc(req); });
        }
        // 多 region：Admin 显式广播到每个 peer（每个 region 各自走共识）
        for (auto* p : peers_()) {
            if (p == nullptr || !p->is_leader()) {
                return std::unexpected(tinykv::NotLeader{
                    .region_id = tinykv::RegionId{0}, .leader_hint = std::nullopt});
            }
            auto gate = hold_gate();
            const auto expect = p->raft().last_index().value + 1;
            if (!p->propose_txn_gc(req)) {
                return std::unexpected(tinykv::NotLeader{
                    .region_id = tinykv::RegionId{0}, .leader_hint = std::nullopt});
            }
            for (int round = 0; round < max_rounds_; ++round) {
                p->tick();
                (void)p->on_ready();
                if (p->applied_at_least(expect)) {
                    break;
                }
            }
            if (!p->applied_at_least(expect)) {
                return std::unexpected(tinykv::IoFailed{.path = "txn:gc",
                                                        .reason = "apply 未收敛（泵超时）"});
            }
            if (auto err = p->take_txn_error(expect); err.has_value()) {
                return std::unexpected(std::move(*err));
            }
        }
        return {};
    }

private:
    [[nodiscard]] std::optional<std::unique_lock<std::mutex>> hold_gate() const {
        if (gate_ != nullptr) {
            return std::unique_lock<std::mutex>(*gate_);
        }
        return std::nullopt;   // 测试装置无共享泵时可不设闸
    }

    template <typename ProposeFn>
    tinykv::result<void> call_through_raft(std::string_view key, ProposeFn&& propose) {
        auto gate = hold_gate();
        tinykv::raftstore::Peer& p = peer_(key);
        const auto expect = p.raft().last_index().value + 1;
        if (!propose(p)) {
            return std::unexpected(tinykv::NotLeader{.region_id = tinykv::RegionId{0},
                                                     .leader_hint = std::nullopt});
        }
        for (int round = 0; round < max_rounds_; ++round) {
            pump_(key);
            if (applied_(key, expect)) {
                break;
            }
        }
        if (!applied_(key, expect)) {
            return std::unexpected(tinykv::IoFailed{.path = "txn:raft",
                                                    .reason = "apply 未收敛（泵超时）"});
        }
        auto err = p.take_txn_error(expect);
        if (err.has_value()) {
            return std::unexpected(std::move(*err));   // 结构化错误直传（client 编排要字段）
        }
        return {};
    }

    PeerRef peer_;
    Pump pump_;
    Applied applied_;
    std::mutex* gate_{nullptr};
    int max_rounds_;
    PeerList peers_;
};

}  // namespace tinykv::server
