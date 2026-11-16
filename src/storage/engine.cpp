// 文件用途：engine.h 的实现——恢复、写路径（WAL 先于内存表）、flush、快照 Reader、Scan 归并。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02、REL-02）。

#include "storage/engine.h"

#include <algorithm>
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

// 只读快照：持有内存表副本 + 当时的 SST 列表，后续 flush 不影响其结果集（SPEC-1 红线 3）
class SnapshotReader final : public Reader {
public:
    SnapshotReader(std::vector<SstEntry> mem, std::vector<std::shared_ptr<SstReader>> ssts)
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
        auto merged = merge_cf(cf, *lower);
        std::vector<std::pair<std::string, std::string>> rows;
        for (auto it = merged.begin(); it != merged.end() && rows.size() < limit; ++it) {
            if (!it->second.has_value()) {
                continue;  // 墓碑：该 key 已被删除
            }
            rows.emplace_back(it->first.substr(1), *it->second);  // 剥掉 1 字节 CF 前缀
        }
        return std::make_unique<RangeIterator>(std::move(rows));
    }

private:
    [[nodiscard]] result<std::string> scan_lower_bound(Cf cf,
                                                       std::span<const std::byte> start) const {
        if (start.empty()) {
            return std::string(1, static_cast<char>(cf));  // 该 CF 的起点
        }
        return encode_key(cf, start);
    }

    // 旧源先写、新源后写覆盖：SST（旧→新）→ 内存表，得到"每 key 取最新"
    [[nodiscard]] std::map<std::string, std::optional<std::string>, std::less<>> merge_cf(
        Cf cf, std::string_view lower) const {
        const char cf_byte = static_cast<char>(cf);
        std::map<std::string, std::optional<std::string>, std::less<>> merged;
        const auto in_range = [&](const std::string& k) {
            return !k.empty() && k.front() == cf_byte && std::string_view(k) >= lower;
        };
        for (const auto& sst : ssts_) {
            for (const auto& e : sst->entries()) {
                if (in_range(e.key)) {
                    merged[e.key] = e.value;
                }
            }
        }
        for (const auto& e : mem_) {
            if (in_range(e.key)) {
                merged[e.key] = e.value;
            }
        }
        return merged;
    }

    [[nodiscard]] std::optional<std::optional<std::string>> lookup(
        std::string_view encoded) const {
        const auto it = std::lower_bound(
            mem_.begin(), mem_.end(), encoded,
            [](const SstEntry& e, std::string_view k) { return std::string_view(e.key) < k; });
        if (it != mem_.end() && std::string_view(it->key) == encoded) {
            return it->value;
        }
        for (auto rit = ssts_.rbegin(); rit != ssts_.rend(); ++rit) {
            if (const SstEntry* e = (*rit)->find(encoded); e != nullptr) {
                return e->value;
            }
        }
        return std::nullopt;
    }

    std::vector<SstEntry> mem_;
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
    }
    std::error_code ec;
    std::filesystem::remove(dst, ec);   // Windows 上 rename 不覆盖已存在文件
    std::filesystem::rename(tmp, dst, ec);
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
        next_sst_seq_ = static_cast<std::uint32_t>(ssts_.size()) + 1;
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
    for (const Modify& m : batch) {
        if (const auto* p = std::get_if<Put>(&m.op); p != nullptr) {
            auto enc = encode_user(p->cf, p->key);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            mem_->put(std::move(*enc), p->value);
        } else {
            const auto& d = std::get<Delete>(m.op);
            auto enc = encode_user(d.cf, d.key);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            mem_->put(std::move(*enc), std::nullopt);
        }
    }
    return {};
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
    std::vector<SstEntry> snapshot;
    snapshot.reserve(mem_->entries().size());
    for (const auto& [key, value] : mem_->entries()) {
        snapshot.push_back(SstEntry{.key = key, .value = value.value});
    }
    return std::make_unique<SnapshotReader>(std::move(snapshot), ssts_);
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
    ++next_sst_seq_;

    std::vector<std::string> names;
    names.reserve(ssts_.size());
    for (std::size_t i = 0; i < ssts_.size(); ++i) {
        names.push_back(sst_name(static_cast<std::uint32_t>(i + 1)));
    }
    if (auto r = write_manifest(names); !r) {
        return r;
    }

    // 清单已落盘 → 内存数据不再需要 WAL 重放，可清段；随后换新内存表与新段
    // 顺序要求：必须先关闭段文件句柄再删除——Windows 不允许删除仍被打开的文件
    wal_.reset();
    if (auto r = Wal::clear_all(wal_dir_); !r) {
        return r;
    }
    auto wal = Wal::open(WalOptions{.dir = wal_dir_,
                                    .fsync_on_commit = opt_.fsync_on_commit,
                                    .segment_bytes = opt_.wal_segment_bytes});
    if (!wal) {
        return std::unexpected(wal.error());
    }
    wal_ = std::move(*wal);
    mem_ = std::make_shared<MemTable>();
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
