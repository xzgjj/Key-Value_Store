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

#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "common/error.h"
#include "scheduler/tso.h"
#include "txn/mvcc.h"

namespace tinykv::txn {

// get 的结果（含锁等待语义：blocked=遇未过期锁，调用方稍后重试）
struct TxnGetResult {
    std::optional<std::string> value;
    bool blocked{false};   // true = 遇未过期他者锁（value 必空）
};

class TxnClient {
public:
    TxnClient(Storage& storage, tso::TsProvider& tso)
        : mvcc_(storage), tso_(tso), lock_ttl_ms_(3000) {}

    // D4-1：lock TTL 可配（默认 3000ms）
    void set_lock_ttl(std::uint64_t ttl_ms) { lock_ttl_ms_ = ttl_ms; }

    [[nodiscard]] result<Ts> begin() {
        auto r = tso_.get_timestamps(1);
        if (!r.has_value() || r->empty()) {
            return std::unexpected(NotLeader{.region_id = RegionId{0},
                                             .leader_hint = std::nullopt});   // TSO 不可用
        }
        start_ts_ = r->front();
        writes_.clear();
        return start_ts_;
    }

    result<void> set(std::string key, std::string value) {
        if (key.empty()) {
            return std::unexpected(InvalidArgument{"空 key"});
        }
        if (!writes_.empty() && primary_.empty() && key < writes_.front().key) {
            // primary = 字典序最小的 key（确定性决议锚点）
        }
        writes_.push_back(Mutation{.op = Mutation::Op::PUT,
                                   .key = std::move(key),
                                   .value = std::move(value)});
        return {};
    }

    result<void> del(std::string key) {
        if (key.empty()) {
            return std::unexpected(InvalidArgument{"空 key"});
        }
        writes_.push_back(
            Mutation{.op = Mutation::Op::DELETE, .key = std::move(key), .value = {}});
        return {};
    }

    // 读：读己之写 / 快照读 / 锁等待+决议重试（now_ms 注入，确定性时钟）
    [[nodiscard]] result<TxnGetResult> get(const std::string& key, std::uint64_t now_ms) {
        for (const auto& m : writes_) {
            if (m.key == key && m.op == Mutation::Op::PUT) {
                return TxnGetResult{.value = m.value, .blocked = false};   // 读己之写
            }
        }
        auto r = mvcc_.get(key, start_ts_);
        if (r.has_value()) {
            return TxnGetResult{.value = std::move(*r), .blocked = false};
        }
        auto locked = resolve_blocking(std::move(r.error()), now_ms);
        if (!locked.has_value()) {
            return std::unexpected(locked.error());
        }
        if (*locked) {
            return TxnGetResult{.value = std::nullopt, .blocked = true};   // 未过期锁，稍后重试
        }
        // 决议完成 → 重试一次
        auto retry = mvcc_.get(key, start_ts_);
        if (!retry.has_value()) {
            return std::unexpected(retry.error());
        }
        return TxnGetResult{.value = std::move(*retry), .blocked = false};
    }

    // 提交：prewrite（primary=字典序最小 key）→ commit（primary 先、secondary 随后）。
    // 写写冲突原样上抛（SPEC：client 换 ts 重试或报用户，D4-5 重试策略在调用方）。
    [[nodiscard]] result<Ts> commit(std::uint64_t now_ms) {
        if (writes_.empty()) {
            return std::unexpected(InvalidArgument{"空事务无 mutations 可提交"});
        }
        primary_ = writes_.front().key;
        for (const auto& m : writes_) {
            primary_ = std::min(primary_, m.key);
        }
        auto pre = mvcc_.prewrite(writes_, primary_, start_ts_, lock_ttl_ms_, now_ms);
        if (!pre.has_value()) {
            auto locked = resolve_blocking(std::move(pre.error()), now_ms);
            if (!locked.has_value()) {
                return std::unexpected(locked.error());
            }
            if (*locked) {
                return std::unexpected(LockConflict{.key = primary_,
                                                    .primary = "",
                                                    .lock_start_ts = 0});   // 未过期，稍后重试
            }
            pre = mvcc_.prewrite(writes_, primary_, start_ts_, lock_ttl_ms_, now_ms);
            if (!pre.has_value()) {
                return std::unexpected(pre.error());
            }
        }
        auto ts = tso_.get_timestamps(1);
        if (!ts.has_value() || ts->empty()) {
            return std::unexpected(NotLeader{.region_id = RegionId{0},
                                             .leader_hint = std::nullopt});
        }
        const Ts commit_ts = ts->front();
        // primary 先提交（提交凭证），secondary 并发语义由调用方控制（D4-3 ≤8）
        std::vector<std::string> primary_keys{primary_};
        if (auto r = mvcc_.commit(primary_keys, start_ts_, commit_ts); !r.has_value()) {
            return std::unexpected(r.error());
        }
        std::vector<std::string> secondary;
        for (const auto& m : writes_) {
            if (m.key != primary_) {
                secondary.push_back(m.key);
            }
        }
        if (!secondary.empty()) {
            if (auto r = mvcc_.commit(secondary, start_ts_, commit_ts); !r.has_value()) {
                return std::unexpected(r.error());
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
        return mvcc_.rollback(keys, start_ts_);
    }

    [[nodiscard]] Ts start_ts() const { return start_ts_; }

private:
    // 锁冲突决议编排：committed → 正向推进；过期/回滚 → 清锁；未过期 → blocked=true。
    // 返回值：true=仍被未过期锁阻塞（调用方稍后重试）；false=已决议，可重试本操作。
    [[nodiscard]] result<bool> resolve_blocking(KvError&& err, std::uint64_t now_ms) {
        if (!std::holds_alternative<LockConflict>(err)) {
            return std::unexpected(std::move(err));   // 非锁冲突（如写写冲突）原样上抛
        }
        const auto lc = std::get<LockConflict>(err);
        auto st = mvcc_.check_txn_status(lc.primary, Ts{lc.lock_start_ts}, now_ms);
        if (!st.has_value()) {
            return std::unexpected(st.error());
        }
        if (st->committed) {
            // 正向推进：用对方的 commit_ts 补 Write 删 Lock（红线3：不存在永久死锁锁）
            auto r = mvcc_.resolve_locks_of(lc.primary, Ts{lc.lock_start_ts}, st->commit_ts);
            if (!r.has_value()) {
                return std::unexpected(r.error());
            }
            return false;
        }
        if (st->lock_ttl_remaining_ms > 0) {
            return true;   // 未过期：等待（调用方稍后带更新的 now_ms 重试）
        }
        // 过期或已回滚 → 清锁
        auto r = mvcc_.resolve_locks_of(lc.primary, Ts{lc.lock_start_ts}, std::nullopt);
        if (!r.has_value()) {
            return std::unexpected(r.error());
        }
        return false;
    }

    MvccTxn mvcc_;
    tso::TsProvider& tso_;
    Ts start_ts_{0};
    std::vector<Mutation> writes_;
    std::string primary_;
    std::uint64_t lock_ttl_ms_;
};

}  // namespace tinykv::txn
