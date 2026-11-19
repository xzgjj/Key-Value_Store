// 文件用途：TxnClient——client 驱动 2PC 编排（M5 第④/⑤层，SPEC-4 §4.3.2/§4.4）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
//
// 职责：Begin→Get/Set→Commit 的客户端生命周期 + 锁等待/决议重试的编排。
// server（MvccTxn + Latch）只保证单 key 原子性；跨 region 协调全部在 client（红线8）。
//
// 决议编排（红线2/3）：get/commit 遇 LockConflict → check_txn_status(primary) →
//   committed   → 正向推进（resolve_locks_of 补 Write 删 Lock，用对方的 commit_ts）；
//   未过期      → 返回 LockConflict（调用方稍后带更新的 now_ms 重试——确定性时钟注入）；
//   过期/回滚   → resolve_locks_of 反向清锁 → 本地重试。
// client 冲突重试上限 3 次、退避 10/50/200ms（D4-5）——本进程内版本退避由调用方控制，
// 这里只做"决议后自动重试"的语义收口。
// 提交歧义（BUG-0020/N-1）：跨区 COMMIT_PRIMARY 调用失败 ≠ 未提交——先
// check_txn_status(primary) 判生死，committed → 正向推进；确认未提交才回滚 secondary。

#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "common/error.h"
#include "scheduler/tso.h"
#include "txn/executor.h"
#include "txn/mvcc.h"
#include "txn/region_grouper.h"

namespace tinykv::txn {

// get 的结果（含锁等待语义：blocked=遇未过期锁，调用方稍后重试）
struct TxnGetResult {
    std::optional<std::string> value;
    bool blocked{false};  // true = 遇未过期他者锁（value 必空）
};

class TxnClient {
public:
    // 执行端注入：LocalExecutor（单机基线）或 Peer 端执行器（经 raft，⑤层）
    TxnClient(TxnExecutor& executor, tso::TsProvider& tso)
        : exec_(executor), tso_(tso), lock_ttl_ms_(3000) {}

    // D4-1：lock TTL 可配（默认 3000ms）
    void set_lock_ttl(std::uint64_t ttl_ms) { lock_ttl_ms_ = ttl_ms; }

    // M7 ⑦：注入 key→region 路由后，commit 会先按 region 分组：
    //   * 单组 → 走原单区路径（零额外开销）；
    //   * 多组 → PREPARE(每组一次) → TSO → COMMIT_PRIMARY → COMMIT_SECONDARY 的阶段机。
    // 不注入 = 关闭跨区（跨区 prewrite 仍被执行端护栏拒绝），行为与改造前一致。
    void set_region_router(txn::RegionOfFn router) { region_of_ = std::move(router); }
    [[nodiscard]] std::size_t last_region_count() const { return last_region_count_; }

    [[nodiscard]] result<Ts> begin() {
        auto r = tso_.get_timestamps(1);
        if (!r.has_value() || r->empty()) {
            return tinykv::unexpected(
                NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});  // TSO 不可用
        }
        start_ts_ = r->front();
        writes_.clear();
        return start_ts_;
    }

    result<void> set(std::string key, std::string value) {
        if (key.empty()) {
            return tinykv::unexpected(InvalidArgument{"空 key"});
        }
        // code-review P0-5：Default CF 的空串是 Delete 墓碑，事务路径不能再用它表示"存在但空"。
        // 在客户端边界就拒绝，避免提交后读出"不存在"的语义错误。
        if (value.empty()) {
            return tinykv::unexpected(
                InvalidArgument{"事务 Put 不接受空 value（空值语义与删除墓碑冲突）"});
        }
        writes_.push_back(
            Mutation{.op = Mutation::Op::kPut, .key = std::move(key), .value = std::move(value)});
        return {};
    }

    result<void> del(std::string key) {
        if (key.empty()) {
            return tinykv::unexpected(InvalidArgument{"空 key"});
        }
        writes_.push_back(Mutation{.op = Mutation::Op::kDel, .key = std::move(key), .value = {}});
        return {};
    }

    // 读：读己之写 / 快照读 / 锁等待+决议重试（now_ms 注入，确定性时钟）
    [[nodiscard]] result<TxnGetResult> get(const std::string& key, std::uint64_t now_ms) {
        // 读己之写：同一 key 多次 set 时最后一次胜出（与 prewrite 落盘值一致）
        for (auto it = writes_.rbegin(); it != writes_.rend(); ++it) {
            if (it->key == key && it->op == Mutation::Op::kPut) {
                return TxnGetResult{.value = it->value, .blocked = false};
            }
        }
        auto r = exec_.get(key, start_ts_);
        if (r.has_value()) {
            return TxnGetResult{.value = std::move(*r), .blocked = false};
        }
        auto locked = resolve_blocking(std::move(r.error()), now_ms);
        if (!locked.has_value()) {
            return tinykv::unexpected(locked.error());
        }
        if (*locked) {
            return TxnGetResult{.value = std::nullopt, .blocked = true};  // 未过期锁，稍后重试
        }
        // 决议完成 → 重试一次
        auto retry = exec_.get(key, start_ts_);
        if (!retry.has_value()) {
            return tinykv::unexpected(retry.error());
        }
        return TxnGetResult{.value = std::move(*retry), .blocked = false};
    }

    // 提交：prewrite（primary=字典序最小 key）→ commit（primary 先、secondary 随后）。
    // 写写冲突原样上抛（SPEC：client 换 ts 重试或报用户，D4-5 重试策略在调用方）。
    [[nodiscard]] result<Ts> commit(std::uint64_t now_ms) {
        if (writes_.empty()) {
            return tinykv::unexpected(InvalidArgument{"空事务无 mutations 可提交"});
        }
        primary_ = writes_.front().key;
        for (const auto& m : writes_) {
            primary_ = std::min(primary_, m.key);
        }
        // 阶段机第一步：PREPARE。分组只在注入了路由且确实跨区时生效。
        if (region_of_) {
            auto grouped = txn::group_by_region(writes_, primary_, region_of_);
            if (!grouped.has_value()) {
                return tinykv::unexpected(grouped.error());
            }
            last_region_count_ = grouped->region_count();
            if (!grouped->single_region()) {
                return commit_grouped(*grouped, now_ms);
            }
        } else {
            last_region_count_ = 1;
        }
        if (auto pre = prewrite_with_resolution(now_ms); !pre.has_value()) {
            return tinykv::unexpected(pre.error());
        }
        auto ts = tso_.get_timestamps(1);
        if (!ts.has_value() || ts->empty()) {
            return tinykv::unexpected(
                NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
        }
        const Ts commit_ts = ts->front();
        // primary 先提交（提交凭证），secondary 并发语义由调用方控制（D4-3 ≤8）
        std::vector<std::string> primary_keys{primary_};
        if (auto r = exec_.commit(primary_keys, start_ts_, commit_ts); !r.has_value()) {
            return tinykv::unexpected(r.error());
        }
        std::vector<std::string> secondary;
        for (const auto& m : writes_) {
            if (m.key != primary_) {
                secondary.push_back(m.key);
            }
        }
        if (!secondary.empty()) {
            if (auto r = exec_.commit(secondary, start_ts_, commit_ts); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
        }
        writes_.clear();
        return commit_ts;
    }

    [[nodiscard]] result<void> rollback() {
        std::vector<std::string> keys;
        keys.reserve(writes_.size());
        for (const auto& m : writes_) {
            keys.push_back(m.key);
        }
        writes_.clear();
        if (keys.empty()) {
            return {};  // 只读事务无锁可回滚（no-op 成功，不是错误）
        }
        return exec_.rollback(keys, start_ts_);
    }

    [[nodiscard]] Ts start_ts() const { return start_ts_; }

private:
    // 单区 prewrite + 一次冲突决议重试。抽出来是因为 commit/commit_grouped 都要这段，
    // 且它自身包含"决议后重试"的分支（留在 commit 里会把主流程压成不可读的长函数）。
    [[nodiscard]] result<void> prewrite_with_resolution(std::uint64_t now_ms) {
        auto pre = exec_.prewrite(writes_, primary_, start_ts_, lock_ttl_ms_, now_ms);
        if (pre.has_value()) {
            return {};
        }
        // 冲突字段必须原样上抛：伪造 {primary="", lock_start_ts=0} 会让调用方无法决议
        // （error.h 明确要求"未知时为 nullopt，禁止伪造"）
        const KvError conflict = pre.error();
        auto locked = resolve_blocking(std::move(pre.error()), now_ms);
        if (!locked.has_value()) {
            return tinykv::unexpected(locked.error());
        }
        if (*locked) {
            return tinykv::unexpected(conflict);  // 未过期锁：原样返回，调用方稍后重试
        }
        return exec_.prewrite(writes_, primary_, start_ts_, lock_ttl_ms_, now_ms);
    }

    // 跨区分组提交（M7 ⑦ 的核心）。顺序即协议：
    //   ① 每组各一次 prewrite（cross_region=true，主组也走这条路，保证 primary 落对分区）
    //   ② 取 commit_ts（TSO）——拿到之后，primary 的 Write 记录就是"已提交"的唯一凭证
    //   ③ COMMIT_PRIMARY：先提交主组（原子锚点落定）
    //   ④ COMMIT_SECONDARY：逐组提交；失败不回滚（也删不掉已提交的 primary），
    //      由读者遇锁 → check_txn_status(primary) 正向推进（恢复路径，红线 3）
    // 任一步 PREPARE 失败 → 回滚已成功的组（幂等），保持"要么全不可见、要么全可见"。
    [[nodiscard]] result<Ts> commit_grouped(const RegionGroups& grouped, std::uint64_t now_ms) {
        std::vector<std::size_t> prepared;
        for (std::size_t i = 0; i < grouped.groups.size(); ++i) {
            const auto& group = grouped.groups[i];
            auto pre = exec_.prewrite_routed(group.mutations.front().key, group.mutations, primary_,
                                             start_ts_, lock_ttl_ms_, now_ms, true);
            if (!pre.has_value()) {
                rollback_groups(grouped, prepared);
                const KvError conflict = pre.error();
                auto locked = resolve_blocking(std::move(pre.error()), now_ms);
                if (!locked.has_value()) {
                    return tinykv::unexpected(locked.error());
                }
                return tinykv::unexpected(conflict);  // 未过期锁：上抛让调用方稍后重试
            }
            prepared.push_back(i);
        }
        auto ts = tso_.get_timestamps(1);
        if (!ts.has_value() || ts->empty()) {
            rollback_groups(grouped, prepared);
            return tinykv::unexpected(
                NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
        }
        Ts commit_ts = ts->front();
        // ③ 主组先提交：整个事务的原子锚点
        if (auto r = commit_one_group(grouped.groups[grouped.primary_index], commit_ts);
            !r.has_value()) {
            // BUG-0020（code-review r2 N-1）：primary 提交歧义——"调用失败"≠"未提交"。
            // call_through_raft 泵超时但 apply 实际成功（BUG-0009 同款窗口）时，无条件
            // 回滚 secondary 会造出"primary 已提交 + secondary 墓碑"的部分提交（SI 被破
            // 坏），且不可自愈：secondary 的锁已成 rollback 墓碑，正向决议永远失败。
            // 协议（Percolator 通行做法）：先判 primary 生死——
            //   committed    → 正向推进（secondary 用判定的 commit_ts 继续 ④）；
            //   确认未提交   → 才允许回滚 others；
            //   未决/判定失败 → 保持现状（宁挂勿错：动 secondary 才是制造部分提交）。
            const auto verdict = judge_primary_ambiguity(now_ms);
            if (!verdict.has_value() || verdict->pending) {
                return tinykv::unexpected(r.error());  // 保持现状：锁决议/重试给出最终结论
            }
            if (!verdict->committed) {
                std::vector<std::size_t> others;
                for (std::size_t i = 0; i < grouped.groups.size(); ++i) {
                    if (i != grouped.primary_index) {
                        others.push_back(i);
                    }
                }
                rollback_groups(grouped, others);  // 确认未提交：回滚其余组是安全的
                return tinykv::unexpected(r.error());
            }
            commit_ts = verdict->commit_ts;  // 已提交：以判定凭证（真实 commit_ts）继续 ④
        }
        // ④ 其余组：失败不回滚，交给恢复路径
        for (std::size_t i = 0; i < grouped.groups.size(); ++i) {
            if (i == grouped.primary_index) {
                continue;
            }
            if (auto r = commit_one_group(grouped.groups[i], commit_ts); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
        }
        writes_.clear();
        return commit_ts;
    }

    // BUG-0020（N-1）：primary 提交失败后的生死判定。check_txn_status(primary) 三分支：
    //   committed    → committed=true（commit_ts 为判定出的真实提交时刻，作正向推进凭证）；
    //   无锁/已回滚/已过期 → committed=false, pending=false（确认未提交，回滚 others 安全）；
    //   TTL 未过期   → pending=true（生死未卜，保持现状——回滚可能踩掉已提交的事务）。
    struct PrimaryVerdict {
        bool committed{false};
        bool pending{true};
        Ts commit_ts{Ts{0}};
    };
    [[nodiscard]] result<PrimaryVerdict> judge_primary_ambiguity(std::uint64_t now_ms) {
        auto st = exec_.check_txn_status(primary_, start_ts_, now_ms);
        if (!st.has_value()) {
            return tinykv::unexpected(st.error());
        }
        if (st->committed) {
            return PrimaryVerdict{.committed = true, .pending = false,
                                  .commit_ts = st->commit_ts};
        }
        return PrimaryVerdict{.committed = false, .pending = st->lock_ttl_remaining_ms > 0,
                              .commit_ts = Ts{0}};
    }

    [[nodiscard]] result<void> commit_one_group(const RegionGroup& group, Ts commit_ts) {
        std::vector<std::string> keys;
        keys.reserve(group.mutations.size());
        for (const auto& m : group.mutations) {
            keys.push_back(m.key);
        }
        return exec_.commit(keys, start_ts_, commit_ts);
    }

    // 回滚已 PREPARE 的组（幂等；单个失败不阻断——锁决议会兜底）
    void rollback_groups(const RegionGroups& grouped, const std::vector<std::size_t>& which) {
        for (const auto idx : which) {
            if (idx >= grouped.groups.size()) {
                continue;
            }
            std::vector<std::string> keys;
            for (const auto& m : grouped.groups[idx].mutations) {
                keys.push_back(m.key);
            }
            (void)exec_.rollback(keys, start_ts_);
        }
    }

    // 锁冲突决议编排：committed → 正向推进；过期/回滚 → 清锁；未过期 → blocked=true。
    // 返回值：true=仍被未过期锁阻塞（调用方稍后重试）；false=已决议，可重试本操作。
    [[nodiscard]] result<bool> resolve_blocking(KvError&& err, std::uint64_t now_ms) {
        if (!std::holds_alternative<LockConflict>(err)) {
            return tinykv::unexpected(std::move(err));  // 非锁冲突（如写写冲突）原样上抛
        }
        const auto lc = std::get<LockConflict>(err);
        auto st = exec_.check_txn_status(lc.primary, Ts{lc.lock_start_ts}, now_ms);
        if (!st.has_value()) {
            return tinykv::unexpected(st.error());
        }
        if (st->committed) {
            // 正向推进：用对方的 commit_ts 补 Write 删 Lock（红线3：不存在永久死锁锁）
            auto r = exec_.resolve_locks_of(lc.primary, Ts{lc.lock_start_ts}, st->commit_ts);
            if (!r.has_value()) {
                return tinykv::unexpected(r.error());
            }
            return false;
        }
        if (st->lock_ttl_remaining_ms > 0) {
            return true;  // 未过期：等待（调用方稍后带更新的 now_ms 重试）
        }
        // 过期或已回滚 → 清锁
        auto r = exec_.resolve_locks_of(lc.primary, Ts{lc.lock_start_ts}, std::nullopt);
        if (!r.has_value()) {
            return tinykv::unexpected(r.error());
        }
        return false;
    }

    TxnExecutor& exec_;
    tso::TsProvider& tso_;
    Ts start_ts_{0};
    std::vector<Mutation> writes_;
    std::string primary_;
    std::uint64_t lock_ttl_ms_;
    txn::RegionOfFn region_of_;         // 空 = 未启用跨区（单区路径）
    std::size_t last_region_count_{1};  // 观测：上次提交跨了几个 region
};

}  // namespace tinykv::txn
