// 文件用途：⑦ 跨区事务的"随机中断"回归（夜跑级）——150 轮随机 kill 后重启收敛，
//          断言"要么全可见、要么全不可见"，锁绝不遗留。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `txn_cross_region_chaos_random`）。
//
// 与 `txn_cross_region_chaos`（进程内枚举三窗口）的分工：
//   * 枚举版：确定性覆盖三个已知窗口，fast 反馈，命中率高；
//   * 随机版（本文件）：真进程崩溃（子进程 _Exit，WAL 恢复语义）+ 随机死亡点
//     （0..4 均匀采样）+ 随机主组侧 + 随机"半途决议"窗口，价值在于不预设
//     "只有三个窗口"——中断可以落在协议的任意步之后。
//
// 每轮流程：种子 = 基数 + 轮次（可精确复现）→ 随机选 kill_step 与主组侧 →
// 子进程执行 2PC 前 kill_step 步后 _Exit → 父进程重开两个引擎 →
// （可能先做一次只清单侧的半途决议）→ 按 primary 真实状态收敛 →
// 断言两 key 可见性一致且与 kill_step 的预期终态吻合。

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "common/error_format.h"
#include "storage/engine.h"
#include "temp_dir.h"
#include "txn/mvcc.h"

#ifndef TXN_CHAOS_WRITER_PATH
#error "需要定义 TXN_CHAOS_WRITER_PATH（见 tests/integration/CMakeLists.txt）"
#endif

namespace {

using tinykv::Engine;
using tinykv::Ts;
using tinykv::testlib::TempDir;
using tinykv::txn::MvccTxn;

constexpr int kRounds = 150;
constexpr std::uint32_t kSeedBase = 20261118;
// 子进程内约定的时钟常量（见 txn_chaos_writer.cpp）
constexpr std::uint64_t kStartTs = 7000;
constexpr std::uint64_t kCommitTs = 7100;
constexpr std::uint64_t kNowExpired = 100000;  // 远超 ttl(3000)+created(1000) → 锁必判过期

struct Side {
    std::unique_ptr<Engine> engine;  // reopen 后持有
    std::string key;
};

// 可见性：读到值=可见；空值/锁冲突=不可见；其他错误=测试失败（不允许静默吞掉）
bool visible(MvccTxn& mvcc, const std::string& key, int round, std::string_view what) {
    auto got = mvcc.get(key, Ts{kCommitTs + 1000});
    if (!got.has_value()) {
        EXPECT_TRUE(std::holds_alternative<tinykv::LockConflict>(got.error()))
            << "第 " << round << " 轮 " << what << " 读到非锁错误: "
            << tinykv::describe(got.error());
        return false;
    }
    return got->has_value();
}

// 半途决议：只清一个引擎的锁（模拟"决议广播被打断"）。forward=true 按 committed 推进。
void resolve_one(Side& side, const std::string& primary, bool forward, int round) {
    MvccTxn mvcc(*side.engine);
    const auto r = forward
        ? mvcc.resolve_locks_of(primary, Ts{kStartTs}, Ts{kCommitTs})
        : mvcc.resolve_locks_of(primary, Ts{kStartTs}, std::nullopt);
    ASSERT_TRUE(r.has_value()) << "第 " << round << " 轮半途决议失败: "
                               << tinykv::describe(r.error());
}

}  // namespace

TEST(CrossRegionChaosRandom, RandomKillPointsAlwaysConvergeToAtomicVisibility) {
    int committed_rounds = 0;
    int aborted_rounds = 0;

    for (int round = 0; round < kRounds; ++round) {
        std::mt19937 rng(kSeedBase + static_cast<std::uint32_t>(round));
        const int kill_step = static_cast<int>(rng() % 5);   // 0..4：协议任意步之后死亡
        const bool primary_in_region1 = (rng() % 2) == 0;    // 主组侧随机，防"只有一种布局"
        const bool partial_resolution = (rng() % 3) == 0;    // 1/3 轮先做一次半途决议
        const bool partial_first = (rng() % 2) == 0;         // 半途决议先清哪一侧

        TempDir tmp{"tinykv_cross_chaos_rand"};
        const std::string dir1 = (tmp.path() / "r1").generic_string();
        const std::string dir2 = (tmp.path() / "r2").generic_string();
        const std::string primary_key = primary_in_region1 ? "pa" : "pz";
        const std::string other_key = primary_in_region1 ? "oz" : "oa";

        // 子进程：执行 2PC 前 kill_step 步后强杀（退出码 0，靠 stdout 判定形态）。
        // 目录必须与主组侧匹配：子进程签名是 <主组目录> <副组目录>，主组侧随机由
        // "把哪对 (目录, key) 当主组" 决定——两种布局都要覆盖。
        const std::string& dir_primary = primary_in_region1 ? dir1 : dir2;
        const std::string& dir_other = primary_in_region1 ? dir2 : dir1;
        const std::string cmd = std::string(TXN_CHAOS_WRITER_PATH) + " \"" + dir_primary +
                                "\" \"" + dir_other + "\" \"" + primary_key + "\" \"" +
                                other_key + "\" " + std::to_string(kill_step);
        const int rc = std::system(cmd.c_str());
        ASSERT_EQ(rc, 0) << "第 " << round << " 轮崩溃写入器失败 rc=" << rc << " cmd=" << cmd;

        // 父进程：重开两个引擎——只可能看到"已 fsync 的前 kill_step 步"
        auto e1 = Engine::open({.dir = dir1});
        auto e2 = Engine::open({.dir = dir2});
        ASSERT_TRUE(e1.has_value());
        ASSERT_TRUE(e2.has_value());
        Side side1{std::move(*e1), primary_in_region1 ? primary_key : other_key};
        Side side2{std::move(*e2), primary_in_region1 ? other_key : primary_key};
        Side& primary_side = primary_in_region1 ? side1 : side2;
        Side& other_side = primary_in_region1 ? side2 : side1;

        // 1/3 轮注入"半途决议"：广播只到达一侧就被打断，剩余锁靠第二次收敛补齐
        if (partial_resolution) {
            Side& first = partial_first ? primary_side : other_side;
            resolve_one(first, primary_key, kill_step >= 3, round);
        }

        // 收敛：读遇锁该做的事——查 primary 真实状态 → 正向推进或反向清锁（红线 3）
        {
            MvccTxn primary_view(*primary_side.engine);
            const auto st = primary_view.check_txn_status(primary_key, Ts{kStartTs}, kNowExpired);
            ASSERT_TRUE(st.has_value()) << "第 " << round << " 轮查 primary 状态失败: "
                                        << tinykv::describe(st.error());
            const bool forward = st->committed;
            for (const Side* side : {&primary_side, &other_side}) {
                MvccTxn mvcc(*side->engine);
                const auto r = forward
                    ? mvcc.resolve_locks_of(primary_key, Ts{kStartTs}, st->commit_ts)
                    : mvcc.resolve_locks_of(primary_key, Ts{kStartTs}, std::nullopt);
                ASSERT_TRUE(r.has_value()) << "第 " << round << " 轮决议失败: "
                                           << tinykv::describe(r.error());
            }
            // 预期终态：kill_step>=3（主组已提交）→ 全可见；否则全不可见
            const bool expect_visible = kill_step >= 3;
            if (expect_visible) {
                ++committed_rounds;
            } else {
                ++aborted_rounds;
            }
            // 断言原子可见性：两 key 可见性一致且等于 kill_step 对应的预期终态
            MvccTxn other_view(*other_side.engine);
            const bool seen_p = visible(primary_view, primary_key, round, "primary");
            const bool seen_o = visible(other_view, other_key, round, "other");
            EXPECT_EQ(seen_p, seen_o) << "第 " << round << " 轮半提交（p=" << seen_p
                                      << " o=" << seen_o << " kill_step=" << kill_step << "）";
            EXPECT_EQ(seen_p, expect_visible)
                << "第 " << round << " 轮终态与 kill_step 预期不符（kill_step=" << kill_step
                << " seen=" << seen_p << "）";
        }
    }
    // 终态两类都必须出现：没有提交轮=收敛逻辑从未走正向；没有回滚轮=中断分支没测到
    EXPECT_GT(committed_rounds, 0) << "150 轮没有任何一轮提交成功，用例失去意义";
    EXPECT_GT(aborted_rounds, 0) << "150 轮没有任何一轮回滚收敛，中断分支未覆盖";
}
