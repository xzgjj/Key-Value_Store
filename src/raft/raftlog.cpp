// 文件用途：raftlog.h 的实现——元信息持久化、条目按 index 读写、截断与压缩。
// 归属：TinyKV-CPP（M2 Raft 共识，FUNC-03）。

#include "raft/raftlog.h"

#include <array>
#include <utility>

namespace tinykv::raft {
namespace {

constexpr std::size_t kMetaBytes = 6 * 8;   // 六个 u64
constexpr const char kMetaKey[] = "\0lm";
constexpr std::size_t kMetaKeyLen = 3;
constexpr const char kEntryPrefix[] = "\0re";
constexpr std::size_t kEntryPrefixLen = 3;
constexpr std::size_t kIndexBytes = 8;

std::string entry_key(std::uint64_t index) {
    std::string key(kEntryPrefix, kEntryPrefixLen);
    for (int shift = 56; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((index >> shift) & 0xFFU));   // 大端：Scan 序 = index 序
    }
    return key;
}

std::string meta_key() { return std::string(kMetaKey, kMetaKeyLen); }

std::span<const std::byte> as_bytes(const std::string& s) {
    return std::as_bytes(std::span{s.data(), s.size()});
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {   // 小端足够：仅本模块自读自写
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

std::uint64_t read_u64(const std::string& s, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(s[off + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

}  // namespace

result<std::unique_ptr<RaftLog>> RaftLog::open(Storage& storage) {
    auto log = std::unique_ptr<RaftLog>(new RaftLog(storage));   // NOLINT：构造私有，立即接管

    auto raw = storage.get(Cf::Default, as_bytes(meta_key()));
    if (!raw) {
        return std::unexpected(raw.error());
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
    // 元信息缺失（全新日志）时保持默认值且**不写盘**：启动路径不做任何写动作（SPEC-1 红线 1 同源）
    return log;
}

result<void> RaftLog::persist_meta() {
    std::string buf;
    buf.reserve(kMetaBytes);
    put_u64(buf, meta_.first_index);
    put_u64(buf, meta_.last_index);
    put_u64(buf, meta_.committed);
    put_u64(buf, meta_.term);
    put_u64(buf, meta_.vote);
    put_u64(buf, meta_.dummy_term);
    ModifyVec batch{Modify{Put{.cf = Cf::Default, .key = meta_key(), .value = std::move(buf)}}};
    return storage_.write(batch);
}

result<Term> RaftLog::term(LogIndex idx) const {
    if (idx.value == 0) {
        return Term{meta_.dummy_term};   // dummy entry 的任期
    }
    if (idx.value < meta_.first_index) {
        return std::unexpected(Corrupted{
            .where = "raftlog:term",
            .reason = "index " + std::to_string(idx.value) + " 已被压缩（first=" +
                      std::to_string(meta_.first_index) + "）"});
    }
    auto entry = at(idx);
    if (!entry) {
        return std::unexpected(entry.error());
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
        return std::unexpected(raw.error());
    }
    if (!raw->value.has_value()) {
        return std::unexpected(InvalidArgument{"entry 不存在：index " + std::to_string(idx.value)});
    }
    Entry entry;
    if (!entry.ParseFromString(*raw->value)) {
        return std::unexpected(Corrupted{.where = "raftlog:at", .reason = "entry 反序列化失败"});
    }
    return entry;
}

result<std::vector<Entry>> RaftLog::slice(LogIndex lo, LogIndex hi) const {
    std::vector<Entry> out;
    if (lo.value > hi.value) {
        return std::unexpected(InvalidArgument{"slice 的 lo 大于 hi"});
    }
    if (lo.value != 0 && lo.value < meta_.first_index) {
        return std::unexpected(Corrupted{.where = "raftlog:slice", .reason = "lo 已被压缩"});
    }
    if (hi.value > meta_.last_index + 1) {
        return std::unexpected(InvalidArgument{"slice 的 hi 越过 last_index"});
    }
    out.reserve(static_cast<std::size_t>(hi.value - lo.value));
    for (std::uint64_t i = lo.value; i < hi.value; ++i) {
        auto entry = at(LogIndex{i});
        if (!entry) {
            return std::unexpected(entry.error());
        }
        out.push_back(std::move(*entry));
    }
    return out;
}

result<void> RaftLog::append(std::span<const Entry> entries) {
    if (entries.empty()) {
        return {};
    }
    if (entries.front().index() != meta_.last_index + 1) {
        return std::unexpected(InvalidArgument{
            "append 必须紧接 last_index：期望 " + std::to_string(meta_.last_index + 1) +
            "，实际 " + std::to_string(entries.front().index())});
    }
    ModifyVec batch;
    batch.reserve(entries.size());
    std::uint64_t expected = meta_.last_index + 1;
    for (const Entry& e : entries) {
        if (e.index() != expected) {
            return std::unexpected(InvalidArgument{"append 的 index 不连续"});
        }
        std::string serialized;
        if (!e.SerializeToString(&serialized)) {
            return std::unexpected(Corrupted{.where = "raftlog:append", .reason = "entry 序列化失败"});
        }
        batch.push_back(Modify{Put{.cf = Cf::Default,
                                   .key = entry_key(e.index()),
                                   .value = std::move(serialized)}});
        ++expected;
    }
    if (auto r = storage_.write(batch); !r) {
        return r;
    }
    meta_.last_index = expected - 1;
    return persist_meta();
}

result<void> RaftLog::remove_range(LogIndex lo, LogIndex hi) {
    if (lo.value >= hi.value) {
        return {};
    }
    ModifyVec batch;
    batch.reserve(static_cast<std::size_t>(hi.value - lo.value));
    for (std::uint64_t i = lo.value; i < hi.value; ++i) {
        batch.push_back(Modify{Delete{.cf = Cf::Default, .key = entry_key(i)}});
    }
    return storage_.write(batch);
}

result<void> RaftLog::truncate_from(LogIndex idx) {
    if (idx.value <= meta_.committed) {
        return std::unexpected(InvalidArgument{
            "禁止截断已提交条目：idx=" + std::to_string(idx.value) +
            " committed=" + std::to_string(meta_.committed)});
    }
    if (idx.value > meta_.last_index) {
        return {};   // 无事可做
    }
    if (auto r = remove_range(idx, LogIndex{meta_.last_index + 1}); !r) {
        return r;
    }
    meta_.last_index = idx.value - 1;
    return persist_meta();
}

result<void> RaftLog::compact(LogIndex upto) {
    if (upto.value < meta_.first_index || upto.value > meta_.last_index) {
        return std::unexpected(InvalidArgument{"compact 位点越界"});
    }
    if (upto.value > meta_.committed) {
        return std::unexpected(InvalidArgument{"compact 不得越过 committed（SPEC-2 红线 9）"});
    }
    // 压缩后 dummy entry 的任期取被压缩的最后一条，保证后续 term(idx) 与 prevLogTerm 判定正确
    auto last_compacted = at(upto);
    if (!last_compacted) {
        return std::unexpected(last_compacted.error());
    }
    if (auto r = remove_range(LogIndex{meta_.first_index}, LogIndex{upto.value + 1}); !r) {
        return r;
    }
    meta_.dummy_term = last_compacted->term();
    meta_.first_index = upto.value + 1;
    return persist_meta();
}

result<void> RaftLog::set_committed(LogIndex idx) {
    if (idx.value < meta_.committed) {
        return std::unexpected(InvalidArgument{"commit 不可回退"});
    }
    meta_.committed = idx.value;
    return persist_meta();
}

result<void> RaftLog::set_hard_state(Term term, std::optional<PeerId> vote) {
    meta_.term = term.value;
    meta_.vote = vote ? vote->value : 0;
    return persist_meta();
}

}  // namespace tinykv::raft
