// 文件用途：raftlog.h 的实现——元信息持久化、条目按 index 读写、截断与压缩。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03）。

#include "raft/raftlog.h"

#include <array>
#include <utility>

namespace tinykv::raft {
namespace {

constexpr std::size_t kMetaBytes = 6 * 8;  // 六个 u64
constexpr const char kMetaKey[] = "\0lm";
constexpr std::size_t kMetaKeyLen = 3;
constexpr const char kAppliedKey[] = "\0la";
constexpr std::size_t kAppliedKeyLen = 3;
constexpr const char kEntryPrefix[] = "\0re";
constexpr std::size_t kEntryPrefixLen = 3;

std::string entry_key(std::uint64_t index) {
    std::string key(kEntryPrefix, kEntryPrefixLen);
    for (int shift = 56; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((index >> shift) & 0xFFU));  // 大端：Scan 序 = index 序
    }
    return key;
}

std::string meta_key() {
    return std::string(kMetaKey, kMetaKeyLen);
}
std::string applied_key() {
    return std::string(kAppliedKey, kAppliedKeyLen);
}

tinykv::span<const std::byte> as_bytes(const std::string& s) {
    return tinykv::as_bytes(tinykv::span{s.data(), s.size()});
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {  // 小端足够：仅本模块自读自写
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

std::uint64_t read_u64(const std::string& s, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(s[off + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

}  // namespace

result<std::unique_ptr<RaftLog>> RaftLog::open(Storage& storage) {
    auto log = std::unique_ptr<RaftLog>(new RaftLog(storage));  // NOLINT：构造私有，立即接管

    auto raw = storage.get(Cf::Default, as_bytes(meta_key()));
    if (!raw) {
        return tinykv::unexpected(raw.error());
    }
    if (raw->value.has_value() && raw->value->size() == kMetaBytes) {
        const std::string& m = *raw->value;
        log->meta_ = Meta{.first_index = read_u64(m, 0),
                          .last_index = read_u64(m, 8),
                          .committed = read_u64(m, 16),
                          .term = read_u64(m, 24),
                          .vote = read_u64(m, 32),
                          .dummy_term = read_u64(m, 40)};
    }
    // applied 独立键：老库没有该键 → 保持 0，重启后走 (0, committed] 重放（至少一次）
    auto applied = storage.get(Cf::Default, as_bytes(applied_key()));
    if (!applied) {
        return tinykv::unexpected(applied.error());
    }
    if (applied->value.has_value() && applied->value->size() == 8) {
        log->meta_.applied = read_u64(*applied->value, 0);
    }
    // 元信息缺失（全新日志）时保持默认值且**不写盘**：启动路径不做任何写动作（SPEC-1 红线 1 同源）
    return log;
}

ModifyVec RaftLog::meta_modifies(const Meta& m) {
    std::string buf;
    buf.reserve(kMetaBytes);
    put_u64(buf, m.first_index);
    put_u64(buf, m.last_index);
    put_u64(buf, m.committed);
    put_u64(buf, m.term);
    put_u64(buf, m.vote);
    put_u64(buf, m.dummy_term);
    std::string applied_buf;
    applied_buf.reserve(8);
    put_u64(applied_buf, m.applied);
    // 两条 Put 同批：快照重置位点与 meta 一起落盘，避免"meta 已改、applied 还是旧值"的窗口
    return ModifyVec{
        Modify{Put{.cf = Cf::Default, .key = meta_key(), .value = std::move(buf)}},
        Modify{Put{.cf = Cf::Default, .key = applied_key(), .value = std::move(applied_buf)}}};
}

result<void> RaftLog::persist_meta() {
    return storage_.write(meta_modifies(meta_));
}

// BUG-0014：条目变更与 meta 必须同一批落盘。引擎的 write 是原子的，分两次写会留下
// "条目已删、last_index/first_index 还是旧值"的崩溃窗口——重启后 at(last_index) 失败，
// 恢复路径直接起不来。
result<void> RaftLog::write_with_meta(ModifyVec batch, const Meta& next) {
    auto meta_batch = meta_modifies(next);
    batch.insert(batch.end(), std::make_move_iterator(meta_batch.begin()),
                 std::make_move_iterator(meta_batch.end()));
    if (auto r = storage_.write(batch); !r) {
        return r;  // 失败：meta_ 未动（只在成功后提交），内存与盘上仍一致
    }
    meta_ = next;
    return {};
}

result<void> RaftLog::set_applied(LogIndex idx) {
    meta_.applied = idx.value;
    return persist_meta();
}

result<Term> RaftLog::term(LogIndex idx) const {
    if (idx.value == 0) {
        return Term{meta_.dummy_term};  // dummy entry 的任期
    }
    if (idx.value < meta_.first_index) {
        return tinykv::unexpected(
            Corrupted{.where = "raftlog:term",
                      .reason = "index " + std::to_string(idx.value) +
                                " 已被压缩（first=" + std::to_string(meta_.first_index) + "）"});
    }
    auto entry = at(idx);
    if (!entry) {
        return tinykv::unexpected(entry.error());
    }
    return Term{entry->term()};
}

result<Entry> RaftLog::at(LogIndex idx) const {
    if (idx.value == 0) {
        Entry dummy;
        dummy.set_index(0);
        dummy.set_term(meta_.dummy_term);
        return dummy;
    }
    const auto raw = storage_.get(Cf::Default, as_bytes(entry_key(idx.value)));
    if (!raw) {
        return tinykv::unexpected(raw.error());
    }
    if (!raw->value.has_value()) {
        return tinykv::unexpected(
            InvalidArgument{"entry 不存在：index " + std::to_string(idx.value)});
    }
    Entry entry;
    if (!entry.ParseFromString(*raw->value)) {
        return tinykv::unexpected(Corrupted{.where = "raftlog:at", .reason = "entry 反序列化失败"});
    }
    return entry;
}

result<std::vector<Entry>> RaftLog::slice(LogIndex lo, LogIndex hi) const {
    std::vector<Entry> out;
    if (lo.value > hi.value) {
        return tinykv::unexpected(InvalidArgument{"slice 的 lo 大于 hi"});
    }
    if (lo.value != 0 && lo.value < meta_.first_index) {
        return tinykv::unexpected(Corrupted{.where = "raftlog:slice", .reason = "lo 已被压缩"});
    }
    if (hi.value > meta_.last_index + 1) {
        return tinykv::unexpected(InvalidArgument{"slice 的 hi 越过 last_index"});
    }
    out.reserve(static_cast<std::size_t>(hi.value - lo.value));
    for (std::uint64_t i = lo.value; i < hi.value; ++i) {
        auto entry = at(LogIndex{i});
        if (!entry) {
            return tinykv::unexpected(entry.error());
        }
        out.push_back(std::move(*entry));
    }
    return out;
}

result<void> RaftLog::append(tinykv::span<const Entry> entries) {
    if (entries.empty()) {
        return {};
    }
    if (entries.front().index() != meta_.last_index + 1) {
        return tinykv::unexpected(InvalidArgument{"append 必须紧接 last_index：期望 " +
                                                  std::to_string(meta_.last_index + 1) + "，实际 " +
                                                  std::to_string(entries.front().index())});
    }
    ModifyVec batch;
    batch.reserve(entries.size());
    std::uint64_t expected = meta_.last_index + 1;
    for (const Entry& e : entries) {
        if (e.index() != expected) {
            return tinykv::unexpected(InvalidArgument{"append 的 index 不连续"});
        }
        std::string serialized;
        if (!e.SerializeToString(&serialized)) {
            return tinykv::unexpected(
                Corrupted{.where = "raftlog:append", .reason = "entry 序列化失败"});
        }
        batch.push_back(Modify{
            Put{.cf = Cf::Default, .key = entry_key(e.index()), .value = std::move(serialized)}});
        ++expected;
    }
    Meta next = meta_;
    next.last_index = expected - 1;
    return write_with_meta(std::move(batch), next);
}

ModifyVec RaftLog::range_modifies(LogIndex lo, LogIndex hi) {
    ModifyVec batch;
    if (lo.value >= hi.value) {
        return batch;
    }
    batch.reserve(static_cast<std::size_t>(hi.value - lo.value));
    for (std::uint64_t i = lo.value; i < hi.value; ++i) {
        batch.push_back(Modify{Delete{.cf = Cf::Default, .key = entry_key(i)}});
    }
    return batch;
}

result<void> RaftLog::remove_range(LogIndex lo, LogIndex hi) {
    if (lo.value >= hi.value) {
        return {};
    }
    return storage_.write(range_modifies(lo, hi));
}

result<void> RaftLog::truncate_from(LogIndex idx) {
    if (idx.value <= meta_.committed) {
        return tinykv::unexpected(
            InvalidArgument{"禁止截断已提交条目：idx=" + std::to_string(idx.value) +
                            " committed=" + std::to_string(meta_.committed)});
    }
    if (idx.value > meta_.last_index) {
        return {};  // 无事可做
    }
    auto deletes = range_modifies(idx, LogIndex{meta_.last_index + 1});
    Meta next = meta_;
    next.last_index = idx.value - 1;
    return write_with_meta(std::move(deletes), next);
}

result<void> RaftLog::compact(LogIndex upto) {
    if (upto.value < meta_.first_index || upto.value > meta_.last_index) {
        return tinykv::unexpected(InvalidArgument{"compact 位点越界"});
    }
    if (upto.value > meta_.applied) {
        return tinykv::unexpected(InvalidArgument{"compact 不得越过 applied（SPEC-2 红线 9）"});
    }
    // 压缩后 dummy entry 的任期取被压缩的最后一条，保证后续 term(idx) 与 prevLogTerm 判定正确
    auto last_compacted = at(upto);
    if (!last_compacted) {
        return tinykv::unexpected(last_compacted.error());
    }
    auto deletes = range_modifies(LogIndex{meta_.first_index}, LogIndex{upto.value + 1});
    Meta next = meta_;
    next.dummy_term = last_compacted->term();
    next.first_index = upto.value + 1;
    return write_with_meta(std::move(deletes), next);
}

result<void> RaftLog::set_committed(LogIndex idx) {
    if (idx.value < meta_.committed) {
        return tinykv::unexpected(InvalidArgument{"commit 不可回退"});
    }
    meta_.committed = idx.value;
    return persist_meta();
}

result<void> RaftLog::reset_snapshot(LogIndex index, Term term) {
    if (index.value < meta_.first_index - 1) {
        return tinykv::unexpected(InvalidArgument{"快照位点早于已知起点，拒绝回退"});
    }
    auto deletes = range_modifies(LogIndex{meta_.first_index}, LogIndex{meta_.last_index + 1});
    Meta next = meta_;
    next.first_index = index.value + 1;
    next.last_index = index.value;
    next.committed = index.value;
    next.dummy_term = term.value;
    // 快照覆盖到 index：旧日志已不可用，applied 必须同批前移，否则重启会从 0 重放已压缩条目
    next.applied = index.value;
    return write_with_meta(std::move(deletes), next);
}

result<void> RaftLog::set_hard_state(Term term, std::optional<PeerId> vote) {
    meta_.term = term.value;
    meta_.vote = vote ? vote->value : 0;
    return persist_meta();
}

}  // namespace tinykv::raft
