// 文件用途：崩溃恢复——已确认提交的写必须能从 WAL 完整重放，且重放不改写文件（REL-02）。
// 归属：TinyKV-CPP（M1 单机存储）。
// 使用场景理解：真正的 kill -9 需要进程级注入（随 M2 的集成测试台落地）。本测试用等价的
// 进程内断言证明同一件事：写入返回成功后，数据必须已经完整躺在 WAL 里——不依赖内存表是否落盘。
// 这是 REL-02 的核心：fsync 返回之后的任何掉电都不该丢数据。

#include "storage/engine.h"
#include "storage/wal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "storage_test_base.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace) —— 测试内便捷引用

namespace fs = std::filesystem;

constexpr int kWrites = 1000;

// 7919 与 1000 互质 → i*7919%1000 是 0..999 的排列，即按键乱序写入
std::string key_of(int i) {
    return "k" + std::to_string((static_cast<long long>(i) * 7919) % kWrites);
}

std::uintmax_t wal_bytes(const fs::path& wal_dir) {
    std::uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(wal_dir)) {
        if (e.is_regular_file()) {
            total += e.file_size();
        }
    }
    return total;
}

// 写入 1000 条且内存表不触发 flush → 此刻数据只在 WAL 里
std::uintmax_t write_thousand_without_flush(const fs::path& dir, std::unique_ptr<Engine>& engine) {
    auto opened = Engine::open(EngineOptions{.dir = dir, .memtable_limit_bytes = 1ULL << 30});
    EXPECT_TRUE(opened.has_value());
    engine = std::move(*opened);
    for (int i = 0; i < kWrites; ++i) {
        ModifyVec batch{Modify{Put{.cf = Cf::Default, .key = key_of(i), .value = "v" + std::to_string(i)}}};
        EXPECT_TRUE(engine->write(batch).has_value());
    }
    return wal_bytes(dir / "wal");
}

TEST(WalCrash, ConfirmedWritesAreReplayableFromWal) {
    TempDir tmp("tinykv_crash_dup");
    const fs::path dir = tmp.path() / "db";
    std::unique_ptr<Engine> engine;
    const auto size_before = write_thousand_without_flush(dir, engine);
    EXPECT_GT(size_before, 0U);

    auto replay = Wal::replay(dir / "wal");
    ASSERT_TRUE(replay.has_value());
    std::size_t records = 0;
    for (const auto& batch : replay->batches) {
        records += batch.size();
    }
    EXPECT_EQ(records, static_cast<std::size_t>(kWrites));
    EXPECT_FALSE(replay->stopped);
}

TEST(WalCrash, ReplayDoesNotRewriteWalFiles) {
    // 参考项目"构造即清空文件"的反例：重放必须是纯读，文件大小一个字节都不许变
    TempDir tmp("tinykv_crash_ro");
    const fs::path dir = tmp.path() / "db";
    std::unique_ptr<Engine> engine;
    const auto size_before = write_thousand_without_flush(dir, engine);

    ASSERT_TRUE(Wal::replay(dir / "wal").has_value());
    ASSERT_TRUE(Wal::replay(dir / "wal").has_value());
    EXPECT_EQ(wal_bytes(dir / "wal"), size_before);
}

TEST(WalCrash, AllKeysReadableAfterRestart) {
    TempDir tmp("tinykv_crash_restart");
    const fs::path dir = tmp.path() / "db";
    {
        std::unique_ptr<Engine> engine;
        write_thousand_without_flush(dir, engine);
        ASSERT_TRUE(engine->close().has_value());   // 正常关闭：内存表落 SST
    }

    auto reopened = Engine::open(EngineOptions{.dir = dir});
    ASSERT_TRUE(reopened.has_value());
    int missing = 0;
    for (int i = 0; i < kWrites; ++i) {
        auto got = (*reopened)->get(Cf::Default, bytes(key_of(i)));
        if (!got.has_value() || !got->value.has_value()) {
            ++missing;
        }
    }
    EXPECT_EQ(missing, 0) << "重启后丢失 key 数：" << missing;
}

}  // namespace
