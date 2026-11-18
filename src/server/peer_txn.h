// 文件用途：Peer 端事务执行器（M6 第③层）——TxnExecutor 的经 raft 生产实现。
// 归属：TinyKV-CPP（M6 运行时；从 tests/testlib 上移：经 raft 执行是生产路径，不是测试装置）。
//
// 三个注入点让同一实现适配单机 daemon 与多副本测试：
//   peer()     —— 目标 Peer（多副本时动态找 leader；单机固定返回自己）
//   pump()     —— 推动共识前进（daemon=Worker 已在后台泵，这里只等；测试=主动 pump）
//   applied()  —— 收敛判定（单机=本 peer 到位；多副本=全部副本到位）
// 读路径（get/check_txn_status/locks_of）：统一走 Peer::wait_read_index——
// quorum 确认 + applied 追平后才读状态机，不再依赖 leader 本地直读的弱一致语义。

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

// proto 枚举里的 DELETE 与 Windows SDK 的 DELETE 宏同名（winnt.h，经 asio/网页头引入）。
// 在宏处于展开状态时写 `kvpb::Mutation::DELETE` 会被换成 `kvpb::Mutation::0x00010000L`，
// 报错信息还离现场很远。这里按数值引用一次，彻底绕开宏——proto/kv.proto 已把 Op 固定为
// PUT=0 / DELETE=1，数值是契约的一部分（改枚举值等于改线上格式，本就在禁改清单里）。
inline constexpr kvpb::Mutation::Op kMutationDelete = static_cast<kvpb::Mutation::Op>(1);

class PeerTxnExecutor : public tinykv::txn::TxnExecutor {  // 非 final：测试适配器派生注入收敛策略
public:
    using PeerRef = std::function<tinykv::raftstore::Peer&(std::string_view key)>;
    using Pump = std::function<void(std::string_view key)>;
    using Applied = std::function<bool(std::string_view key, std::uint64_t index)>;
    using PeerList = std::function<std::vector<tinykv::raftstore::Peer*>()>;  // GC 广播用
    // key → 所属 region（ARCH-1 P0-1 护栏用）。不注入 = 关闭护栏（单 region / 测试装置）。
    using RegionOf = std::function<std::optional<tinykv::RegionId>(std::string_view key)>;

    // gate：daemon 组合根注入的 Peer 串行闸（Worker 泵与 HTTP 共用；泵回调不得重复加锁）
    PeerTxnExecutor(PeerRef peer, Pump pump, Applied applied, std::mutex* gate,
                    int max_rounds = 400, PeerList peers = {}, RegionOf region_of = {})
        : peer_(std::move(peer)),
          pump_(std::move(pump)),
          applied_(std::move(applied)),
          gate_(gate),
          max_rounds_(max_rounds),
          peers_(std::move(peers)),
          region_of_(std::move(region_of)) {}

    [[nodiscard]] tinykv::result<std::optional<std::string>> get(std::string_view key,
                                                                 tinykv::Ts start_ts) override {
        auto gate = hold_gate();
        auto& p = peer_(key);
        const auto ri = p.wait_read_index([&] { pump_(key); });
        if (!ri.has_value()) {
            return tinykv::unexpected(ri.error());
        }
        return p.mvcc().get(key, start_ts);
    }

    [[nodiscard]] tinykv::result<void> prewrite(tinykv::span<const tinykv::txn::Mutation> mutations,
                                                std::string_view primary, tinykv::Ts start_ts,
                                                std::uint64_t ttl_ms,
                                                std::uint64_t now_ms) override {
        // ARCH-1 P0-1：护栏必须先于协调器。整批 mutations 会经 primary 所在 peer 一次提案，
        // 而读路径按 key 路由——跨区写入会静默落进 primary 的分区，对外表现"提交成功但数据不见了"。
        if (auto guard = guard_single_region(mutations, primary); !guard.has_value()) {
            return tinykv::unexpected(guard.error());
        }
        // ARCH-1 P0-2：加锁时刻必须随请求走完链路。丢掉它，apply 侧只能写 0，
        // check_txn_status 就会把任何运行超过 TTL 的进程新加的锁判成"已过期"。
        kvpb::TxnPrewriteRequest req;
        req.set_primary_lock(std::string(primary));
        req.set_start_ts(start_ts.value);
        req.set_lock_ttl(ttl_ms);
        req.set_created_ms(now_ms);
        for (const auto& m : mutations) {
            auto* one = req.add_mutations();
            one->set_op(m.op == tinykv::txn::Mutation::Op::kPut ? kvpb::Mutation::PUT
                                                                : kMutationDelete);
            one->set_key(m.key);
            one->set_value(m.value);
        }
        return call_through_raft(
            primary, [&](tinykv::raftstore::Peer& p) { return p.propose_txn_prewrite(req); });
    }

    // 跨区分组 prewrite：本批 mutations 必须同属 route_key 的 region（跨组越界当场拦下，
    // 否则又会退化成"写进别的分区"）。primary 允许在别组（cross_region=true）。
    [[nodiscard]] tinykv::result<void> prewrite_routed(
        std::string_view route_key, tinykv::span<const tinykv::txn::Mutation> mutations,
        std::string_view primary, tinykv::Ts start_ts, std::uint64_t ttl_ms, std::uint64_t now_ms,
        bool cross_region) override {
        if (auto guard = guard_batch_region(route_key, mutations, cross_region);
            !guard.has_value()) {
            return tinykv::unexpected(guard.error());
        }
        kvpb::TxnPrewriteRequest req;
        req.set_primary_lock(std::string(primary));
        req.set_start_ts(start_ts.value);
        req.set_lock_ttl(ttl_ms);
        req.set_created_ms(now_ms);
        req.set_cross_region(cross_region);
        for (const auto& m : mutations) {
            auto* one = req.add_mutations();
            one->set_op(m.op == tinykv::txn::Mutation::Op::kPut ? kvpb::Mutation::PUT
                                                                : kMutationDelete);
            one->set_key(m.key);
            one->set_value(m.value);
        }
        return call_through_raft(
            route_key, [&](tinykv::raftstore::Peer& p) { return p.propose_txn_prewrite(req); });
    }

    [[nodiscard]] tinykv::result<void> commit(tinykv::span<const std::string> keys,
                                              tinykv::Ts start_ts, tinykv::Ts commit_ts) override {
        kvpb::TxnCommitRequest req;
        req.set_start_ts(start_ts.value);
        req.set_commit_ts(commit_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft(
            keys.empty() ? std::string_view{} : keys.front(),
            [&](tinykv::raftstore::Peer& p) { return p.propose_txn_commit(req); });
    }

    [[nodiscard]] tinykv::result<void> rollback(tinykv::span<const std::string> keys,
                                                tinykv::Ts start_ts) override {
        kvpb::TxnBatchRollbackRequest req;
        req.set_start_ts(start_ts.value);
        for (const auto& k : keys) {
            req.add_keys(k);
        }
        return call_through_raft(
            keys.empty() ? std::string_view{} : keys.front(),
            [&](tinykv::raftstore::Peer& p) { return p.propose_txn_rollback(req); });
    }

    [[nodiscard]] tinykv::result<tinykv::txn::TxnStatus> check_txn_status(
        std::string_view primary, tinykv::Ts lock_start_ts, std::uint64_t now_ms) override {
        auto gate = hold_gate();
        auto& p = peer_(primary);
        const auto ri = p.wait_read_index([&] { pump_(primary); });
        if (!ri.has_value()) {
            return tinykv::unexpected(ri.error());
        }
        return p.mvcc().check_txn_status(primary, lock_start_ts, now_ms);
    }

    // 锁决议（红线 3）：该事务的锁可能分散在多个 region 的引擎里（跨区事务），
    // 因此按 peer 广播——只清 primary 所在 region 会留下"永远解不掉的锁"，
    // 读者每次读都撞 LockConflict，直到 TTL 过期。单区装配下 peers_ 只有一个，
    // 行为与改造前一致。
    [[nodiscard]] tinykv::result<std::size_t> resolve_locks_of(
        std::string_view primary, tinykv::Ts lock_start_ts,
        std::optional<tinykv::Ts> commit_to) override {
        auto gate = hold_gate();
        auto& primary_peer = peer_(primary);
        const auto ri = primary_peer.wait_read_index([&] { pump_(primary); });
        if (!ri.has_value()) {
            return tinykv::unexpected(ri.error());
        }
        std::vector<tinykv::raftstore::Peer*> targets;
        if (peers_) {
            targets = peers_();
        } else {
            targets.push_back(&primary_peer);
        }
        // 先收集各 region 的 victim，再统一放开闸提案——收集期间不持写锁，避免自我死锁
        std::vector<std::pair<tinykv::raftstore::Peer*, std::vector<std::string>>> victims;
        for (auto* p : targets) {
            if (p == nullptr) {
                continue;
            }
            auto found = p->mvcc().locks_of(primary, lock_start_ts);
            if (!found.has_value()) {
                return tinykv::unexpected(found.error());  // 扫描失败/超上限：不静默遗留
            }
            if (!found->empty()) {
                victims.emplace_back(p, std::move(*found));
            }
        }
        if (victims.empty()) {
            return std::size_t{0};
        }
        gate.reset();  // 写路径自己再加闸：同一线程不可对同一 mutex 重复 lock
        std::size_t total = 0;
        for (auto& [peer, keys] : victims) {
            (void)peer;
            tinykv::result<void> outcome =
                tinykv::unexpected(tinykv::InvalidArgument{"决议分支未执行"});
            if (commit_to.has_value()) {
                kvpb::TxnCommitRequest req;
                req.set_start_ts(lock_start_ts.value);
                req.set_commit_ts(commit_to->value);
                for (const auto& k : keys) {
                    req.add_keys(k);
                }
                outcome = call_through_raft(keys.front(), [&](tinykv::raftstore::Peer& p) {
                    return p.propose_txn_commit(req);
                });
            } else {
                kvpb::TxnBatchRollbackRequest req;
                req.set_start_ts(lock_start_ts.value);
                for (const auto& k : keys) {
                    req.add_keys(k);
                }
                outcome = call_through_raft(keys.front(), [&](tinykv::raftstore::Peer& p) {
                    return p.propose_txn_rollback(req);
                });
            }
            if (!outcome.has_value()) {
                return tinykv::unexpected(outcome.error());
            }
            total += keys.size();
        }
        return total;
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
                return tinykv::unexpected(tinykv::NotLeader{.region_id = tinykv::RegionId{0},
                                                            .leader_hint = std::nullopt});
            }
            auto gate = hold_gate();
            const auto expect = p->raft().last_index().value + 1;
            if (!p->propose_txn_gc(req)) {
                return tinykv::unexpected(tinykv::NotLeader{.region_id = tinykv::RegionId{0},
                                                            .leader_hint = std::nullopt});
            }
            for (int round = 0; round < max_rounds_; ++round) {
                p->tick();
                (void)p->on_ready();
                if (p->applied_at_least(expect)) {
                    break;
                }
            }
            if (!p->applied_at_least(expect)) {
                return tinykv::unexpected(
                    tinykv::IoFailed{.path = "txn:gc", .reason = "apply 未收敛（泵超时）"});
            }
            if (auto err = p->take_txn_error(expect); err.has_value()) {
                return tinykv::unexpected(std::move(*err));
            }
        }
        return {};
    }

private:
    // ARCH-1 P0-1 / SPEC-6 §6.2.1：跨 region 事务护栏（⑦-0）。
    // 判据：primary 与每个 mutation 必须落在同一 region；任一不成立即结构化拒绝，
    // 绝不允许退回"全写 primary"的旧行为（SPEC-6 §6.5 红线 1）。
    // 分组守卫：本批必须全部落在 route_key 的 region；primary 的归属由调用方（协调器）保证。
    [[nodiscard]] tinykv::result<void> guard_batch_region(
        std::string_view route_key, tinykv::span<const tinykv::txn::Mutation> mutations,
        bool cross_region) const {
        if (!region_of_) {
            return {};  // 未注入路由：单 region 装配，无跨区可能
        }
        const auto batch_region = region_of_(route_key);
        if (!batch_region.has_value()) {
            return tinykv::unexpected(tinykv::KeyNotInRegion{.key = std::string(route_key),
                                                             .region_id = tinykv::RegionId{0}});
        }
        for (const auto& m : mutations) {
            const auto region = region_of_(m.key);
            if (!region.has_value() || *region != *batch_region) {
                return tinykv::unexpected(tinykv::InvalidArgument{
                    "分组 prewrite 的 key 必须同属一个 region（key=" + m.key +
                    "，route=" + std::string(route_key) + "）"});
            }
        }
        (void)cross_region;  // 只影响 apply 侧的 primary 校验，分组侧恒要求同区
        return {};
    }

    [[nodiscard]] tinykv::result<void> guard_single_region(
        tinykv::span<const tinykv::txn::Mutation> mutations, std::string_view primary) const {
        if (!region_of_) {
            return {};  // 未注入路由：单 region 装配或测试装置，无跨区可能
        }
        const auto primary_region = region_of_(primary);
        if (!primary_region.has_value()) {
            return tinykv::unexpected(tinykv::KeyNotInRegion{.key = std::string(primary),
                                                             .region_id = tinykv::RegionId{0}});
        }
        for (const auto& m : mutations) {
            const auto region = region_of_(m.key);
            if (!region.has_value() || *region != *primary_region) {
                return tinykv::unexpected(
                    tinykv::InvalidArgument{"跨区事务未启用（key=" + m.key + " 属 region " +
                                            std::to_string(region.has_value() ? region->value : 0) +
                                            "，primary=" + std::string(primary) + " 属 region " +
                                            std::to_string(primary_region->value) + "）"});
            }
        }
        return {};
    }

    [[nodiscard]] std::optional<std::unique_lock<std::mutex>> hold_gate() const {
        if (gate_ != nullptr) {
            return std::unique_lock<std::mutex>(*gate_);
        }
        return std::nullopt;  // 测试装置无共享泵时可不设闸
    }

    template <typename ProposeFn>
    tinykv::result<void> call_through_raft(std::string_view key, ProposeFn&& propose) {
        auto gate = hold_gate();
        tinykv::raftstore::Peer& p = peer_(key);
        const auto expect = p.raft().last_index().value + 1;
        if (!propose(p)) {
            return tinykv::unexpected(
                tinykv::NotLeader{.region_id = tinykv::RegionId{0}, .leader_hint = std::nullopt});
        }
        for (int round = 0; round < max_rounds_; ++round) {
            pump_(key);
            if (applied_(key, expect)) {
                break;
            }
        }
        if (!applied_(key, expect)) {
            return tinykv::unexpected(
                tinykv::IoFailed{.path = "txn:raft", .reason = "apply 未收敛（泵超时）"});
        }
        auto err = p.take_txn_error(expect);
        if (err.has_value()) {
            return tinykv::unexpected(std::move(*err));  // 结构化错误直传（client 编排要字段）
        }
        return {};
    }

    PeerRef peer_;
    Pump pump_;
    Applied applied_;
    std::mutex* gate_{nullptr};
    int max_rounds_;
    PeerList peers_;
    RegionOf region_of_;  // 空 = 单 region 装配（护栏不生效）
};

}  // namespace tinykv::server
