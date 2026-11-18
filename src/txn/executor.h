// 文件用途：事务执行端抽象（M5 第⑤层，SPEC-4 架构：client 驱动 2PC）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
//
// TxnClient 只做编排，执行端可插拔：
//   LocalExecutor —— 进程内直连 MvccTxn（单机/测试基线）；
//   Peer 端（⑤层）—— 序列化 kvpb 消息进 raft 提案，apply 时经 Latch+MvccTxn 落三列族。
// 两个执行端的语义完全一致（同一套 MvccTxn 原语），差别只在"共识要不要走"。

#pragma once

#include <optional>
#include <string>

#include "common/compat.h"
#include "common/error.h"
#include "txn/mvcc.h"

namespace tinykv::txn {

class TxnExecutor {
public:
    virtual ~TxnExecutor() = default;

    [[nodiscard]] virtual result<std::optional<std::string>> get(std::string_view key,
                                                                 Ts start_ts) = 0;
    [[nodiscard]] virtual result<void> prewrite(tinykv::span<const Mutation> mutations,
                                                std::string_view primary, Ts start_ts,
                                                std::uint64_t ttl_ms, std::uint64_t now_ms) = 0;
    [[nodiscard]] virtual result<void> commit(tinykv::span<const std::string> keys, Ts start_ts,
                                              Ts commit_ts) = 0;
    [[nodiscard]] virtual result<void> rollback(tinykv::span<const std::string> keys,
                                                Ts start_ts) = 0;
    [[nodiscard]] virtual result<TxnStatus> check_txn_status(std::string_view primary,
                                                             Ts lock_start_ts,
                                                             std::uint64_t now_ms) = 0;
    [[nodiscard]] virtual result<std::size_t> resolve_locks_of(std::string_view primary,
                                                               Ts lock_start_ts,
                                                               std::optional<Ts> commit_to) = 0;
};

// 进程内直连 MvccTxn（单机语义基线）
class LocalExecutor final : public TxnExecutor {
public:
    explicit LocalExecutor(Storage& storage) : mvcc_(storage) {}

    [[nodiscard]] result<std::optional<std::string>> get(std::string_view key,
                                                         Ts start_ts) override {
        return mvcc_.get(key, start_ts);
    }
    [[nodiscard]] result<void> prewrite(tinykv::span<const Mutation> mutations,
                                        std::string_view primary, Ts start_ts, std::uint64_t ttl_ms,
                                        std::uint64_t now_ms) override {
        return mvcc_.prewrite(mutations, primary, start_ts, ttl_ms, now_ms);
    }
    [[nodiscard]] result<void> commit(tinykv::span<const std::string> keys, Ts start_ts,
                                      Ts commit_ts) override {
        return mvcc_.commit(keys, start_ts, commit_ts);
    }
    [[nodiscard]] result<void> rollback(tinykv::span<const std::string> keys,
                                        Ts start_ts) override {
        return mvcc_.rollback(keys, start_ts);
    }
    [[nodiscard]] result<TxnStatus> check_txn_status(std::string_view primary, Ts lock_start_ts,
                                                     std::uint64_t now_ms) override {
        return mvcc_.check_txn_status(primary, lock_start_ts, now_ms);
    }
    [[nodiscard]] result<std::size_t> resolve_locks_of(std::string_view primary, Ts lock_start_ts,
                                                       std::optional<Ts> commit_to) override {
        return mvcc_.resolve_locks_of(primary, lock_start_ts, commit_to);
    }

private:
    MvccTxn mvcc_;
};

}  // namespace tinykv::txn
