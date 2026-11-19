// 文件用途：meta_store.h 的实现——全量快照编解码、CRC 校验、.bak 回退与原子替换。
// 归属：TinyKV-CPP（M7 演进 ⑧）。

#include "raftstore/meta_store.h"

#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

#include "common/crc32c.h"
#include "common/file_handle.h"
#include "raftstore/region.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tinykv::raftstore {
namespace {

constexpr std::string_view kMagic = "TKM1";
constexpr std::size_t kTrailerBytes = 4;  // 尾部 crc32c

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

std::uint32_t read_u32(std::string_view raw, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(raw[off + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

std::uint64_t read_u64(std::string_view raw, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(raw[off + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

int raw_fsync(std::FILE* f) {
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return ::fdatasync(::fileno(f));
#endif
}

}  // namespace

StoreMetaStore::StoreMetaStore(std::filesystem::path data_dir, StoreMeta& meta, std::string subdir)
    : dir_(std::move(data_dir) / std::move(subdir)), meta_(meta) {
}

std::string StoreMetaStore::encode(const std::vector<Region>& regions) {
    std::string out;
    out.reserve(64 * regions.size() + 16);
    out.append(kMagic);
    put_u32(out, static_cast<std::uint32_t>(regions.size()));
    for (const auto& r : regions) {
        put_u64(out, r.id.value);
        put_u64(out, r.epoch.version);
        put_u64(out, r.epoch.conf_ver);
        put_u64(out, static_cast<std::uint64_t>(r.size_hint));
        put_u32(out, static_cast<std::uint32_t>(r.start_key.size()));
        out.append(r.start_key);
        put_u32(out, static_cast<std::uint32_t>(r.end_key.size()));
        out.append(r.end_key);
    }
    const auto crc = crc32c(tinykv::as_bytes(tinykv::span{out}));
    put_u32(out, crc);
    return out;
}

result<std::vector<Region>> StoreMetaStore::parse_file(const std::filesystem::path& file) const {
    // RAII：raw.append 抛异常时句柄仍会释放（旧实现走不到 fclose）
    FilePtr f(std::fopen(file.string().c_str(), "rb"));
    if (!f) {
        return tinykv::unexpected(IoFailed{.path = file.string(), .reason = "fopen(rb) 失败"});
    }
    std::string raw;
    char buf[4096];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof(buf), f.get());
        raw.append(buf, n);
        if (n < sizeof(buf)) {
            break;
        }
    }
    if (std::ferror(f.get()) != 0) {
        return tinykv::unexpected(IoFailed{.path = file.string(), .reason = "读取 meta 文件失败"});
    }

    if (raw.size() < kMagic.size() + 4 + kTrailerBytes) {
        return tinykv::unexpected(Corrupted{.where = file.string(), .reason = "meta 文件过短"});
    }
    const std::size_t body_len = raw.size() - kTrailerBytes;
    const auto want = read_u32(raw, body_len);
    const auto got = crc32c(tinykv::as_bytes(tinykv::span{raw.data(), body_len}));
    if (want != got) {
        return tinykv::unexpected(Corrupted{.where = file.string(), .reason = "meta CRC 不匹配"});
    }
    if (std::string_view(raw.data(), kMagic.size()) != kMagic) {
        return tinykv::unexpected(Corrupted{.where = file.string(), .reason = "meta magic 不符"});
    }
    const auto count = read_u32(raw, kMagic.size());
    // BUG-0021（code-review r2 N-2）：reserve 前先做容量上界校验（与 P1-4 snapshot
    // total_chunks 同款修复模式）——每条记录至少 40 字节，count 超过物理可能即损坏文件。
    // 校验必须在 reserve 之前：否则 CRC 碰撞/构造文件的 count=4×10⁹ 会触发巨量分配，
    // bad_alloc/length_error 穿出 load() 未捕获 → 进程终止。
    const std::size_t payload = body_len - (kMagic.size() + 4);
    if (count > payload / 40) {
        return tinykv::unexpected(
            Corrupted{.where = file.string(), .reason = "meta count 超出文件容量"});
    }
    std::size_t pos = kMagic.size() + 4;
    std::vector<Region> regions;
    regions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        // 每条至少 8*4 + 4 + 4 = 40 字节；越界即损坏
        if (pos + 40 > body_len) {
            return tinykv::unexpected(Corrupted{.where = file.string(), .reason = "meta 记录越界"});
        }
        Region r;
        r.id = RegionId{read_u64(raw, pos)};
        const auto version = read_u64(raw, pos + 8);
        const auto conf_ver = read_u64(raw, pos + 16);
        r.size_hint = static_cast<std::int64_t>(read_u64(raw, pos + 24));
        const auto start_len = read_u32(raw, pos + 32);
        pos += 36;
        if (pos + start_len + 4 > body_len) {
            return tinykv::unexpected(
                Corrupted{.where = file.string(), .reason = "start_key 越界"});
        }
        r.start_key.assign(raw, pos, start_len);
        pos += start_len;
        const auto end_len = read_u32(raw, pos);
        pos += 4;
        if (pos + end_len > body_len) {
            return tinykv::unexpected(Corrupted{.where = file.string(), .reason = "end_key 越界"});
        }
        r.end_key.assign(raw, pos, end_len);
        pos += end_len;
        r.epoch = RegionEpoch{.version = version, .conf_ver = conf_ver};
        regions.push_back(std::move(r));
    }
    return regions;
}

result<bool> StoreMetaStore::load() {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = dir_.string(), .reason = "create_directories: " + ec.message()});
    }
    const auto main_file = dir_ / kFileName;
    const auto backup = dir_ / kBackupName;
    // code-review：exists 的 ec 不能忽略——IO 错误会被当成"首次启动"，重建 region 1 覆盖真相
    const bool main_exists = std::filesystem::exists(main_file, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = main_file.string(), .reason = "探测 meta 文件失败: " + ec.message()});
    }
    if (!main_exists) {
        ec.clear();
        const bool backup_exists = std::filesystem::exists(backup, ec);
        if (ec) {
            return tinykv::unexpected(IoFailed{.path = backup.string(),
                                               .reason = "探测 meta 备份失败: " + ec.message()});
        }
        if (backup_exists) {
            // 主文件缺失但备份在：说明上次改名窗口被打断，用备份（"不许猜"的合法形态）
            auto regions = parse_file(backup);
            if (!regions.has_value()) {
                return tinykv::unexpected(regions.error());
            }
            for (auto& r : *regions) {
                if (auto added = meta_.add_region(std::move(r)); !added.has_value()) {
                    return tinykv::unexpected(added.error());
                }
            }
            return true;
        }
        return false;  // 首次启动：无 meta，走单 region 兼容路径
    }
    auto regions = parse_file(main_file);
    if (!regions.has_value()) {
        ec.clear();
        const bool backup_exists = std::filesystem::exists(backup, ec);
        if (ec) {
            return tinykv::unexpected(IoFailed{.path = backup.string(),
                                               .reason = "探测 meta 备份失败: " + ec.message()});
        }
        if (!backup_exists) {
            return tinykv::unexpected(regions.error());  // 无备份可退：明确失败
        }
        auto from_backup = parse_file(backup);
        if (!from_backup.has_value()) {
            return tinykv::unexpected(regions.error());
        }
        regions = std::move(from_backup);  // 回退上一版并继续（红线 4：回退 + 告警，不修补）
    }
    for (auto& r : *regions) {
        if (auto added = meta_.add_region(std::move(r)); !added.has_value()) {
            return tinykv::unexpected(added.error());  // 盘上 meta 自相矛盾：不许继续
        }
    }
    return true;
}

result<void> StoreMetaStore::persist() {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = dir_.string(), .reason = "create_directories: " + ec.message()});
    }
    const auto payload = encode(meta_.all_regions());
    const auto tmp = dir_ / (std::string(kFileName) + ".tmp");
    FilePtr f(std::fopen(tmp.string().c_str(), "wb"));
    if (!f) {
        return tinykv::unexpected(IoFailed{.path = tmp.string(), .reason = "fopen(wb) 失败"});
    }
    const std::size_t written = std::fwrite(payload.data(), 1, payload.size(), f.get());
    const bool synced = std::fflush(f.get()) == 0 && raw_fsync(f.get()) == 0;
    f.reset();
    if (written != payload.size() || !synced) {
        std::filesystem::remove(tmp, ec);
        return tinykv::unexpected(
            IoFailed{.path = tmp.string(), .reason = "写 meta 失败（短写/落盘）"});
    }
    const auto main_file = dir_ / kFileName;
    const auto backup = dir_ / kBackupName;
    // 旧版先留成 .bak：新文件损坏时还有上一版可用（红线 4 的物质基础）
    const bool main_exists = std::filesystem::exists(main_file, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = main_file.string(), .reason = "探测 meta 文件失败: " + ec.message()});
    }
    if (main_exists) {
        ec.clear();
        std::filesystem::remove(backup, ec);
        std::filesystem::rename(main_file, backup, ec);
        if (ec) {
            return tinykv::unexpected(
                IoFailed{.path = backup.string(), .reason = "备份 meta 失败: " + ec.message()});
        }
    }
    std::filesystem::rename(tmp, main_file, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = main_file.string(), .reason = "rename meta 失败: " + ec.message()});
    }
    return {};
}

}  // namespace tinykv::raftstore
