// 文件用途：wal.h 的实现——段文件追加、CRC32C 校验、只读重放、坏记录即停。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-02 / REL-02）。

#include "storage/wal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "common/crc32c.h"
#include "common/varint.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tinykv {
namespace {

constexpr std::size_t kHeaderBytes = Wal::kHeaderBytes;              // [u32 len][u32 crc]
constexpr std::size_t kMaxRecordBytes = Wal::kMaxRecordBytes;        // 单条上限：防坏长度触发巨量分配
constexpr std::size_t kSegmentNameWidth = 6;

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t get_u32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

// 载荷布局：[varint 条数] 后逐条 [kind][cf][varint klen][key][varint vlen][value(仅 put)]
void encode_batch(tinykv::span<const Modify> batch, std::string& out) {
    put_varint(out, batch.size());
    for (const Modify& m : batch) {
        if (const auto* p = std::get_if<Put>(&m.op); p != nullptr) {
            out.push_back(0);  // kind: put
            out.push_back(static_cast<char>(p->cf));
            put_varint(out, p->key.size());
            out.append(p->key);
            put_varint(out, p->value.size());
            out.append(p->value);
        } else {
            const auto& d = std::get<Delete>(m.op);
            out.push_back(1);  // kind: delete（墓碑）
            out.push_back(static_cast<char>(d.cf));
            put_varint(out, d.key.size());
            out.append(d.key);
            put_varint(out, 0);  // 墓碑无 value
        }
    }
}

bool take_bytes(std::string_view in, std::size_t& pos, std::uint64_t n, std::string& out) {
    if (n > in.size() - pos) {
        return false;
    }
    out.assign(in.substr(pos, static_cast<std::size_t>(n)));
    pos += static_cast<std::size_t>(n);
    return true;
}

bool decode_batch(std::string_view payload, ModifyVec& out) {
    std::size_t pos = 0;
    std::uint64_t count = 0;
    if (!get_varint(payload, pos, count) || count > payload.size()) {
        return false;  // 条数不可能超过载荷字节数，越界即坏数据
    }
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        if (pos + 2 > payload.size()) {
            return false;
        }
        const auto kind = static_cast<std::uint8_t>(payload[pos]);
        const auto cf = static_cast<Cf>(static_cast<std::uint8_t>(payload[pos + 1]));
        pos += 2;
        std::uint64_t klen = 0;
        std::string key;
        if (!get_varint(payload, pos, klen) || !take_bytes(payload, pos, klen, key)) {
            return false;
        }
        std::uint64_t vlen = 0;
        std::string value;
        if (!get_varint(payload, pos, vlen) || !take_bytes(payload, pos, vlen, value)) {
            return false;
        }
        if (kind == 0) {
            out.push_back(Modify{Put{.cf = cf, .key = std::move(key), .value = std::move(value)}});
        } else {
            out.push_back(Modify{Delete{.cf = cf, .key = std::move(key)}});
        }
    }
    return pos == payload.size();  // 尾随垃圾视为损坏
}

std::vector<std::pair<std::uint32_t, std::filesystem::path>> list_segments(
    const std::filesystem::path& dir) {
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> segs;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return segs;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto path = entry.path();
        const auto name = path.filename().string();
        if (name.rfind("wal-", 0) != 0 || path.extension() != ".log") {
            continue;
        }
        const auto digits = name.substr(4, name.size() - 4 - 4);
        try {
            segs.emplace_back(static_cast<std::uint32_t>(std::stoul(digits)), path);
        } catch (const std::exception&) {
            continue;  // 命名不合规的文件不是本模块的段，忽略
        }
    }
    std::sort(segs.begin(), segs.end());
    return segs;
}

int raw_fsync(std::FILE* f) {
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return ::fdatasync(::fileno(f));
#endif
}

std::string segment_name(std::uint32_t index) {
    std::string digits = std::to_string(index);
    if (digits.size() < kSegmentNameWidth) {
        digits.insert(0, kSegmentNameWidth - digits.size(), '0');
    }
    return "wal-" + digits + ".log";
}

}  // namespace

Wal::Wal(std::filesystem::path dir, bool fsync_on_commit, std::size_t segment_bytes,
         std::chrono::milliseconds commit_window)
    : dir_(std::move(dir)),
      fsync_on_commit_(fsync_on_commit),
      segment_bytes_(segment_bytes),
      commit_window_(commit_window) {
}

Wal::~Wal() {
    // 析构不抛（code-review）：join 可能抛 system_error，sync_locked 的错误路径会构造
    // IoFailed 字符串——任何一个穿出都会 std::terminate。
    try {
        // 先停后台提交者并等待其退出（绝不 detach），再把窗口内残留的写强制刷盘
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(commit_mu_);  // 与提交者的 wait 用同一把锁，唤醒不丢
        }
        cv_.notify_all();
        if (committer_.joinable()) {
            committer_.join();
        }
        if (file_ != nullptr) {
            (void)sync_locked();  // 析构尽力落盘；错误无法向调用者报告，但不影响 fclose
            std::fclose(file_);
            file_ = nullptr;
        }
    } catch (...) {
        // 见上：尽力而为，绝不从析构抛出
    }
}

void Wal::commit_loop() {
    // 窗口等待用独立的 commit_mu_，**不占用写路径的 mu_**：否则 append 每批都被锁竞争拖住，
    // 写入被后台攒批饿死（实测 B 场景 fsync=off 掉到 3/s）。只有真正 fsync 的瞬间才短暂持 mu_。
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(commit_mu_);
            cv_.wait(lock, [this] {
                return dirty_.load(std::memory_order_acquire) ||
                       stop_.load(std::memory_order_acquire);
            });
            if (stop_.load(std::memory_order_acquire) && !dirty_.load(std::memory_order_acquire)) {
                return;  // 停止且无残留：退出
            }
            if (!stop_.load(std::memory_order_acquire)) {
                cv_.wait_for(lock, commit_window_,
                             [this] { return stop_.load(std::memory_order_acquire); });
            }
        }
        if (!dirty_.load(std::memory_order_acquire)) {
            continue;
        }
        // 尊重 fsync_on_commit_：关掉时只 fflush（数据进页缓存），不做真 fsync
        std::lock_guard<std::mutex> lock(mu_);
        if (dirty_.load(std::memory_order_acquire)) {
            // code-review P1-1：后台落盘失败不能只 continue 空转——已向调用方确认的写会永久丢失，
            // 而错误无人可见。改为粘性失败状态：后续 append 一律返回 IoFailed，由上层停止接客。
            if (std::fflush(file_) != 0) {
                mark_failed_locked("fflush 失败（后台提交者）");
                continue;
            }
            if (fsync_on_commit_ && raw_fsync(file_) != 0) {
                mark_failed_locked("fsync 失败（后台提交者）");
                continue;
            }
            fsync_count_.fetch_add(1, std::memory_order_relaxed);
            dirty_.store(false, std::memory_order_release);
        }
    }
}

void Wal::mark_failed_locked(std::string_view reason) {
    if (!failed_.exchange(true, std::memory_order_acq_rel)) {
        fail_reason_ = std::string(reason);
    }
    dirty_.store(false, std::memory_order_release);  // 停止空转重试（错误已粘住，重试无意义）
}

result<void> Wal::open_new_segment() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
        ++next_index_;
    }
    current_ = dir_ / segment_name(next_index_);
    file_ = std::fopen(current_.string().c_str(), "ab");
    if (file_ == nullptr) {
        return tinykv::unexpected(IoFailed{.path = current_.string(), .reason = "fopen(ab) 失败"});
    }
    std::error_code ec;
    current_size_ = std::filesystem::file_size(current_, ec);
    if (ec) {
        current_size_ = 0;
    }
    return {};
}

result<std::unique_ptr<Wal>> Wal::open(const WalOptions& opt) {
    std::error_code ec;
    std::filesystem::create_directories(opt.dir, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = opt.dir.string(), .reason = "create_directories: " + ec.message()});
    }
    auto wal = std::unique_ptr<Wal>(
        new Wal(opt.dir, opt.fsync_on_commit, opt.segment_bytes, opt.commit_window));
    const auto segs = list_segments(opt.dir);
    if (segs.empty()) {
        wal->next_index_ = 1;
        if (auto r = wal->open_new_segment(); !r) {
            return tinykv::unexpected(r.error());
        }
    } else {
        // 续写到最新段：不新建、不改写既有内容
        wal->next_index_ = segs.back().first;
        wal->current_ = segs.back().second;
        wal->file_ = std::fopen(wal->current_.string().c_str(), "ab");
        if (wal->file_ == nullptr) {
            return tinykv::unexpected(
                IoFailed{.path = wal->current_.string(), .reason = "fopen(ab) 失败"});
        }
        wal->current_size_ = std::filesystem::file_size(wal->current_, ec);
    }
    wal->committer_ = std::thread([p = wal.get()] { p->commit_loop(); });
    return wal;
}

result<WalReplayResult> Wal::replay(const std::filesystem::path& dir) {
    WalReplayResult out;
    for (const auto& [index, path] : list_segments(dir)) {
        (void)index;
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr) {
            return tinykv::unexpected(IoFailed{.path = path.string(), .reason = "fopen(rb) 失败"});
        }
        std::uint64_t offset = 0;
        std::string payload;
        while (true) {
            std::array<char, kHeaderBytes> hdr{};
            const std::size_t got = std::fread(hdr.data(), 1, hdr.size(), f);
            if (got == 0) {
                break;  // 正常读完
            }
            if (got != hdr.size()) {
                out.stopped = true;
                out.reason = "尾部记录头不完整（掉电窗口）";
                break;
            }
            const auto len = get_u32(hdr.data());
            const auto crc = get_u32(hdr.data() + 4);
            if (len == 0 || len > kMaxRecordBytes) {
                out.stopped = true;
                out.reason = "记录长度非法";
                break;
            }
            payload.resize(len);
            if (std::fread(payload.data(), 1, len, f) != len) {
                out.stopped = true;
                out.reason = "载荷不完整（掉电窗口）";
                break;
            }
            if (crc32c(tinykv::as_bytes(tinykv::span{payload})) != crc) {
                out.stopped = true;
                out.reason = "CRC32C 不匹配";
                break;
            }
            ModifyVec batch;
            if (!decode_batch(payload, batch)) {
                out.stopped = true;
                out.reason = "载荷解码失败";
                break;
            }
            out.batches.push_back(std::move(batch));
            offset += kHeaderBytes + len;
        }
        std::fclose(f);
        if (out.stopped) {
            out.stopped_offset = offset;
            break;  // 坏记录之后的数据不可信，后续段不再读
        }
    }
    return out;
}

result<void> Wal::clear_all(const std::filesystem::path& dir) {
    for (const auto& [index, path] : list_segments(dir)) {
        (void)index;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            return tinykv::unexpected(
                IoFailed{.path = path.string(), .reason = "删除段失败: " + ec.message()});
        }
    }
    return {};
}

result<void> Wal::sync_locked() {
    if (file_ == nullptr) {
        return tinykv::unexpected(IoFailed{.path = dir_.string(), .reason = "段文件未打开"});
    }
    if (std::fflush(file_) != 0) {
        return tinykv::unexpected(IoFailed{.path = current_.string(), .reason = "fflush 失败"});
    }
    if (raw_fsync(file_) != 0) {
        return tinykv::unexpected(IoFailed{.path = current_.string(), .reason = "fsync 失败"});
    }
    fsync_count_.fetch_add(1, std::memory_order_relaxed);
    dirty_.store(false, std::memory_order_release);
    return {};
}

result<void> Wal::append(tinykv::span<const Modify> batch) {
    // code-review P0-1 附带项：先按"键值原始长度 + 每条固定开销"估算，超限直接拒绝。
    // 旧实现先 encode_batch 再检查——超大批次会先产生一次几十 MB 的分配，然后才被拒绝。
    std::size_t estimate = 0;
    for (const Modify& m : batch) {
        if (const auto* p = std::get_if<Put>(&m.op)) {
            estimate += p->key.size() + p->value.size() + 12;  // kind/cf/两个 varint 长度
        } else {
            estimate += std::get<Delete>(m.op).key.size() + 12;
        }
        if (estimate > kMaxRecordBytes) {
            return tinykv::unexpected(InvalidArgument{"单个写入批次编码后超过 64MB 上限"});
        }
    }
    std::string payload;
    encode_batch(batch, payload);
    if (payload.size() > kMaxRecordBytes) {
        return tinykv::unexpected(InvalidArgument{"单个写入批次编码后超过 64MB 上限"});
    }
    std::string record;
    record.reserve(kHeaderBytes + payload.size());
    put_u32(record, static_cast<std::uint32_t>(payload.size()));
    put_u32(record, crc32c(tinykv::as_bytes(tinykv::span{payload})));
    record.append(payload);

    std::lock_guard<std::mutex> lock(mu_);
    if (file_ == nullptr) {
        return tinykv::unexpected(IoFailed{.path = dir_.string(), .reason = "段文件未打开"});
    }
    // code-review P1-1：后台提交者已判定落盘失败 → 继续接受写入等于向调用方撒谎
    if (failed_.load(std::memory_order_acquire)) {
        return tinykv::unexpected(
            IoFailed{.path = current_.string(), .reason = "WAL 已进入失败态: " + fail_reason_});
    }
    // 仅在段内已有数据时考虑切换：空段上直接写，避免"单条记录超上限"造成段号跳号
    if (current_size_ > 0 && current_size_ + record.size() > segment_bytes_) {
        if (dirty_.load(std::memory_order_acquire)) {
            if (auto r = sync_locked(); !r) {  // 切段前先把旧段落盘
                return r;
            }
        }
        if (auto r = open_new_segment(); !r) {
            return r;
        }
    }
    if (std::fwrite(record.data(), 1, record.size(), file_) != record.size()) {
        return tinykv::unexpected(IoFailed{.path = current_.string(), .reason = "fwrite 失败"});
    }
    // 立即 fflush 推进内核页缓存：进程级崩溃不丢；fsync 延后到后台批量窗口
    if (std::fflush(file_) != 0) {
        return tinykv::unexpected(IoFailed{.path = current_.string(), .reason = "fflush 失败"});
    }
    current_size_ += record.size();
    dirty_.store(true, std::memory_order_release);
    cv_.notify_all();
    return {};
}

result<void> Wal::sync() {
    std::lock_guard<std::mutex> lock(mu_);
    return sync_locked();
}

}  // namespace tinykv
