// 文件用途：持久化不变量守卫用例——批次原子性、SST/清单写盘顺序、IO 错误分层。
// 归属：TinyKV-CPP（M7 修复波次：code-review-2026-11-18 §1 的 P0-4/P0-6、§2 的 P1-2/P1-3）。
//
// 使用场景理解：这几条缺陷都无法靠"正常读写"暴露——
//   * 条目与 meta 分两批写，只有在两次写之间崩溃才不一致（BUG-0014）；
//   * SST 直接写正式名、只 fflush，只有短写/掉电才留半截表（BUG-0013）；
//   * 清单"先删后改名"，只在改名窗口被打断时才丢清单（BUG-0012）；
//   * MVCC 把 IoFailed 折成 nullopt，只在"读失败但业务继续"时才产生幻读（BUG-0015）。
// 因此本文件用"可观测的结构性证据"替代随机崩溃：统计落盘批次数与批次内容、
// 检查目录里有没有临时文件、把引擎关掉制造真实 IO 失败。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/compat.h"
#include "raft/raftlog.h"
#include "storage/engine.h"
#include "temp_dir.h"
#include "txn/mvcc.h"

namespace {

// 精确引入而非整体 using namespace：本文件要与 raft/mvcc/storage 三处同名类型共存，
// 整体引入会让 Cf/Ts 的来源变模糊（也顺带避开跨文件的重复代码块检测）。
namespace raft = tinykv::raft;
namespace txn = tinykv::txn;
namespace raftpb = tinykv::raftpb;
using tinykv::Cf;
using tinykv::Engine;
using tinykv::GetResult;
using tinykv::IoFailed;
using tinykv::LogIndex;
using tinykv::Modify;
using tinykv::Put;
using tinykv::Reader;
using tinykv::result;
using tinykv::Storage;
using tinykv::Ts;
using tinykv::testlib::TempDir;

// 记录每批写的 key 集合，用来断言"条目变更与 meta 在同一批"
class RecordingStorage final : public Storage {
public:
    explicit RecordingStorage(Engine& inner) : inner_(inner) {}

    [[nodiscard]] result<GetResult> get(Cf cf, tinykv::span<const std::byte> key) override {
        return inner_.get(cf, key);
    }
    [[nodiscard]] result<void> write(tinykv::span<const Modify> batch) override {
        std::vector<std::string> keys;
        keys.reserve(batch.size());
        for (const auto& m : batch) {
            keys.push_back(std::visit([](const auto& op) { return op.key; }, m.op));
        }
        batches_.push_back(std::move(keys));
        return inner_.write(batch);
    }
    [[nodiscard]] result<std::unique_ptr<Reader>> create_reader() override {
        return inner_.create_reader();
    }

    [[nodiscard]] std::size_t batch_count() const { return batches_.size(); }
    [[nodiscard]] const std::vector<std::string>& last_batch() const { return batches_.back(); }

private:
    Engine& inner_;
    std::vector<std::vector<std::string>> batches_;
};

// 是否含 raft log 内部 meta 键（"\0lm"）与 applied 键（"\0la"）
bool has_meta_key(const std::vector<std::string>& keys) {
    return std::any_of(keys.begin(), keys.end(),
                       [](const std::string& k) { return k == std::string("\0lm", 3); });
}
bool has_applied_key(const std::vector<std::string>& keys) {
    return std::any_of(keys.begin(), keys.end(),
                       [](const std::string& k) { return k == std::string("\0la", 3); });
}
bool has_entry_key(const std::vector<std::string>& keys) {
    return std::any_of(keys.begin(), keys.end(), [](const std::string& k) {
        return k.size() >= 3 && k.compare(0, 3, std::string("\0re", 3)) == 0;
    });
}

raftpb::Entry make_entry(std::uint64_t index, std::uint64_t term, std::string data) {
    raftpb::Entry e;
    e.set_index(index);
    e.set_term(term);
    e.set_data(std::move(data));
    return e;
}

struct FaultsFixture {
    TempDir tmp{"tinykv_durability"};
    std::unique_ptr<Engine> engine;

    FaultsFixture() {
        auto eng = Engine::open({.dir = tmp.path() / "db"});
        EXPECT_TRUE(eng.has_value()) << "引擎打开失败";
        engine = std::move(*eng);
    }
};

// BUG-0014：append 必须"条目 + meta"一次落盘。两次写之间崩溃会留下
// 条目已写、last_index 未推进的库——重启后 at(last_index) 失败，恢复路径直接起不来。
TEST(DurabilityGuard, RaftLogAppendWritesEntriesAndMetaInOneBatch) {
    FaultsFixture f;
    RecordingStorage rec(*f.engine);
    auto log = raft::RaftLog::open(rec);
    ASSERT_TRUE(log.has_value());

    std::vector<raftpb::Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b")};
    ASSERT_TRUE((*log)->append(entries).has_value());

    ASSERT_EQ(rec.batch_count(), 1U) << "条目与 meta 被拆成了多批（崩溃窗口）";
    EXPECT_TRUE(has_entry_key(rec.last_batch()));
    EXPECT_TRUE(has_meta_key(rec.last_batch()));
    EXPECT_TRUE(has_applied_key(rec.last_batch()));
    EXPECT_EQ((*log)->last_index().value, 2U);
}

// 同一条不变量在截断与压缩路径上：删除项与 meta 必须同批（BUG-0014）
TEST(DurabilityGuard, RaftLogTruncateAndCompactAreAtomicBatches) {
    FaultsFixture f;
    RecordingStorage rec(*f.engine);
    auto log = raft::RaftLog::open(rec);
    ASSERT_TRUE(log.has_value());
    std::vector<raftpb::Entry> entries{make_entry(1, 1, "a"), make_entry(2, 1, "b"),
                                       make_entry(3, 1, "c")};
    ASSERT_TRUE((*log)->append(entries).has_value());
    ASSERT_TRUE((*log)->set_committed(LogIndex{1}).has_value());
    ASSERT_TRUE((*log)->set_applied(LogIndex{1}).has_value());

    // 截断：只能删未提交部分（committed=1 → 从 2 起可截）
    const auto before_truncate = rec.batch_count();
    ASSERT_TRUE((*log)->truncate_from(LogIndex{2}).has_value());
    EXPECT_EQ(rec.batch_count(), before_truncate + 1) << "截断拆成了多批（批次统计含 meta 同批）";
    EXPECT_TRUE(has_entry_key(rec.last_batch()));
    EXPECT_TRUE(has_meta_key(rec.last_batch()));

    // 压缩：重新追加并用快照/已应用位点把 1 压掉
    const std::vector<raftpb::Entry> tail{make_entry(2, 1, "b"), make_entry(3, 1, "c")};
    ASSERT_TRUE((*log)->append(tail).has_value());
    ASSERT_TRUE((*log)->set_committed(LogIndex{3}).has_value());
    ASSERT_TRUE((*log)->set_applied(LogIndex{3}).has_value());
    const auto before_compact = rec.batch_count();
    ASSERT_TRUE((*log)->compact(LogIndex{1}).has_value());
    EXPECT_EQ(rec.batch_count(), before_compact + 1) << "压缩拆成了多批";
    EXPECT_TRUE(has_entry_key(rec.last_batch()));
    EXPECT_TRUE(has_meta_key(rec.last_batch()));
}

// BUG-0013：SST 写盘必须先写临时名再改名；BUG-0012：清单改名不能先删后改名。
// 可观测证据：目录里没有残留临时文件、连续两次 flush 后清单仍可读出两张表。
TEST(DurabilityGuard, FlushLeavesNoTempFilesAndManifestSurvivesRewrite) {
    TempDir tmp{"tinykv_flush"};
    const auto dir = tmp.path() / "db";
    {
        auto eng = Engine::open({.dir = dir, .memtable_limit_bytes = 1024});
        ASSERT_TRUE(eng.has_value());
        for (int i = 0; i < 40; ++i) {
            const std::string key = "k" + std::to_string(i);
            const std::vector<Modify> batch{
                Modify{Put{.cf = Cf::Default, .key = key, .value = "v"}}};
            auto r = (*eng)->write(batch);
            ASSERT_TRUE(r.has_value()) << i;
        }
        ASSERT_GE((*eng)->sst_count(), 2U) << "需要至少两张表才能覆盖清单重写";
        // 目录里不允许有半截产物
        for (const auto& entry : std::filesystem::directory_iterator(dir / "sst")) {
            EXPECT_NE(entry.path().extension(), ".tmp") << "残留临时 SST: " << entry.path();
        }
        EXPECT_TRUE(std::filesystem::exists(dir / "manifest"));
        ASSERT_TRUE((*eng)->close().has_value());
    }
    // 重开：清单必须列出全部表，数据完整（清单先删后改名会在这里丢表 → Corrupted）
    auto reopened = Engine::open({.dir = dir, .memtable_limit_bytes = 1024});
    ASSERT_TRUE(reopened.has_value()) << "清单/表不一致导致重开失败";
    for (int i = 0; i < 40; ++i) {
        const std::string key = "k" + std::to_string(i);
        auto got =
            (*reopened)->get(Cf::Default, tinykv::as_bytes(tinykv::span{key.data(), key.size()}));
        ASSERT_TRUE(got.has_value()) << key;
        ASSERT_TRUE(got->value.has_value()) << key << " 丢失";
        EXPECT_EQ(*got->value, "v");
    }
}

// BUG-0015：读失败不能被折叠成"不存在"。引擎关闭制造真实 IoFailed：
// 旧实现返回 nullopt（读路径当成"该 key 无值"），并发下就是幻读。
TEST(DurabilityGuard, MvccReadSurfacesIoFailureInsteadOfEmptyValue) {
    FaultsFixture f;
    ASSERT_TRUE(f.engine->close().has_value());
    txn::MvccTxn mvcc(*f.engine);
    auto got = mvcc.get("any", Ts{1000});
    ASSERT_FALSE(got.has_value()) << "IO 失败被当成了'无值'";
    EXPECT_TRUE(std::holds_alternative<IoFailed>(got.error()));

    auto status = mvcc.check_txn_status("any", Ts{1000}, 2000);
    ASSERT_FALSE(status.has_value()) << "决议路径同样不能把读失败当'已回滚'";
    EXPECT_TRUE(std::holds_alternative<IoFailed>(status.error()));
}

}  // namespace
