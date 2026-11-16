// 文件用途：Peer 端事务执行器（M5 第⑤层）——TxnExecutor 的经 raft 实现。
// 归属：TinyKV-CPP（tests/testlib：传输泵是测试装置；生产 Worker 属 daemon 化工作，记 notes-m5）。
//
// 写路径（prewrite/commit/rollback）：构造 kvpb 消息 → Peer::propose_txn_* 进 raft →
// 泵到位点 → take_txn_error 取结构化错误（成功=nullopt）。
// 读路径（get/check_txn_status/resolve 的 victim 枚举）：leader 本地 MVCC 直读
// （M2 leader 本地读同款偏差，记 notes-m5）。
// 决议写入（resolve_locks_of 的 rollback/commit）必须经 raft——否则副本会分叉。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <kv.pb.h>

#include <gtest/gtest.h>

#include "peer_cluster.h"
#include "txn/executor.h"

namespace tinykv::testlib {

class PeerTxnExecutor final : public tinykv::txn::TxnExecutor {
public:
    explicit PeerTxnExecutor(PeerCluster& cluster) : cluster_(cluster) {}

    [[nodiscard]] tinykv::result<std::optional<std::string>> get(
        std::string_view key, tinykv::Ts start_ts) override {
        return leader().mvcc().get(key, start_ts);
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
            one->set_op(m.op == tinykv::txn::Mutation::Op::PUT ? kvpb::Mutation::PUT
                                                               : kvpb::Mutation::DELETE);
            one->set_key(m.key);
            one->set_value(m.value);
        }
        return call_through_raft(
            [&](Peer& p) { return p.propose_txn_prewrite(req); });
    }

    [[nodiscard]] tinykv::result<void> commit(std::span<const std::string> keys,
                                              tinykv::Ts start_ts,
                                              tinykv::Ts commit_ts) override {
        kvpb::TxnCommitRequest req;
        req.set_start_ts(start_ts.value);
        req.set_commit_ts(commit_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft([&](Peer& p) { return p.propose_txn_commit(req); });
    }

    [[nodiscard]] tinykv::result<void> rollback(std::span<const std::string> keys,
                                                tinykv::Ts start_ts) override {
        kvpb::TxnBatchRollbackRequest req;
        req.set_start_ts(start_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft([&](Peer& p) { return p.propose_txn_rollback(req); });
    }

    [[nodiscard]] tinykv::result<tinykv::txn::TxnStatus> check_txn_status(
        std::string_view primary, tinykv::Ts lock_start_ts, std::uint64_t now_ms) override {
        return leader().mvcc().check_txn_status(primary, lock_start_ts, now_ms);
    }

    // 决议写入经 raft：victim 枚举走 leader 本地（只读），rollback/commit 提案走共识
    [[nodiscard]] tinykv::result<std::size_t> resolve_locks_of(
        std::string_view primary, tinykv::Ts lock_start_ts,
        std::optional<tinykv::Ts> commit_to) override {
        auto victims = leader().mvcc().locks_of(primary, lock_start_ts);
        if (victims.empty()) {
            return std::size_t{0};
        }
        std::optional<tinykv::result<void>> r;   // 决议分支必有一路执行
        if (commit_to.has_value()) {
            kvpb::TxnCommitRequest req;
            req.set_start_ts(lock_start_ts.value);
            req.set_commit_ts(commit_to->value);
            for (const auto& k : victims) {
                req.add_keys(k);
            }
            r = call_through_raft([&](Peer& p) { return p.propose_txn_commit(req); });
        } else {
            kvpb::TxnBatchRollbackRequest req;
            req.set_start_ts(lock_start_ts.value);
            for (const auto& k : victims) {
                req.add_keys(k);
            }
            r = call_through_raft([&](Peer& p) { return p.propose_txn_rollback(req); });
        }
        if (!r.has_value() || !r->has_value()) {
            return std::unexpected(r.has_value() ? r->error()
                                                 : tinykv::InvalidArgument{"决议分支未执行"});
        }
        return victims.size();
    }

private:

    [[nodiscard]] Peer& leader() {
        const int idx = cluster_.leader();
        if (idx < 0) {
            ADD_FAILURE() << "事务执行需要 leader";
            return cluster_.peer(0);   // 调用方断言会兜住（leader 缺失必失败）
        }
        return cluster_.peer(idx);
    }

    // 提案 → 泵到 apply 位点越过本条 → 取结构化错误（nullopt = 成功）
    template <typename ProposeFn>
    tinykv::result<void> call_through_raft(ProposeFn&& propose) {
        Peer& p = leader();
        const auto expect = p.raft().last_index().value + 1;
        if (!propose(p)) {
            return std::unexpected(tinykv::NotLeader{.region_id = tinykv::RegionId{0},
                                                     .leader_hint = std::nullopt});
        }
        // 等全部副本 apply 到位（不只 leader）——调用方随后读任意副本的 MVCC 状态
        bool all_applied = false;
        for (int round = 0; round < 400 && !all_applied; ++round) {
            cluster_.pump();
            for (int n = 0; n < cluster_.size(); ++n) {
                cluster_.peer(n).tick();
            }
            cluster_.pump();
            all_applied = true;
            for (int n = 0; n < cluster_.size(); ++n) {
                if (!cluster_.peer(n).applied_at_least(expect)) {
                    all_applied = false;
                }
            }
        }
        if (!all_applied) {
            return std::unexpected(tinykv::IoFailed{.path = "txn:raft",
                                                    .reason = "副本 apply 未收敛（泵超时）"});
        }
        auto err = p.take_txn_error(expect);
        if (err.has_value()) {
            return std::unexpected(std::move(*err));   // 结构化错误直传（client 编排要字段）
        }
        return {};
    }

    PeerCluster& cluster_;
};

}  // namespace tinykv::testlib
