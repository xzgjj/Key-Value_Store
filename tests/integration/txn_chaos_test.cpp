// 文件用途：事务 + Multi-Raft 叠加 chaos 150 轮（M5 第⑥层，SPEC-4 §3.7 txn_chaos_n150）。
// 归属：TinyKV-CPP（M5 集成层，integration/nightly）。
// 使用场景理解：M4 的 mr_chaos_n150 注入的是 split+confchange；本文件在**事务层**叠加：
// 每轮随机生成并发事务历史（写写冲突/同 key 竞争/崩溃 prewrite 残留锁 + TTL 决议），
// 全部走经 raft 的执行端（PeerTxnExecutor），断言：
//   (a) 每轮结束时三副本的已提交数据一致（共识复制的 MVCC 状态收敛）；
//   (b) 崩溃残留锁经决议后全部清除（红线3：不存在永久死锁锁）；
//   (c) SI 终态不变量：账本可解释全部提交（无写丢失/无脏写中间态）。

#include "peer_txn.h"
#include "common/error_format.h"
#include "txn/client.h"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "peer_cluster.h"
#include "txn_fixture.h"

namespace {

using tinykv::Ts;
using tinykv::testlib::PeerCluster;
using tinykv::testlib::PeerTxnExecutor;
using tinykv::txn::TxnClient;
using tinykv::tso::MonotonicTsProvider;

constexpr int kRounds = 150;

struct RoundFixture {
    PeerCluster cluster{3};
    MonotonicTsProvider tso;
    std::uint64_t now_ms{0};
    std::unique_ptr<PeerTxnExecutor> exec;

    RoundFixture() {
        EXPECT_TRUE(cluster.elect());
        exec = std::make_unique<PeerTxnExecutor>(cluster);
    }

    [[nodiscard]] TxnClient client() { return TxnClient(*exec, tso); }

    void tick_and_pump(int rounds = 3) {
        for (int i = 0; i < rounds; ++i) {
            for (int n = 0; n < cluster.size(); ++n) {
                cluster.peer(n).tick();
            }
            cluster.pump();
        }
    }

    [[nodiscard]] std::string read_via_peer(int node, const std::string& key, Ts snapshot) {
        auto r = cluster.peer(node).mvcc().get(key, snapshot);
        if (!r.has_value() || !r->has_value()) {
            return {};
        }
        return **r;
    }
};

// 崩溃注入：只 prewrite 不 commit（锁残留）；撞上此前崩溃事务的未过期锁时按编排语义
// 先决议（回滚 blocker）再重试——blocker 字段从结构化错误拿（Peer 端直传 KvError 的原因）
std::string crash_prewrite(RoundFixture& f, const std::string& key, const std::string& value,
                           Ts start_ts, unsigned seed) {
    std::vector<tinykv::txn::Mutation> muts{
        {.op = tinykv::txn::Mutation::Op::kPut, .key = key, .value = value}};
    auto pr = f.exec->prewrite(muts, key, start_ts, 3000, f.now_ms);
    if (!pr.has_value() && std::holds_alternative<tinykv::LockConflict>(pr.error())) {
        const auto& lc = std::get<tinykv::LockConflict>(pr.error());
        auto resolved =
            f.exec->resolve_locks_of(lc.primary, Ts{lc.lock_start_ts}, std::nullopt);
        if (!resolved.has_value()) {
            return "决议失败(种子" + std::to_string(seed) + ")";
        }
        pr = f.exec->prewrite(muts, key, start_ts, 3000, f.now_ms);
    }
    if (!pr.has_value()) {
        return "prewrite 失败(种子" + std::to_string(seed) + "): " +
               std::string(tinykv::describe_kind(pr.error()));
    }
    return {};
}

// 决议全部崩溃残留锁（TTL 过期 → BatchRollback 经 raft）并断言三副本锁全清（红线3）
std::string resolve_and_verify(
    RoundFixture& f, const std::vector<std::pair<std::string, Ts>>& crashed) {
    f.now_ms += 3001;
    for (const auto& [key, st] : crashed) {
        auto r = f.exec->resolve_locks_of(key, st, std::nullopt);
        if (!r.has_value()) {
            return "resolve 失败: " + std::string(tinykv::describe_kind(r.error()));
        }
    }
    f.tick_and_pump(10);
    for (int n = 0; n < f.cluster.size(); ++n) {
        for (const auto& [key, st] : crashed) {
            auto status = f.cluster.peer(n).mvcc().check_txn_status(key, st, f.now_ms);
            if (!status.has_value()) {
                return "check_txn_status 失败(副本" + std::to_string(n) + ")";
            }
            if (!status->rolled_back) {
                return "锁未清(副本" + std::to_string(n) + " key" + key + " 红线3)";
            }
        }
    }
    return {};
}

std::string run_round(unsigned seed) {   // 空串=通过
    RoundFixture f;
    std::mt19937 rng(seed);
    std::map<std::string, std::string> committed_final;    // key → 最终提交值（账本）
    std::vector<std::pair<std::string, Ts>> crashed;        // 崩溃事务：primary → start_ts

    const int kTxns = 8;
    for (int i = 0; i < kTxns; ++i) {
        auto c = f.client();
        auto start = c.begin();
        if (!start.has_value()) {
            return "begin 失败";
        }
        const bool will_crash = std::uniform_int_distribution<unsigned>(0, 3)(rng) == 0;
        const auto key = "k" + std::to_string(std::uniform_int_distribution<unsigned>(0, 2)(rng));
        const auto value = "v" + std::to_string(seed) + "_" + std::to_string(i);
        (void)c.set(key, value);
        f.now_ms += 1;

        if (will_crash) {
            auto err = crash_prewrite(f, key, value, *start, seed);
            if (!err.empty()) {
                return err;
            }
            crashed.emplace_back(key, *start);
            continue;
        }
        auto commit = c.commit(f.now_ms);
        if (commit.has_value()) {
            committed_final[key] = value;   // 首提交者胜：同 key 后提交者会冲突
        } else if (std::holds_alternative<tinykv::IoFailed>(commit.error()) ||
                   std::holds_alternative<tinykv::Corrupted>(commit.error())) {
            return "commit 环境失败(种子" + std::to_string(seed) + ")";
        }
        // 冲突/回滚不算失败——SI 语义的一部分
    }

    f.tick_and_pump();

    // (b) 决议全部崩溃残留锁并断言三副本锁全清（红线3）
    auto resolve_err = resolve_and_verify(f, crashed);
    if (!resolve_err.empty()) {
        return resolve_err;
    }

    // (a) 已提交数据三副本一致
    for (const auto& [key, value] : committed_final) {
        for (int n = 0; n < f.cluster.size(); ++n) {
            auto got = f.read_via_peer(n, key, Ts{f.tso.current().value + 1});
            if (got != value) {
                return "副本不一致(副本" + std::to_string(n) + " key" + key +
                       " got=" + got + " want=" + value + ")";
            }
        }
    }
    return {};   // 通过
}

TEST(TxnChaosN150, SiInvariantsUnderChaosAcrossHundredFiftySeeds) {
    std::vector<unsigned> failed;
    std::string first_reason;
    for (int round = 0; round < kRounds; ++round) {
        const unsigned seed = 7000U + static_cast<unsigned>(round);
        auto reason = run_round(seed);
        if (!reason.empty()) {
            failed.push_back(seed);
            if (first_reason.empty()) {
                first_reason = reason;
            }
        }
    }
    ASSERT_TRUE(failed.empty()) << "失败种子数 " << failed.size() << "，首个失败种子 "
                                << (failed.empty() ? 0U : failed.front()) << " 原因 "
                                << first_reason;
}

}  // namespace
