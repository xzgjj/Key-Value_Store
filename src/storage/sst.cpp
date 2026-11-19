// 文件用途：sst.h 的实现——整表写入（尾部 CRC）与整表载入校验。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01/02）。

#include "storage/sst.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>  // _commit / _fileno（落盘原语，与 wal.cpp 同口径）
#else
#include <unistd.h>  // fdatasync / fileno
#endif

#include "common/crc32c.h"
#include "common/file_handle.h"
#include "common/varint.h"

namespace tinykv {
namespace {

constexpr std::size_t kTrailerBytes = 8;  // [u32 条数][u32 crc32c(body)]

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t read_u32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

bool read_file(const std::filesystem::path& path, std::string& out) {
    // RAII：out.append 抛 bad_alloc 时也会关闭句柄（旧实现会泄漏 FILE*）；
    // 同时检查 ferror——把"读失败"当"文件较短"会让损坏的 SST 被当成空表。
    FilePtr f(std::fopen(path.string().c_str(), "rb"));
    if (!f) {
        return false;
    }
    std::array<char, 64 * 1024> buf{};
    std::size_t got = 0;
    while ((got = std::fread(buf.data(), 1, buf.size(), f.get())) > 0) {
        out.append(buf.data(), got);
    }
    return std::ferror(f.get()) == 0;
}

// 解析 body：返回 false 表示结构非法（截断/长度越界/尾随垃圾）
bool parse_body(std::string_view body, std::vector<SstEntry>& out) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::uint64_t klen = 0;
        if (!get_varint(body, pos, klen) || klen > body.size() - pos) {
            return false;
        }
        SstEntry e;
        e.key.assign(body.substr(pos, static_cast<std::size_t>(klen)));
        pos += static_cast<std::size_t>(klen);
        if (pos >= body.size()) {
            return false;
        }
        const auto kind = static_cast<std::uint8_t>(body[pos++]);
        // kind 只允许 0（有值）/1（墓碑）：其它值说明文件损坏，按"删除"处理会静默丢数据
        if (kind > 1) {
            return false;
        }
        std::uint64_t vlen = 0;
        if (!get_varint(body, pos, vlen) || vlen > body.size() - pos) {
            return false;
        }
        if (kind == 0) {
            e.value = std::string(body.substr(pos, static_cast<std::size_t>(vlen)));
        }
        pos += static_cast<std::size_t>(vlen);
        // 严格升序是二分查找 find() 的正确性前提：乱序表会让查找返回错误结果
        if (!out.empty() && !(out.back().key < e.key)) {
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

}  // namespace

result<std::unique_ptr<SstReader>> SstReader::open(const std::filesystem::path& path) {
    std::string raw;
    if (!read_file(path, raw)) {
        return tinykv::unexpected(IoFailed{.path = path.string(), .reason = "fopen(rb) 失败"});
    }
    if (raw.size() < kTrailerBytes) {
        return tinykv::unexpected(Corrupted{.where = path.string(), .reason = "文件短于尾部长度"});
    }
    const std::size_t body_len = raw.size() - kTrailerBytes;
    const std::string_view body(raw.data(), body_len);
    const auto count = read_u32(raw.data() + body_len);
    const auto crc = read_u32(raw.data() + body_len + 4);
    if (crc32c(tinykv::as_bytes(tinykv::span{body})) != crc) {
        return tinykv::unexpected(Corrupted{.where = path.string(), .reason = "crc32c 不匹配"});
    }

    auto reader = std::unique_ptr<SstReader>(new SstReader());  // NOLINT：私有构造，立即接管
    if (!parse_body(body, reader->entries_)) {
        return tinykv::unexpected(Corrupted{.where = path.string(), .reason = "记录结构非法"});
    }
    if (reader->entries_.size() != count) {
        return tinykv::unexpected(Corrupted{.where = path.string(), .reason = "条数与尾部不一致"});
    }
    return reader;
}

const SstEntry* SstReader::find(std::string_view key) const {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), key,
        [](const SstEntry& e, std::string_view k) { return std::string_view(e.key) < k; });
    if (it == entries_.end() || std::string_view(it->key) != key) {
        return nullptr;
    }
    return &*it;
}

result<std::unique_ptr<SstWriter>> SstWriter::create(const std::filesystem::path& path) {
    auto writer = std::unique_ptr<SstWriter>(new SstWriter());  // NOLINT：私有构造，立即接管
    writer->path_ = path;
    return writer;
}

result<void> SstWriter::add(std::string key, std::optional<std::string> value) {
    if (finished_) {
        return tinykv::unexpected(InvalidArgument{"finish() 之后不可再 add"});
    }
    if (has_last_key_ && key <= last_key_) {
        return tinykv::unexpected(InvalidArgument{"SST 要求 key 严格升序，实际逆序或重复：" + key});
    }
    last_key_ = key;
    has_last_key_ = true;
    put_varint(body_, key.size());
    body_.append(key);
    body_.push_back(value.has_value() ? 0 : 1);
    put_varint(body_, value ? value->size() : 0U);
    if (value) {
        body_.append(*value);
    }
    ++count_;
    return {};
}

result<void> SstWriter::finish() {
    if (finished_) {
        return tinykv::unexpected(InvalidArgument{"finish() 重复调用"});
    }
    // CRC 必须覆盖"纯 body"（不含尾部 8 字节）。先算长度与校验值再追加尾部——
    // 若写在两个 put_u32 之间，body_.size() 已被前一个 put_u32 撑大，校验范围会少 4 字节。
    const std::size_t body_len = body_.size();
    const std::uint32_t body_crc = crc32c(tinykv::as_bytes(tinykv::span{body_.data(), body_len}));
    put_u32(body_, count_);
    put_u32(body_, body_crc);
    // BUG-0012：先写临时名 → fsync → 改名为正式名。旧实现直接写正式名且只 fflush，
    // 短写会留下半截 .sst；清单缺失时引擎按目录扫描会把半截表当合法表加载 → Corrupted 启动失败。
    const auto tmp = path_.string() + ".tmp";
    // RAII：作用域退出必关闭（旧实现依赖手工 fclose，中途新增可能抛异常的步骤就会漏）
    FilePtr f(std::fopen(tmp.c_str(), "wb"));
    if (!f) {
        return tinykv::unexpected(IoFailed{.path = tmp, .reason = "fopen(wb) 失败"});
    }
    const std::size_t written = std::fwrite(body_.data(), 1, body_.size(), f.get());
    const bool flushed = std::fflush(f.get()) == 0;
#ifdef _WIN32
    const bool synced = flushed && _commit(_fileno(f.get())) == 0;
#else
    const bool synced = flushed && ::fdatasync(::fileno(f.get())) == 0;
#endif
    f.reset();  // 显式关闭：rename 前必须释放句柄（Windows 下打开的文件不能改名）
    if (written != body_.size() || !synced) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);  // 失败即删，不留半截文件
        return tinykv::unexpected(IoFailed{.path = tmp, .reason = "写 SST 失败（短写或落盘失败）"});
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return tinykv::unexpected(
            IoFailed{.path = path_.string(), .reason = "SST 改名失败: " + ec.message()});
    }
    finished_ = true;
    return {};
}

SstWriter::~SstWriter() = default;

}  // namespace tinykv
