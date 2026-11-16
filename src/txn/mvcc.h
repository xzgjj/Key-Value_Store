// 文件用途：MvccTxn——Percolator 三列族 MVCC 读写（M5 第③层，SPEC-4 §4.3.2/§4.4）。
// 归属：TinyKV-CPP（M5 Percolator 事务）。
//
// 数据布局（SPEC-4 架构，encode 见 common/encode.h，ts 大端 → 同 key 多版本字节序有序）：
//   Default CF: encode_ts_key(Default, key, start_ts) → value        （数据；commit 后留作版本）
//   Lock    CF: encode_key(Lock, key)             → LockValue        （每 key 至多一把锁）
//   Write   CF: encode_ts_key(Write, key, commit_ts) → BE(start_ts)  （提交记录）
//
// 红线落地：
//   红线1 写写冲突立即 abort 自己（绝不等待，REL-06 无死锁的结构性保证）；
//   红线2 primary 决议唯一——本类只做机械读写，生死判定全部走 check_txn_status(primary)；
//   红线5 ts 大端编码，"commitTs ≤ start_ts 最新版本"依赖 Write CF 有序扫描。
//
// 确定性时钟偏差（记 notes-m5）：TTL 判定需要"现在"——为可复现，prewrite/check_txn_status
// 显式接收 now_ms 参数（与 ④b 追平门槛用 tick 而非墙钟同一取向），不读墙钟。

#pragma once

#include "common/encode.h"
#include "common/error.h"
#include "common/ids.h"
#include "storage/storage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tinykv::txn {

// 客户端侧变更（与 kvpb::Mutation 对应；server 侧 apply 用同一结构）
struct Mutation {
    // Op 显式取值对齐 kvpb::Mutation.Op（PUT=0/DELETE=1）
    enum class Op : std::uint8_t { PUT = 0, DELETE = 1 };
    Op op{Op::PUT};
    std::string key;
    std::string value;
};

// CheckTxnStatus 结果（SPEC-4：committed(commit_ts) | rolled_back | lock_ttl_remaining(ms)）
struct TxnStatus {
    bool committed{false};
    Ts commit_ts{0};                        // committed 时 > 0
    std::uint64_t lock_ttl_remaining_ms{0};  // 未过期时 > 0；过期/已终态 = 0
    bool rolled_back{false};
};

// Lock CF 值布局：[flag:1B][start_ts:8B BE][ttl_ms:4B BE][created_ms:8B BE][primary_len:4B BE][primary]
// flag: 0x01 = 活跃锁；0x02 = rollback 墓碑（决议依据之一，prewrite 遇到可覆盖）
inline constexpr std::uint8_t kLockFlagActive = 0x01;
inline constexpr std::uint8_t kLockFlagRollback = 0x02;

struct LockValue {
    std::uint8_t flag{kLockFlagActive};
    Ts start_ts{0};
    std::uint64_t ttl_ms{0};
    std::uint64_t created_ms{0};
    std::string primary;
};

[[nodiscard]] inline std::string be64_str(std::uint64_t v) {
    std::string out(8, '\0');
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<char>((v >> ((7 - i) * 8)) & 0xFFU);
    }
    return out;
}

[[nodiscard]] inline std::string encode_lock_value(const LockValue& lv) {
    std::string out;
    out.reserve(1 + 8 + 4 + 8 + 4 + lv.primary.size());
    out.push_back(static_cast<char>(lv.flag));
    auto put_be = [&out](std::uint64_t v, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) {
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
        }
    };
    put_be(lv.start_ts.value, 8);
    put_be(lv.ttl_ms, 4);
    put_be(lv.created_ms, 8);
    put_be(lv.primary.size(), 4);
    out += lv.primary;
    return out;
}

[[nodiscard]] inline std::optional<LockValue> decode_lock_value(std::string_view raw) {
    if (raw.size() < 25) {
        return std::nullopt;   // 损坏（长度下界：1+8+4+8+4）
    }
    LockValue lv;
    auto read_be = [&raw](std::size_t off, int bytes) {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            v = (v << 8) | static_cast<std::uint8_t>(raw[off + static_cast<std::size_t>(i)]);
        }
        return v;
    };
    lv.flag = static_cast<std::uint8_t>(raw[0]);
    lv.start_ts = Ts{read_be(1, 8)};
    lv.ttl_ms = read_be(9, 4);
    lv.created_ms = read_be(13, 8);
    const auto plen = read_be(21, 4);
    if (raw.size() < 25 + plen) {
        return std::nullopt;
    }
    lv.primary.assign(raw.substr(25, plen));
    return lv;
}

// MvccTxn 视角的"存储用户键"：user_key + 大端 ts 后缀（不带 CF 前缀——
// Engine::get/scan/write 自带 CF 编码（"凡进引擎的 key 都经 encode 函数"，前缀由引擎补），
// 调用方再拼一次就会双重前缀）。ts 大端保证同 user_key 多版本字节序有序（SPEC-4 红线5）。
[[nodiscard]] inline std::string data_key(std::string_view key, Ts start_ts) {
    return std::string(key) + be64_str(start_ts.value);
}
[[nodiscard]] inline std::string lock_key(std::string_view key) { return std::string(key); }
[[nodiscard]] inline std::string write_key(std::string_view key, Ts commit_ts) {
    return std::string(key) + be64_str(commit_ts.value);
}

class MvccTxn {
public:
    explicit MvccTxn(Storage& storage) : storage_(storage) {}

    // ---- 读（SPEC-4 §4.4 读路径 ①③；② 遇锁等待/决议由上层编排，此处直接报 LockConflict） ----
    [[nodiscard]] result<std::optional<std::string>> get(std::string_view key, Ts start_ts) {
        // ① 先看 Lock：自己的锁 → 读自己未提交数据；他者的锁 → LockConflict（上层等待/决议）
        auto lock = load_lock(key);
        if (lock.has_value() && lock->flag == kLockFlagActive) {
            if (lock->start_ts.value == start_ts.value) {
                return read_data(key, start_ts);
            }
            return std::unexpected(LockConflict{.key = std::string(key),
                                                .primary = lock->primary,
                                                .lock_start_ts = lock->start_ts.value});
        }
        // ③ 无锁：Write CF 找 commitTs ≤ start_ts 的最新版本
        auto write = find_write(key, start_ts);
        if (!write.has_value()) {
            return std::optional<std::string>{};   // 该时刻无已提交版本
        }
        return read_data(key, *write);
    }

    // ---- prewrite（§4.4：① 写写冲突立即 abort；② 锁冲突交给上层；③ 同一提案写 Default+Lock） ----
    [[nodiscard]] result<void> prewrite(std::span<const Mutation> mutations,
                                        std::string_view primary, Ts start_ts,
                                        std::uint64_t ttl_ms, std::uint64_t now_ms) {
        if (mutations.empty()) {
            return std::unexpected(InvalidArgument{"mutations 不得为空"});
        }
        const bool has_primary = std::any_of(
            mutations.begin(), mutations.end(),
            [&](const Mutation& m) { return m.key == primary; });
        if (!has_primary) {
            return std::unexpected(InvalidArgument{"primary 必须在 mutations 中（决议锚点）"});
        }
        // ①② 检查阶段（先查后写，失败立即 abort 自己——红线1）
        for (const auto& m : mutations) {
            if (m.key.empty()) {
                return std::unexpected(InvalidArgument{"空 key"});
            }
            // 写写冲突判据 = 该 key 最新 Write.commitTs > start_ts（本事务启动后有人提交过）；
            // 命中即立即 abort 自己，绝不等待他者（REL-06 无死锁的结构性保证）
            auto latest = latest_commit_bound(m.key);
            if (latest.has_value() && latest->value > start_ts.value) {
                return std::unexpected(WriteConflict{.key = m.key,
                                                     .start_ts = start_ts.value,
                                                     .conflict_commit_ts = latest->value});
            }
            auto lock = load_lock(m.key);
            if (lock.has_value() && lock->flag == kLockFlagActive &&
                lock->start_ts.value != start_ts.value) {
                return std::unexpected(LockConflict{.key = m.key,
                                                    .primary = lock->primary,
                                                    .lock_start_ts = lock->start_ts.value});
            }
        }
        // 精确的冲突 commitTs 需要二次定位（上面检查阶段已拦截，这里补字段供诊断）
        // ③ 写入：Default 数据 + Lock——同一 Storage 批（= 同一 raft 提案的原子性）
        std::vector<Modify> batch;
        batch.reserve(mutations.size() * 2);
        LockValue lv{.flag = kLockFlagActive,
                     .start_ts = start_ts,
                     .ttl_ms = ttl_ms,
                     .created_ms = now_ms,
                     .primary = std::string(primary)};
        const std::string lock_raw = encode_lock_value(lv);
        for (const auto& m : mutations) {
            if (m.op == Mutation::Op::PUT) {
                batch.push_back(Modify{Put{.cf = Cf::Default,
                                           .key = data_key(m.key, start_ts),
                                           .value = m.value}});
            } else {
                // Delete 的 prewrite：Default 写墓碑（空值）占位，commit 后 Write 指向它
                batch.push_back(Modify{Put{.cf = Cf::Default,
                                           .key = data_key(m.key, start_ts),
                                           .value = std::string{}}});
            }
            batch.push_back(Modify{Put{.cf = Cf::Lock, .key = lock_key(m.key), .value = lock_raw}});
        }
        return storage_.write(batch);
    }

    // ---- commit（§4.4：校验 Lock 归属 → 写 Write → 删 Lock，同一提案） ----
    [[nodiscard]] result<void> commit(std::span<const std::string> keys, Ts start_ts,
                                      Ts commit_ts) {
        // DEBUG
        if (keys.empty()) {
            return std::unexpected(InvalidArgument{"keys 不得为空"});
        }
        if (commit_ts.value <= start_ts.value) {
            return std::unexpected(InvalidArgument{"commit_ts 必须 > start_ts"});
        }
        std::vector<Modify> batch;
        batch.reserve(keys.size() * 2);
        for (const auto& key : keys) {
            auto lock = load_lock(key);
            if (!lock.has_value() || lock->flag != kLockFlagActive ||
                lock->start_ts.value != start_ts.value) {
                // 幂等重试：Write 已存在（同 commit_ts）→ 视为成功
                auto done = find_write_exact(key, commit_ts);
                if (done.has_value()) {
                    continue;
                }
                return std::unexpected(InvalidArgument{"commit: 锁不存在或归属不符（已终态）"});
            }
            batch.push_back(Modify{Put{.cf = Cf::Write,
                                       .key = write_key(key, commit_ts),
                                       .value = be64_str(start_ts.value)}});
            batch.push_back(Modify{Delete{.cf = Cf::Lock, .key = lock_key(key)}});
        }
        if (batch.empty()) {
            return {};   // 全部幂等命中
        }
        return storage_.write(batch);
    }

    // ---- rollback（§4.2.1：Put 墓碑到 Lock CF + 清 Default 未提交数据；幂等） ----
    [[nodiscard]] result<void> rollback(std::span<const std::string> keys, Ts start_ts) {
        if (keys.empty()) {
            return std::unexpected(InvalidArgument{"keys 不得为空"});
        }
        std::vector<Modify> batch;
        for (const auto& key : keys) {
            auto lock = load_lock(key);
            if (!lock.has_value()) {
                continue;   // 幂等：无锁（已终态或从未 prewrite）
            }
            if (lock->flag == kLockFlagActive && lock->start_ts.value != start_ts.value) {
                continue;   // 他者之锁，不动（primary 决议唯一，红线2）
            }
            if (lock->flag == kLockFlagActive) {
                batch.push_back(Modify{Delete{.cf = Cf::Default,
                                              .key = data_key(key, start_ts)}});
            }
            LockValue tombstone{.flag = kLockFlagRollback,
                                .start_ts = start_ts,
                                .ttl_ms = 0,
                                .created_ms = 0,
                                .primary = lock->primary};
            batch.push_back(
                Modify{Put{.cf = Cf::Lock, .key = lock_key(key), .value = encode_lock_value(tombstone)}});
        }
        if (batch.empty()) {
            return {};
        }
        return storage_.write(batch);
    }

    // ---- 决议入口（红线2/3：生死只由 primary 状态决定） ----
    [[nodiscard]] result<TxnStatus> check_txn_status(std::string_view primary, Ts lock_start_ts,
                                                     std::uint64_t now_ms) {
        TxnStatus out;
        // 已提交？
        auto committed_at = find_commit_ts_of(primary, lock_start_ts);
        if (committed_at.has_value()) {
            out.committed = true;
            out.commit_ts = *committed_at;
            return out;
        }
        auto lock = load_lock(primary);
        if (lock.has_value() && lock->flag == kLockFlagActive &&
            lock->start_ts.value == lock_start_ts.value) {
            const auto elapsed = now_ms >= lock->created_ms ? now_ms - lock->created_ms : 0;
            if (elapsed < lock->ttl_ms) {
                out.lock_ttl_remaining_ms = lock->ttl_ms - elapsed;
                return out;   // 未过期：调用方继续等待
            }
            return out;   // 过期：全零 = 可回滚（调用方执行 BatchRollback）
        }
        out.rolled_back = true;   // 无锁（墓碑或从未存在）→ 判回滚
        return out;
    }

private:
    [[nodiscard]] static std::uint64_t read_be64(std::string_view raw) {
        std::uint64_t v = 0;
        for (const char ch : raw) {
            v = (v << 8) | static_cast<std::uint8_t>(ch);
        }
        return v;
    }

    [[nodiscard]] std::optional<LockValue> load_lock(std::string_view key) const {
        auto r = storage_.get(Cf::Lock, as_bytes(key));
        if (!r.has_value() || !r->value.has_value()) {
            return std::nullopt;
        }
        auto lv = decode_lock_value(*r->value);
        return lv;
    }

    // 读 Default 某版本（编码键精确 get）
    [[nodiscard]] result<std::optional<std::string>> read_data(std::string_view key,
                                                               Ts version_ts) const {
        auto r = storage_.get(Cf::Default, as_bytes_bytes(data_key(key, version_ts)));
        if (!r.has_value() || !r->value.has_value() || r->value->empty()) {
            return std::optional<std::string>{};   // 不存在 / Delete 墓碑
        }
        return std::optional<std::string>{std::move(*r->value)};
    }

    // 版本归属判定：剥掉 8B ts 尾后按用户 key 精确等值——starts_with 会把 "k1" 的版本
    // 误配给 "k"（本层首跑踩坑），等值+有序迭代才是正确口径。
    template <typename Fn>
    void for_each_write_version(std::string_view key, Fn&& fn) const {
        auto reader = storage_.create_reader();
        if (!reader.has_value()) {
            return;
        }
        auto it = (*reader)->scan(Cf::Write, as_bytes_bytes(std::string(key)), kScanLimit);
        if (!it.has_value()) {
            return;
        }
        for (; (*it)->valid(); (*it)->next()) {
            const std::string stored((*it)->key());   // = user_key + ts 后缀（CF 前缀已剥）
            if (stored.size() < 8) {
                return;
            }
            const auto user = stored.substr(0, stored.size() - 8);
            if (user > key) {
                return;   // 升序迭代：已越过本 key 的版本区间
            }
            if (user != key) {
                continue;   // user < key：别的 key（key 是另一 key 的前缀时）
            }
            std::forward<Fn>(fn)(Ts{read_be64(stored.substr(stored.size() - 8))});
        }
    }

    // Write CF 找 commitTs ≤ start_ts 的最新版本 → 返回其数据版本（start_ts）
    [[nodiscard]] std::optional<Ts> find_write(std::string_view key, Ts start_ts) const {
        auto reader = storage_.create_reader();
        if (!reader.has_value()) {
            return std::nullopt;
        }
        std::optional<Ts> best;
        for_each_write_version(key, [&](Ts commit_ts) {
            if (commit_ts.value <= start_ts.value) {
                best = commit_ts;   // 升序扫：不断覆盖 → 最终即 ≤ start_ts 的最大者
            }
        });
        if (!best.has_value()) {
            return std::nullopt;
        }
        auto rec = (*reader)->get(Cf::Write, as_bytes_bytes(write_key(key, *best)));
        if (!rec.has_value() || !rec->value.has_value() || rec->value->size() < 8) {
            return std::nullopt;
        }
        return Ts{read_be64(*rec->value)};
    }

    // 精确位点存在性（幂等重试判定）
    [[nodiscard]] std::optional<Ts> find_write_exact(std::string_view key, Ts commit_ts) const {
        auto r = storage_.get(Cf::Write, as_bytes_bytes(write_key(key, commit_ts)));
        if (!r.has_value() || !r->value.has_value() || r->value->size() < 8) {
            return std::nullopt;
        }
        return Ts{read_be64(*r->value)};
    }

    // primary 在 lock_start_ts 的提交位点（决议用）
    [[nodiscard]] std::optional<Ts> find_commit_ts_of(std::string_view key,
                                                      Ts lock_start_ts) const {
        auto reader = storage_.create_reader();
        if (!reader.has_value()) {
            return std::nullopt;
        }
        std::optional<Ts> hit;
        for_each_write_version(key, [&](Ts commit_ts) {
            if (hit.has_value()) {
                return;
            }
            auto rec = (*reader)->get(Cf::Write, as_bytes_bytes(write_key(key, commit_ts)));
            if (rec.has_value() && rec->value.has_value() && rec->value->size() >= 8 &&
                read_be64(*rec->value) == lock_start_ts.value) {
                hit = commit_ts;
            }
        });
        return hit;
    }

    // 写写冲突检查用的边界（找到 > start_ts 的最小提交即冲突；简化：先查 ≤ start_ts 无意义，
    // 改为扫全部版本取最大 commitTs，> start_ts 即冲突）
    [[nodiscard]] std::optional<Ts> latest_commit_bound(std::string_view key) const {
        auto reader = storage_.create_reader();
        if (!reader.has_value()) {
            return std::nullopt;
        }
        auto it = (*reader)->scan(Cf::Write, as_bytes_bytes(std::string(key)), kScanLimit);
        if (!it.has_value()) {
            std::fprintf(stderr, "[dbg] find_write scan 失败\n");
            return std::nullopt;
        }
        std::optional<Ts> best;
        for (; (*it)->valid(); (*it)->next()) {
            const std::string stored((*it)->key());
            if (stored.size() < 8) {
                break;
            }
            const auto user = stored.substr(0, stored.size() - 8);
            if (user > key) {
                break;
            }
            if (user != key) {
                continue;
            }
            best = Ts{read_be64(stored.substr(stored.size() - 8))};
        }
        return best;
    }

    [[nodiscard]] static std::span<const std::byte> as_bytes(std::string_view s) {
        return std::span(reinterpret_cast<const std::byte*>(s.data()), s.size());
    }
    [[nodiscard]] static std::span<const std::byte> as_bytes_bytes(const std::string& s) {
        return std::span(reinterpret_cast<const std::byte*>(s.data()), s.size());
    }

    static constexpr std::size_t kScanLimit = 4096;   // 版本数课程级，GC 前有界
    Storage& storage_;
};

}  // namespace tinykv::txn
