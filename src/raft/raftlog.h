// 文件用途：Raft 日志——四指针模型（first/last/committed + dummy entry）与冲突截断（SPEC-2 §2.3.1）。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03）。
//
// 存储承载方式（决策，见 implementation-notes §3）：
//   raft log **直接存放在 M1 的 KV 引擎里**，而不是再造一套段文件 WAL。
//   理由：引擎的 WAL 已经把"追加即持久 + CRC + 崩溃重放"做到 REL-02 强度（并有 1000 条随机写
//   回归用例），raft log 需要的"按 index 取、按 index 截断、压缩掉旧段"在 KV 抽象上是三个
//   普通操作，自己再写一套段管理只会多一处崩溃窗口。这与 TiKV 把 raft log 存 RocksDB 同路。
//   代价：raft log 与业务数据共享一个引擎的 flush 策略，后续若要分离需引入独立引擎实例。
//
// key 布局（cf = Default，前缀保证有序）：
//   "\0re" + be64(index) → 序列化的 raftpb.Entry（大端 index 保证 Scan 序 = index 序）
//   "\0lm"               → 元信息：first/last/committed/term/vote/dummy_term 六个 u64

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <raft.pb.h>

#include "common/error.h"
#include "common/ids.h"
#include "storage/storage.h"

namespace tinykv::raft {

namespace raftpb = ::tinykv::raftpb;
using Entry = raftpb::Entry;

class RaftLog {
public:
    // 打开（或初始化）日志。空日志的初始状态：dummy entry 在 index 0 / term 0，
    // first_index 返回 1（首条可读位置），last_index 返回 0（尚无条目）。
    [[nodiscard]] static result<std::unique_ptr<RaftLog>> open(Storage& storage);

    [[nodiscard]] LogIndex first_index() const { return LogIndex{meta_.first_index}; }
    [[nodiscard]] LogIndex last_index() const { return LogIndex{meta_.last_index}; }
    [[nodiscard]] LogIndex committed() const { return LogIndex{meta_.committed}; }
    // commit 必须持久化：重启后若重置为 0，已提交但未 apply 的条目会被当成未提交
    [[nodiscard]] result<void> set_committed(LogIndex idx);

    [[nodiscard]] Term current_term() const { return Term{meta_.term}; }
    [[nodiscard]] std::optional<PeerId> voted_for() const {
        return meta_.vote == 0 ? std::nullopt : std::optional<PeerId>(PeerId{meta_.vote});
    }
    [[nodiscard]] Term dummy_term() const { return Term{meta_.dummy_term}; }

    // 任期查询：index==0 返回 dummy term；小于 first_index 说明已被压缩 → Corrupted
    [[nodiscard]] result<Term> term(LogIndex idx) const;
    [[nodiscard]] result<Entry> at(LogIndex idx) const;
    // 取 [lo, hi) 的条目；跨出可读范围返回 InvalidArgument
    [[nodiscard]] result<std::vector<Entry>> slice(LogIndex lo, LogIndex hi) const;

    [[nodiscard]] result<void> append(std::span<const Entry> entries);
    // 删除 index >= idx 的条目（冲突覆盖）；idx 必须 > committed
    [[nodiscard]] result<void> truncate_from(LogIndex idx);
    // 压缩 index <= upto 的条目（已被快照覆盖）
    [[nodiscard]] result<void> compact(LogIndex upto);

    [[nodiscard]] result<void> set_hard_state(Term term, std::optional<PeerId> vote);

private:
    struct Meta {
        std::uint64_t first_index{1};
        std::uint64_t last_index{0};
        std::uint64_t committed{0};
        std::uint64_t term{0};
        std::uint64_t vote{0};
        std::uint64_t dummy_term{0};
    };

    explicit RaftLog(Storage& storage) : storage_(storage) {}

    [[nodiscard]] result<void> persist_meta();
    [[nodiscard]] result<void> remove_range(LogIndex lo, LogIndex hi);  // 删 [lo, hi]

    Storage& storage_;
    Meta meta_;
};

}  // namespace tinykv::raft
