// 文件用途：engine.h 的实现——恢复、写路径（WAL 先于内存表）、flush、快照 Reader、Scan 归并。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02、REL-02）。

#include "storage/engine.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

#include "common/encode.h"

namespace tinykv {
namespace {

constexpr const char* kManifestName = "MANIFEST";
constexpr const char* kManifestTmp = "MANIFEST.tmp";

// 用户 key + CF → 编码 key（非空校验由 encode_key 负责）
result<std::string> encode_user(Cf cf, std::string_view user) {
    return encode_key(cf, std::as_bytes(std::span{user.data(), user.size()}));
}

std::string sst_name(std::uint32_t seq) {
    std::string digits = std::to_string(seq);
    if (digits.size() < 6) {
        digits.insert(0, 6 - digits.size(), '0');
    }
    return "sst-" + digits + ".sst";
}

// 从文件名取序号：合并后名字不再连续，恢复时必须按最大序号而不是表数量续号
std::uint32_t sst_seq(const std::string& name) {
    constexpr std::string_view prefix = "sst-";
    if (!name.starts_with(prefix) || name.size() <= prefix.size()) {
        return 0;
    }
    const char* first = name.data() + prefix.size();
    const char* last = name.data() + name.size();
    std::uint32_t out = 0;
    const auto res = std::from_chars(first, last, out, 10);
    return res.ec == std::errc{} ? out : 0;
}

// 扫描结果迭代器：materialize 后按需前进（M1 不做流式，理由见 sst.h 头注）
class RangeIterator final : public Iterator {
public:
    explicit RangeIterator(std::vector<std::pair<std::string, std::string>> rows)
        : rows_(std::move(rows)) {}

    [[nodiscard]] bool valid() const override { return index_ < rows_.size(); }
    void next() override { ++index_; }
    [[nodiscard]] std::string_view key() const override {
        return valid() ? std::string_view(rows_[index_].first) : std::string_view{};
    }
    [[nodiscard]] std::string_view value() const override {
        return valid() ? std::string_view(rows_[index_].second) : std::string_view{};
    }

private:
    std::vector<std::pair<std::string, std::string>> rows_;
    std::size_t index_{0};
};

// 只读快照：持有共享的（写时复制）内存表 + 当时的 SST 列表，后续 flush/写不影响其结果集
// （SPEC-1 红线 3）。mem_ 用 shared_ptr<const>：Engine 写前会 COW，这里看到的永远是不可变视图。
class SnapshotReader final : public Reader {
public:
    SnapshotReader(std::shared_ptr<const MemTable> mem,
                   std::vector<std::shared_ptr<SstReader>> ssts)
        : mem_(std::move(mem)), ssts_(std::move(ssts)) {}

    [[nodiscard]] result<GetResult> get(Cf cf, std::span<const std::byte> key) const override {
        auto encoded = encode_key(cf, key);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        const auto hit = lookup(*encoded);
        if (!hit.has_value()) {
            return GetResult{.value = std::nullopt};
        }
        return GetResult{.value = *hit};
    }

    [[nodiscard]] result<std::unique_ptr<Iterator>> scan(
        Cf cf, std::span<const std::byte> start, std::size_t limit) const override {
        if (limit == 0) {
            return std::unexpected(InvalidArgument{"scan 的 limit 必须 > 0"});
        }
        auto lower = scan_lower_bound(cf, start);
        if (!lower) {
            return std::unexpected(lower.error());
        }
        // 流式归并：k 路游标取最小 key，命中 limit 即停，不再把全量数据 materialize 成 map。
        auto rows = merge_stream(cf, *lower, limit);
        return std::make_unique<RangeIterator>(std::move(rows));
    }

private:
    [[nodiscard]] result<std::string> scan_lower_bound(Cf cf,
                                                       std::span<const std::byte> start) const {
        if (start.empty()) {
            return std::string(1, static_cast<char>(cf));   // 该 CF 的起点
        }
        return encode_key(cf, start);
    }

    // k 路归并游标：SST 用下标 + 直接持有该表指针，内存表用 map 迭代器。
    // 封装成类是为了把判空/取值/前移从 merge_stream 移出——否则主函数会被 lambda 撑到
    // CI 的行数与复杂度上限。注意构造期就要能取 key（skip 循环），故必须直接持有表指针，
    // 不能"先构造再绑定"——那会在构造期解引用空指针（曾导致 CI 段错误）。
    class Cursor {
    public:
        Cursor() = default;
        // SST 源：lower_bound 定位后跳到本 CF 首条
        Cursor(const SstReader& sst, const std::string& lower, std::size_t source,
               const std::function<bool(std::string_view)>& in_range)
            : sst_(&sst), sst_count_(sst.entries().size()), from_mem_(false), source_(source) {
            const auto& entries = sst.entries();
            auto it = std::lower_bound(entries.begin(), entries.end(), lower,
                                       [](const SstEntry& e, const std::string& k) {
                                           return e.key < k;
                                       });
            sst_index_ = static_cast<std::size_t>(it - entries.begin());
            while (valid() && !in_range(key())) {
                ++sst_index_;
            }
        }
        // 内存表源：map.lower_bound 后跳到本 CF 首条
        Cursor(const MemTable::Map& map, const std::string& lower, std::size_t source,
               const std::function<bool(std::string_view)>& in_range)
            : mem_end_(map.end()), from_mem_(true), source_(source) {
            mem_it_ = map.lower_bound(lower);
            while (valid() && !in_range(key())) {
                ++mem_it_;
            }
        }

        [[nodiscard]] bool valid() const {
            return from_mem_ ? mem_it_ != mem_end_ : sst_index_ < sst_count_;
        }
        [[nodiscard]] std::string_view key() const {
            return from_mem_ ? std::string_view(mem_it_->first)
                             : std::string_view(sst_->entries()[sst_index_].key);
        }
        [[nodiscard]] const std::optional<std::string>& value() const {
            return from_mem_ ? mem_it_->second.value : sst_->entries()[sst_index_].value;
        }
        void advance(const std::function<bool(std::string_view)>& in_range) {
            if (from_mem_) {
                ++mem_it_;
            } else {
                ++sst_index_;
                while (valid() && !in_range(key())) {
                    ++sst_index_;
                }
            }
        }
        [[nodiscard]] std::size_t source() const { return source_; }

    private:
        const SstReader* sst_{nullptr};
        std::size_t sst_index_{0};
        std::size_t sst_count_{0};
        MemTable::Map::const_iterator mem_it_;
        MemTable::Map::const_iterator mem_end_;
        bool from_mem_{false};
        std::size_t source_{0};
    };

    [[nodiscard]] std::vector<Cursor> build_cursors(
        const std::string& lower, const std::function<bool(std::string_view)>& in_range) const {
        std::vector<Cursor> cursors;
        cursors.reserve(ssts_.size() + 1);
        for (std::size_t s = 0; s < ssts_.size(); ++s) {
            cursors.emplace_back(*ssts_[s], lower, s, in_range);
        }
        cursors.emplace_back(mem_->entries(), lower, ssts_.size(), in_range);
        return cursors;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> merge_stream(
        Cf cf, const std::string& lower, std::size_t limit) const {
        const char cf_byte = static_cast<char>(cf);
        const auto in_range = [&](std::string_view k) {
            return !k.empty() && k.front() == cf_byte && k >= std::string_view(lower);
        };
        auto cursors = build_cursors(lower, in_range);
        std::vector<std::pair<std::string, std::string>> rows;
        while (rows.size() < limit) {
            // 第一遍：找当前最小 key（跳过已耗尽或不在范围内的源）
            std::string_view best_key;
            for (const auto& c : cursors) {
                if (!c.valid() || !in_range(c.key())) {
                    continue;
                }
                if (best_key.empty() || c.key() < best_key) {
                    best_key = c.key();
                }
            }
            if (best_key.empty()) {
                break;   // 所有源耗尽
            }
            // 第二遍：同 key 取最新源（下标越大越新）；命中该 key 的源一起前移
            std::optional<std::string> chosen;
            for (auto& c : cursors) {
                if (!c.valid() || c.key() != best_key) {
                    continue;
                }
                chosen = c.value();   // 从小到大覆盖，最后（最新）胜出
                c.advance(in_range);
            }
            if (chosen.has_value()) {
                rows.emplace_back(std::string(best_key.substr(1)), *chosen);
            }
        }
        return rows;
    }

    [[nodiscard]] std::optional<std::optional<std::string>> lookup(
        std::string_view encoded) const {
        if (const MemValue* v = mem_->find(encoded); v != nullptr) {
            return v->value;
        }
        for (auto rit = ssts_.rbegin(); rit != ssts_.rend(); ++rit) {
            if (const SstEntry* e = (*rit)->find(encoded); e != nullptr) {
                return e->value;
            }
        }
        return std::nullopt;
    }

    std::shared_ptr<const MemTable> mem_;
    std::vector<std::shared_ptr<SstReader>> ssts_;
};

}  // namespace

Engine::Engine(EngineOptions opt) : opt_(std::move(opt)) {
    sst_dir_ = opt_.dir / "sst";
    wal_dir_ = opt_.dir / "wal";
    mem_ = std::make_shared<MemTable>();
}

result<std::vector<std::string>> Engine::load_manifest() const {
    std::vector<std::string> names;
    std::ifstream in(opt_.dir / kManifestName);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                names.push_back(line);
            }
        }
    }
    if (!names.empty()) {
        return names;
    }
    // 清单缺失或为空：回落到目录扫描。清单只是目录的缓存，sst/ 里的文件才是事实，
    // 这样即使清单改名窗口被打断也不会把已有表当成不存在（宁可多载，不可漏载）。
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(sst_dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sst") {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

result<void> Engine::write_manifest(const std::vector<std::string>& names) const {
    const auto tmp = opt_.dir / kManifestTmp;
    const auto dst = opt_.dir / kManifestName;
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) {
            return std::unexpected(IoFailed{.path = tmp.string(), .reason = "无法写入临时清单"});
        }
        for (const auto& n : names) {
            out << n << '\n';
        }
        out.flush();
        if (!out.good()) {
            return std::unexpected(IoFailed{.path = tmp.string(), .reason = "清单写入失败（磁盘满？）"});
        }
    }
    // BUG-0012：先删后改名会在中间留一个"清单不存在"的窗口（此前注释称 Windows 必须如此）。
    // 正确做法是先尝试原子改名（POSIX 覆盖语义），只有平台拒绝覆盖时才回退到"先删后改名"。
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(dst, ignored);
        std::filesystem::rename(tmp, dst, ec);
    }
    if (ec) {
        return std::unexpected(IoFailed{.path = dst.string(), .reason = "rename 清单失败: " + ec.message()});
    }
    return {};
}

result<void> Engine::recover() {
    std::error_code ec;
    std::filesystem::create_directories(sst_dir_, ec);
    std::filesystem::create_directories(wal_dir_, ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = opt_.dir.string(), .reason = "创建数据目录失败"});
    }

    auto names = load_manifest();
    if (!names) {
        return std::unexpected(names.error());
    }
    for (const auto& name : *names) {
        auto reader = SstReader::open(sst_dir_ / name);
        if (!reader) {
            return std::unexpected(reader.error());
        }
        ssts_.push_back(std::shared_ptr<SstReader>(std::move(*reader)));
        sst_names_.push_back(name);
        next_sst_seq_ = std::max(next_sst_seq_, sst_seq(name) + 1);
    }

    // 重放 WAL：只读，绝不 truncate（红线 1）
    auto replay = Wal::replay(wal_dir_);
    if (!replay) {
        return std::unexpected(replay.error());
    }
    for (const auto& batch : replay->batches) {
        if (auto r = apply_batch(batch); !r) {
            return r;
        }
    }

    wal_ = nullptr;
    auto wal = Wal::open(WalOptions{.dir = wal_dir_,
                                    .fsync_on_commit = opt_.fsync_on_commit,
                                    .segment_bytes = opt_.wal_segment_bytes});
    if (!wal) {
        return std::unexpected(wal.error());
    }
    wal_ = std::move(*wal);
    return {};
}

result<void> Engine::apply_batch(std::span<const Modify> batch) {
    MemTable& mem = mutable_mem_locked();
    for (const Modify& m : batch) {
        if (const auto* p = std::get_if<Put>(&m.op); p != nullptr) {
            auto enc = encode_user(p->cf, p->key);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            mem.put(std::move(*enc), p->value);
        } else {
            const auto& d = std::get<Delete>(m.op);
            auto enc = encode_user(d.cf, d.key);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            mem.put(std::move(*enc), std::nullopt);
        }
    }
    return {};
}

MemTable& Engine::mutable_mem_locked() {
    // 调用方持有 mu_：若有 Reader 持有旧 mem_ 快照，写前先复制，读侧继续看旧表。
    // 无快照时 use_count()==1，直接原地写，与优化前行为一致（零额外拷贝）。
    if (mem_.use_count() > 1) {
        mem_ = std::make_shared<MemTable>(*mem_);
    }
    return *mem_;
}

result<std::unique_ptr<Engine>> Engine::open(const EngineOptions& opt) {
    auto engine = std::unique_ptr<Engine>(new Engine(opt));  // NOLINT：私有构造，立即接管
    if (auto r = engine->recover(); !r) {
        return std::unexpected(r.error());
    }
    return engine;
}

result<GetResult> Engine::get(Cf cf, std::span<const std::byte> key) {
    auto encoded = encode_key(cf, key);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    std::lock_guard lock(mu_);
    if (closed_) {
        return std::unexpected(IoFailed{.path = opt_.dir.string(), .reason = "引擎已关闭"});
    }
    // 此处直接查内存表与 SST：与 Engine::lookup 语义一致，但按成员访问以避免额外拷贝
    if (const MemValue* v = mem_->find(*encoded); v != nullptr) {
        return GetResult{.value = v->value};
    }
    for (auto rit = ssts_.rbegin(); rit != ssts_.rend(); ++rit) {
        if (const SstEntry* e = (*rit)->find(*encoded); e != nullptr) {
            return GetResult{.value = e->value};
        }
    }
    return GetResult{.value = std::nullopt};
}

result<void> Engine::write(std::span<const Modify> batch) {
    if (batch.empty()) {
        return std::unexpected(InvalidArgument{"写入批次为空"});
    }
    std::lock_guard lock(mu_);
    if (closed_) {
        return std::unexpected(IoFailed{.path = opt_.dir.string(), .reason = "引擎已关闭"});
    }
    // code-review P0-5 第二道防线：flush 的换段窗口里 WAL 可能暂时为空（磁盘满/文件被占用）。
    // 没有这一行，一次失败的 flush 之后每次写都会解引用空指针，进程直接崩。
    if (wal_ == nullptr) {
        return std::unexpected(IoFailed{.path = opt_.dir.string(),
                                        .reason = "WAL 不可用（上次换段失败，需重开引擎）"});
    }
    // 先落 WAL 再改内存表：WAL 失败时内存表未被污染（REL-02）
    if (auto r = wal_->append(batch); !r) {
        return r;
    }
    if (auto r = apply_batch(batch); !r) {
        return r;
    }
    if (mem_->bytes() > opt_.memtable_limit_bytes) {
        return flush_locked();
    }
    return {};
}

result<std::unique_ptr<Reader>> Engine::create_reader() {
    std::lock_guard lock(mu_);
    // 共享快照：直接把当前 mem_ 交给 Reader（O(1)），后续写入由 mutable_mem_locked()
    // 在 use_count>1 时写时复制。SST 列表本身是 shared_ptr 且不可变，同样直接共享。
    return std::make_unique<SnapshotReader>(mem_, ssts_);
}

result<void> Engine::flush_locked() {
    if (mem_->empty()) {
        return {};
    }
    const std::string name = sst_name(next_sst_seq_);
    const auto path = sst_dir_ / name;
    auto writer = SstWriter::create(path);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    for (const auto& [key, value] : mem_->entries()) {
        if (auto r = (*writer)->add(key, value.value); !r) {
            return r;
        }
    }
    if (auto r = (*writer)->finish(); !r) {
        return r;
    }
    auto reader = SstReader::open(path);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    ssts_.push_back(std::shared_ptr<SstReader>(std::move(*reader)));
    sst_names_.push_back(name);   // 名字与表一一对应，不能按序号重建（合并后序号不连续）
    ++next_sst_seq_;

    if (auto r = write_manifest(sst_names_); !r) {
        return r;
    }

    // 清单已落盘 → 内存数据不再需要 WAL 重放，可清段；随后换新内存表与新段
    // 顺序要求：必须先关闭段文件句柄再删除——Windows 不允许删除仍被打开的文件
    // code-review P0-5：任何一步失败都不许留下 wal_ == nullptr 的状态；失败后要么重开成功，
    // 要么明确标记（write() 的判空兜底），绝不让下一次写入解引用空指针。
    if (auto r = rotate_wal_locked(); !r) {
        return r;
    }
    mem_ = std::make_shared<MemTable>();

    // 表数过阈值即合并：清单已落盘、WAL 已清段，合并失败只留下"表偏多"的可恢复状态
    if (opt_.sst_compact_threshold != 0 && ssts_.size() >= opt_.sst_compact_threshold) {
        return compact_ssts_locked();
    }
    return {};
}

// 换段：先关旧段（Windows 不允许删打开的文件）→ 清空目录 → 打开新段。
// code-review P0-5：旧实现先 `wal_.reset()` 再 clear_all/Wal::open，任一步失败就留下空指针，
// 下一次 write 直接解引用崩溃。这里把"失败也要可用"作为契约：清段失败时尝试重开，
// 重开也失败则置空并由 write() 的判空兜底返回 IoFailed（可恢复：重开引擎）。
result<void> Engine::rotate_wal_locked() {
    wal_.reset();
    if (auto r = Wal::clear_all(wal_dir_); !r) {
        (void)reopen_wal_locked();   // 尽力恢复；失败则留给 write() 的判空兜底
        return r;
    }
    return reopen_wal_locked();
}

result<void> Engine::reopen_wal_locked() {
    auto wal = Wal::open(WalOptions{.dir = wal_dir_,
                                    .fsync_on_commit = opt_.fsync_on_commit,
                                    .segment_bytes = opt_.wal_segment_bytes});
    if (!wal) {
        wal_ = nullptr;
        return std::unexpected(wal.error());
    }
    wal_ = std::move(*wal);
    return {};
}

// 全量合并：旧→新覆盖得到每个 key 的最终值；合并后没有更老的表，墓碑可以安全丢弃。
// 先写新表与清单，再删旧文件——中途崩溃只会留下未被清单引用的孤儿文件，不会丢数据。
result<void> Engine::compact_ssts_locked() {
    if (ssts_.size() < 2) {
        return {};
    }
    std::map<std::string, std::optional<std::string>, std::less<>> merged;
    for (const auto& sst : ssts_) {
        for (const auto& e : sst->entries()) {
            merged[e.key] = e.value;
        }
    }
    if (merged.empty()) {
        return {};
    }
    const auto old_names = sst_names_;
    const std::string name = sst_name(next_sst_seq_);
    auto writer = SstWriter::create(sst_dir_ / name);
    if (!writer) {
        return std::unexpected(writer.error());
    }
    for (const auto& [key, value] : merged) {
        if (!value.has_value()) {
            continue;   // 没有更老的表，墓碑无需保留
        }
        if (auto r = (*writer)->add(key, value); !r) {
            return r;
        }
    }
    if (auto r = (*writer)->finish(); !r) {
        return r;
    }
    auto reader = SstReader::open(sst_dir_ / name);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    ssts_.assign(1, std::shared_ptr<SstReader>(std::move(*reader)));
    sst_names_.assign(1, name);
    ++next_sst_seq_;
    if (auto r = write_manifest(sst_names_); !r) {
        return r;
    }
    std::error_code ec;
    for (const auto& old : old_names) {
        std::filesystem::remove(sst_dir_ / old, ec);   // 孤儿文件不影响恢复，删除失败不致命
    }
    return {};
}

result<void> Engine::close() {
    std::lock_guard lock(mu_);
    if (closed_) {
        return {};
    }
    if (auto r = flush_locked(); !r) {
        return r;
    }
    wal_.reset();
    closed_ = true;
    return {};
}

Engine::~Engine() {
    // 析构不抛：尽力 flush，失败时数据仍在 WAL 里，下次 open 会重放出来
    if (!closed_) {
        std::lock_guard lock(mu_);
        if (!closed_ && wal_ != nullptr && !mem_->empty()) {
            (void)flush_locked();
        }
    }
}

}  // namespace tinykv
