// 文件用途：⑧ Region 元数据仓储用例——往返一致、CRC 损坏回退、.bak 兜底与"不许猜"。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `split_meta_persistence_test` 的单元侧）。
//
// 使用场景理解：元数据是"路由真相"，它出错的方式只有三种，正是本文件三条用例：
//   1) 写完读回对不上（编码缺陷）→ 往返一致断言；
//   2) 文件被截断/位翻转（掉电、坏盘）→ 必须报 Corrupted 或回退 .bak，绝不"修补后继续"；
//   3) 主文件丢了但备份在（改名窗口被打断）→ 用备份恢复，仍是同一份拓扑。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "raftstore/meta_store.h"
#include "temp_dir.h"

namespace {

using tinykv::Corrupted;
using tinykv::RegionEpoch;
using tinykv::RegionId;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;
using tinykv::raftstore::StoreMetaStore;
using tinykv::testlib::TempDir;

Region make_region(std::uint64_t id, std::string start, std::string end, std::uint64_t ver) {
    Region r;
    r.id = RegionId{id};
    r.start_key = std::move(start);
    r.end_key = std::move(end);
    r.epoch = RegionEpoch{.version = ver, .conf_ver = 1};
    r.size_hint = 128;
    return r;
}

TEST(MetaStoreTest, PersistThenLoadRestoresSameTopology) {
    TempDir tmp{"tinykv_meta_roundtrip"};
    StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "m", 2)).has_value());
    ASSERT_TRUE(meta.add_region(make_region(2, "m", "", 3)).has_value());
    {
        StoreMetaStore store(tmp.path() / "db", meta);
        ASSERT_TRUE(store.persist().has_value());
    }
    StoreMeta restored;
    StoreMetaStore store(tmp.path() / "db", restored);
    auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(*loaded) << "有 meta 却报告首次启动";
    ASSERT_EQ(restored.region_count(), 2U);
    const auto regions = restored.all_regions();
    ASSERT_EQ(regions.size(), 2U);
    EXPECT_EQ(regions[0].id.value, 1U);
    EXPECT_EQ(regions[0].end_key, "m");
    EXPECT_EQ(regions[0].epoch.version, 2U);
    EXPECT_EQ(regions[1].start_key, "m");
    EXPECT_EQ(regions[1].epoch.conf_ver, 1U);
    // 路由仍可判定（重建 peer 的前提）
    auto hit = restored.find_region("z");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->id.value, 2U);
}

TEST(MetaStoreTest, MissingFileMeansFirstBootNotAnError) {
    TempDir tmp{"tinykv_meta_first"};
    StoreMeta meta;
    StoreMetaStore store(tmp.path() / "db", meta);
    auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(*loaded) << "空目录必须报告「首次启动」，由调用方决定建 region 1";
}

// 位翻转：CRC 不匹配 → Corrupted（不是静默当成空库，否则重启后整个拓扑消失）
TEST(MetaStoreTest, CorruptedFileIsRejected) {
    TempDir tmp{"tinykv_meta_corrupt"};
    const auto dir = tmp.path() / "db";
    StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "", 1)).has_value());
    {
        StoreMetaStore store(dir, meta);
        ASSERT_TRUE(store.persist().has_value());
    }
    const auto file = dir / "meta" / StoreMetaStore::kFileName;
    ASSERT_TRUE(std::filesystem::exists(file));
    {
        std::fstream io(file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.is_open());
        io.seekp(20);  // 落在第一条记录的 id 字段内
        const char bad = static_cast<char>(0xFF);
        io.write(&bad, 1);
    }
    StoreMeta restored;
    StoreMetaStore store(dir, restored);
    auto loaded = store.load();
    ASSERT_FALSE(loaded.has_value()) << "损坏的 meta 不得被当成空库";
    EXPECT_TRUE(std::holds_alternative<Corrupted>(loaded.error()));
}

// 主文件丢失但 .bak 在：改名窗口被打断的合法恢复路径
TEST(MetaStoreTest, BackupIsUsedWhenPrimaryIsMissing) {
    TempDir tmp{"tinykv_meta_backup"};
    const auto dir = tmp.path() / "db";
    StoreMeta meta;
    ASSERT_TRUE(meta.add_region(make_region(1, "", "m", 1)).has_value());
    {
        StoreMetaStore store(dir, meta);
        ASSERT_TRUE(store.persist().has_value());  // 第一次：写主文件
    }
    // 第二次 persist 会把上一版留成 .bak
    ASSERT_TRUE(meta.add_region(make_region(2, "m", "", 2)).has_value());
    {
        StoreMetaStore store(dir, meta);
        ASSERT_TRUE(store.persist().has_value());
    }
    ASSERT_TRUE(std::filesystem::exists(dir / "meta" / StoreMetaStore::kBackupName));
    std::filesystem::remove(dir / "meta" / StoreMetaStore::kFileName);

    StoreMeta restored;
    StoreMetaStore store(dir, restored);
    auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(*loaded);
    EXPECT_EQ(restored.region_count(), 1U) << "回退的是上一版（一个 region），不是凭空造两个";
}

}  // namespace
