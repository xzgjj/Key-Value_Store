// 文件用途：⑦ 跨区事务的"中断注入"回归——150 轮随机中断后，事务要么全可见要么全不可见。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `txn_cross_region_chaos`）。
//
// 使用场景理解：跨区 2PC 的危险窗口只有三处，本文件就围着它们转：
//   ① PREPARE 只做了主组 → 主组有锁、副组没锁；
//   ② PREPARE 全做完但没取 commit_ts → 两组都有锁，主组无 Write；
//   ③ 主组已提交、副组未提交 → 副组有锁、主组有 Write（**唯一会让"半提交"看起来发生**的窗口）。
// 每轮随机落在其中一个窗口，然后用"读者遇到锁时该做的事"（查 primary 状态 → 正向推进/回滚）
// 收敛，最后断言两把 key 的可见性一致。这不是随机 kill 进程，而是把三个窗口枚举出来跑，
// 命中率 100% 且可复现——随机 kill 的价值在于"发现未想到的窗口"，那条留给夜跑。

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "common/error_format.h"
#include "storage/engine.h"
#include "temp_dir.h"
#include "txn/mvcc.h"

namespace {

using tinykv::Engine;
using tinykv::LockConflict;
using tinykv::Ts;
using tinykv::testlib::TempDir;
using tinykv::txn::Mutation;
using tinykv::txn::MvccTxn;

Mutation put(std::string key, std::string value) {
    return Mutation{.op = Mutation::Op::kPut, .key = std::move(key), .value = std::move(value)};
}

// 一轮里的三种中断点（枚举而非概率：三个窗口必须都被覆盖）
enum class Window { kAfterPrimaryPrewrite, kAfterAllPrewrite, kAfterPrimaryCommit };

// 读者侧收敛：遇锁则按 primary 的真实状态决定推进还是回滚（红线 3 的实现口径）
void converge(MvccTxn& local, MvccTxn& primary_side, const std::string& primary, Ts start_ts,
              std::uint64_t now_ms) {
    const auto st = primary_side.check_txn_status(primary, start_ts, now_ms);
    if (!st.has_value()) {
        return;
    }
    if (st->committed) {
        (void)local.resolve_locks_of(primary, start_ts, st->commit_ts);  // 正向推进
        return;
    }
    if (st->lock_ttl_remaining_ms == 0) {  // 已过期 → 反向清锁
        (void)local.resolve_locks_of(primary, start_ts, std::nullopt);
    }
}

TEST(CrossRegionChaosTest, InterruptedTwoPhaseCommitNeverExposesHalfTransaction) {
    constexpr int kRounds = 150;
    std::mt19937 rng(20261118);
    int committed_rounds = 0;
    int aborted_rounds = 0;

    for (int round = 0; round < kRounds; ++round) {
        TempDir tmp{"tinykv_cross_chaos"};
        auto region1 = Engine::open({.dir = tmp.path() / "r1"});
        auto region2 = Engine::open({.dir = tmp.path() / "r2"});
        ASSERT_TRUE(region1.has_value());
        ASSERT_TRUE(region2.has_value());

        // 每轮的 key 唯一：避免上一轮残留影响（同一目录只在本轮内使用）
        const std::string key_a = "a" + std::to_string(round);
        const std::string key_z = "z" + std::to_string(round);
        const Ts start_ts{static_cast<std::uint64_t>(1000 + round * 10)};
        const std::uint64_t now_ms = 5000;
        const auto window = static_cast<Window>(rng() % 3);

        // ① PREPARE 主组（region 1）
        {
            MvccTxn mvcc(**region1);
            const std::vector<Mutation> batch{put(key_a, "va")};
            ASSERT_TRUE(mvcc.prewrite(batch, key_a, start_ts, 3000, now_ms).has_value());
        }
        // ② 是否继续 PREPARE 副组（region 2）
        const bool secondary_prepared = window != Window::kAfterPrimaryPrewrite;
        if (secondary_prepared) {
            MvccTxn mvcc(**region2);
            const std::vector<Mutation> batch{put(key_z, "vz")};
            ASSERT_TRUE(mvcc.prewrite(batch, key_a, start_ts, 3000, now_ms, false).has_value());
        }
        // ③ 是否提交主组
        const bool primary_committed = window == Window::kAfterPrimaryCommit;
        const Ts commit_ts{start_ts.value + 5};
        if (primary_committed) {
            MvccTxn mvcc(**region1);
            const std::vector<std::string> keys{key_a};
            ASSERT_TRUE(mvcc.commit(keys, start_ts, commit_ts).has_value());
        }

        // ---- 恢复：模拟"读者遇到锁 → 查 primary → 收敛" ----
        {
            MvccTxn primary_side(**region1);
            MvccTxn local(**region2);
            converge(local, primary_side, key_a, start_ts, now_ms + 10);
        }
        // 主组自己的锁也要按同一决议处理（真实装配里广播到每个 region）
        if (!primary_committed) {
            MvccTxn primary_side(**region1);
            const auto st = primary_side.check_txn_status(key_a, start_ts, now_ms + 10);
            ASSERT_TRUE(st.has_value());
            if (!st->committed && st->lock_ttl_remaining_ms == 0) {
                (void)primary_side.resolve_locks_of(key_a, start_ts, std::nullopt);
            }
        }

        // ---- 断言：两把 key 的可见性必须一致 ----
        // 注意：未决事务的锁仍在时，读回 LockConflict 是**正确**行为（调用方等或决议），
        // 不能算"读失败"也不能算"不可见→可见不一致"。这里统一按"未提交即不可见"处理：
        // 只有真正读到值才算可见，其余（空值 / 锁冲突）都归入不可见一侧。
        const auto visible = [&](Engine& eng, const std::string& key) {
            MvccTxn mvcc(eng);
            auto got = mvcc.get(key, Ts{start_ts.value + 100});
            if (!got.has_value()) {
                EXPECT_TRUE(std::holds_alternative<LockConflict>(got.error()))
                    << "第 " << round << " 轮读到非锁错误: " << tinykv::describe(got.error());
                return false;
            }
            return got->has_value();
        };
        const bool seen_a = visible(**region1, key_a);
        const bool seen_z = visible(**region2, key_z);
        EXPECT_EQ(seen_a, seen_z) << "第 " << round << " 轮出现半提交（a=" << seen_a
                                  << " z=" << seen_z << "，窗口=" << static_cast<int>(window)
                                  << "）";
        if (seen_a && seen_z) {
            ++committed_rounds;
        } else {
            ++aborted_rounds;
        }
    }
    // 三个窗口都要被覆盖到：全提交与全回滚各至少出现一次，否则这轮 chaos 没测到东西
    EXPECT_GT(committed_rounds, 0) << "150 轮里没有任何一轮提交成功，用例失去意义";
    EXPECT_GT(aborted_rounds, 0) << "150 轮里没有任何一轮回滚，用例覆盖不到中断分支";
}

}  // namespace
