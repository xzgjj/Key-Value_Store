// 文件用途：⑦ 跨区事务用例（M7-REL-01）——两区原子提交、关闭时明确拒绝、崩溃后由锁决议收敛。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `txn_cross_region_test`）。
//
// 使用场景理解：跨区事务只有三种可判定的结局，本文件一条用例钉一种：
//   1) 两区都可见（正常提交）；
//   2) 都不可见（PREPARE 阶段失败 → 回滚已准备的组）；
//   3) 中途"崩溃"（primary 已提交、secondary 未提交）→ 读者遇锁后靠 primary 状态正向推进。
// 第 3 条不用真杀进程：把"primary 已提交、secondary 还挂着锁"这个状态直接造出来，
// 它才是恢复路径真正要处理的输入（随机 kill 命中的正是这个窗口，但不稳定）。

#include "server/server_app.h"

#include "cli/cli.h"
#include "cli/http_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "raft_test_base.h"
#include "storage/engine.h"
#include "txn/mvcc.h"

namespace {

using tinykv::Cf;
using tinykv::Engine;
using tinykv::Ts;
using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;
using tinykv::txn::Mutation;
using tinykv::txn::MvccTxn;

struct Daemon {
    TempDir tmp{"tinykv_cross_region"};
    ServerApp app;

    explicit Daemon(bool cross_region) : app(make_options(tmp.path() / "db", cross_region)) {
        EXPECT_TRUE(app.start().has_value());
    }
    ~Daemon() { (void)app.stop(); }

    static ServerOptions make_options(const std::filesystem::path& dir, bool cross_region) {
        ServerOptions opt;
        opt.data_dir = dir;
        opt.status_port = 0;
        opt.worker_interval = std::chrono::milliseconds(2);
        opt.cross_region_enabled = cross_region;
        return opt;
    }

    [[nodiscard]] int cli(const std::vector<std::string>& args, std::string* out = nullptr) {
        std::vector<std::string> full{"--endpoint",
                                      "127.0.0.1:" + std::to_string(app.status_port())};
        full.insert(full.end(), args.begin(), args.end());
        std::ostringstream o;
        std::ostringstream e;
        const int rc = tinykv::cli::run_cli(full, o, e);
        last_err = e.str();
        if (out != nullptr) {
            *out = o.str();
        }
        return rc;
    }
    std::string last_err;
};

// 从 "OK session=tN" 里取 session id
std::string session_of(const std::string& text) {
    const auto at = text.find("session=");
    if (at == std::string::npos) {
        return {};
    }
    std::string sid;
    for (std::size_t i = at + 8; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n' || ch == '\r' || ch == ' ') {
            break;
        }
        sid.push_back(ch);
    }
    return sid;
}

// 1) 两区原子提交：a 在 region 1、z 在 region 2，同一事务提交后两处都可见
TEST(CrossRegionTxnTest, CommitsAcrossTwoRegionsAndBothKeysVisible) {
    Daemon d(/*cross_region=*/true);
    std::string out;
    ASSERT_EQ(d.cli({"region", "split", "1", "m"}, &out), 0) << d.last_err << out;

    std::string begin;
    ASSERT_EQ(d.cli({"txn", "begin"}, &begin), 0) << d.last_err;
    const std::string sid = session_of(begin);
    ASSERT_FALSE(sid.empty()) << begin;

    ASSERT_EQ(d.cli({"txn", "set", sid, "a", "va"}), 0) << d.last_err;  // region 1
    ASSERT_EQ(d.cli({"txn", "set", sid, "z", "vz"}), 0) << d.last_err;  // region 2
    std::string commit;
    ASSERT_EQ(d.cli({"txn", "commit", sid}, &commit), 0)
        << "跨区提交应成功；stdout=" << commit << " stderr=" << d.last_err;

    // 新事务读两处：都可见
    std::string begin2;
    ASSERT_EQ(d.cli({"txn", "begin"}, &begin2), 0) << d.last_err;
    const std::string sid2 = session_of(begin2);
    std::string got_a;
    std::string got_z;
    ASSERT_EQ(d.cli({"txn", "get", sid2, "a"}, &got_a), 0) << d.last_err;
    ASSERT_EQ(d.cli({"txn", "get", sid2, "z"}, &got_z), 0) << d.last_err;
    EXPECT_NE(got_a.find("va"), std::string::npos) << got_a;
    EXPECT_NE(got_z.find("vz"), std::string::npos)
        << "region 2 的写入没落地（又回到'提交成功但数据不见了'）: " << got_z;
    ASSERT_EQ(d.cli({"txn", "rollback", sid2}), 0);
}

// 2) 关闭跨区时：提交被明确拒绝（不是静默写错分区）
TEST(CrossRegionTxnTest, RejectedWhenCoordinatorDisabled) {
    Daemon d(/*cross_region=*/false);
    std::string out;
    ASSERT_EQ(d.cli({"region", "split", "1", "m"}, &out), 0) << d.last_err << out;

    std::string begin;
    ASSERT_EQ(d.cli({"txn", "begin"}, &begin), 0) << d.last_err;
    const std::string sid = session_of(begin);
    ASSERT_FALSE(sid.empty());
    ASSERT_EQ(d.cli({"txn", "set", sid, "a", "va"}), 0) << d.last_err;
    ASSERT_EQ(d.cli({"txn", "set", sid, "z", "vz"}), 0) << d.last_err;

    std::string commit;
    EXPECT_NE(d.cli({"txn", "commit", sid}, &commit), 0) << "跨区事务在协调器关闭时必须失败";
    const std::string combined = commit + d.last_err;
    EXPECT_NE(combined.find("跨区"), std::string::npos) << "错误信息应指明是跨区问题: " << combined;
}

// 3) 恢复路径：primary 已提交、secondary 还挂着锁（"提交到一半崩了"）
//    读者遇锁 → check_txn_status(primary) → 已提交 → 用同一 commit_ts 补 Write 删锁 → 值可见
TEST(CrossRegionTxnTest, SecondaryLockConvergesFromPrimaryCommit) {
    TempDir tmp{"tinykv_cross_region_recover"};
    auto region1 = Engine::open({.dir = tmp.path() / "r1"});
    auto region2 = Engine::open({.dir = tmp.path() / "r2"});
    ASSERT_TRUE(region1.has_value());
    ASSERT_TRUE(region2.has_value());

    const Ts start_ts{100};
    // 造状态：primary 在 region 1（正常 prewrite），secondary 在 region 2（跨区分组，primary
    // 在别区）
    {
        MvccTxn mvcc(**region1);
        const std::vector<Mutation> primary_batch{
            Mutation{.op = Mutation::Op::kPut, .key = "a", .value = "va"}};
        ASSERT_TRUE(mvcc.prewrite(primary_batch, "a", start_ts, 3000, 10).has_value());
    }
    {
        MvccTxn mvcc(**region2);
        const std::vector<Mutation> secondary_batch{
            Mutation{.op = Mutation::Op::kPut, .key = "z", .value = "vz"}};
        // cross_region 语义 = 允许 primary 不在本批
        ASSERT_TRUE(
            mvcc.prewrite(secondary_batch, "a", start_ts, 3000, 10, /*primary_required=*/false)
                .has_value());
    }
    // "崩溃"：只有 primary 被提交
    {
        MvccTxn mvcc(**region1);
        const std::vector<std::string> keys{"a"};
        ASSERT_TRUE(mvcc.commit(keys, start_ts, Ts{200}).has_value());
    }

    // 读者在 region 2 遇锁 → 按 primary 状态决议（红线 3）
    {
        MvccTxn mvcc(**region2);
        auto got = mvcc.get("z", Ts{300});
        ASSERT_FALSE(got.has_value());
        ASSERT_TRUE(std::holds_alternative<tinykv::LockConflict>(got.error()));

        auto st = MvccTxn(**region1).check_txn_status("a", start_ts, 20);
        ASSERT_TRUE(st.has_value());
        ASSERT_TRUE(st->committed) << "primary 的 Write 记录就是'已提交'的凭证";
        const Ts commit_ts = st->commit_ts;

        // 决议锚点是 primary（"a"），不是被锁住的 key —— 这正是"决议只看 primary 状态"的形状；
        // 真实装配里 PeerTxnExecutor 会把这次决议广播到每个 region 的引擎。
        const auto cleaned = mvcc.resolve_locks_of("a", start_ts, commit_ts);
        ASSERT_TRUE(cleaned.has_value());
        (void)cleaned;
        // 注意：真实装配里 resolve 会连 primary 所在 region 一起决议（PeerTxnExecutor 广播）；
        // 这里只验证 region 2 的这一半：锁被正向推进成 Write
        auto after = mvcc.get("z", Ts{300});
        ASSERT_TRUE(after.has_value()) << "决议后仍读不到";
        ASSERT_TRUE(after->has_value());
        EXPECT_EQ(**after, "vz");
    }
}

}  // namespace
