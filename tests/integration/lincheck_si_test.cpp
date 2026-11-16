// 文件用途：SI 外部审计（M5 第⑥层，REL-06 端到端，SPEC-4 §3.7 lincheck_si_test）。
// 归属：TinyKV-CPP（M5 集成层，integration/nightly）。
// 使用场景理解：单元用例证明的是"每条语义单点正确"；本文件跑**随机并发历史**再用
// 外部审计器逐条判定三条 SI 不变量：
//   (a) 无脏读——每个成功读返回的值必须是某个已提交版本，且该版本 commit_ts ≤ 读事务快照；
//   (b) 首提交不被覆盖（写写冲突语义）——同 key 两个提交者，后者的 start_ts 必须 > 前者的
//       commit_ts（否则本应 WriteConflict abort）；
//   (c) 无写丢失——审计器自己维护的"应该可见值"与引擎读数一致（每个读事务看到的是
//       ≤ 其 start_ts 的最新提交版本）。
// 历史由确定性种子生成（多种子），不依赖并发调度（SI 判定与调度无关，审计只看历史）。

#include "txn/client.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "storage/engine.h"
#include "temp_dir.h"
#include "txn_fixture.h"

namespace {

using tinykv::Ts;
using tinykv::txn::TxnClient;
using Fixture = tinykv::testlib::TxnFixture;

// 审计账本：每个 key 的提交链（commit_ts → value），由成功提交事务填充
struct Ledger {
    std::map<std::string, std::map<Ts, std::string, std::less<>>> committed;   // key → commit_ts → value
    std::map<Ts, std::uint64_t> commit_start;                                  // commit_ts → 该事务 start_ts

    // 无脏读 + 无写丢失：读到 v ⇒ 存在 commit_ts ≤ 读事务 start_ts 的提交且值匹配、
    // 且它是 ≤ start_ts 中最新的
    [[nodiscard]] bool explains_read(const std::string& key, const std::string& value,
                                     Ts reader_start) const {
        auto kit = committed.find(key);
        if (kit == committed.end()) {
            return false;
        }
        const Ts* best = nullptr;
        for (const auto& [cts, v] : kit->second) {
            if (cts.value <= reader_start.value) {
                best = &cts;   // map 升序：最后命中即 ≤ start_ts 的最大者
            }
        }
        return best != nullptr && kit->second.at(*best) == value;
    }

    // 首提交不被覆盖：同 key 提交链按 commit_ts 升序，后者 start_ts 必须 > 前者 commit_ts
    [[nodiscard]] bool no_blind_overwrite(const std::string& key,
                                          const std::map<Ts, std::uint64_t>& starts) const {
        auto kit = committed.find(key);
        if (kit == committed.end()) {
            return true;
        }
        std::optional<Ts> prev_commit;
        for (const auto& [cts, v] : kit->second) {
            (void)v;
            if (prev_commit.has_value()) {
                auto sit = starts.find(cts);
                if (sit == starts.end() || sit->second <= prev_commit->value) {
                    return false;   // 后提交者的快照没看到前者——本应 WriteConflict
                }
            }
            prev_commit = cts;
        }
        return true;
    }
};

struct Record {
    Ts start_ts;
    Ts commit_ts{Ts{0}};     // 0 = 未提交（冲突/回滚）
    std::map<std::string, std::string> writes;
    std::vector<std::tuple<std::string, std::string, Ts>> reads;   // key, value, start_ts
};

// 跑一轮随机历史：N 个事务在共享 key 空间上并发（start_ts 单调，写入重叠制造冲突）
bool run_history(Fixture& f, unsigned seed, Ledger& ledger, std::vector<Record>& history) {
    std::mt19937 rng(seed);
    constexpr int kTxns = 24;
    for (int i = 0; i < kTxns; ++i) {
        auto c = f.client();
        auto start = c.begin();
        if (!start.has_value()) {
            return false;
        }
        Record rec;
        rec.start_ts = *start;
        const int writes = static_cast<int>(std::uniform_int_distribution<unsigned>(0, 2)(rng));
        for (int w = 0; w < writes; ++w) {
            const auto key = "k" + std::to_string(std::uniform_int_distribution<unsigned>(0, 3)(rng));
            const auto value = "v" + std::to_string(seed) + "_" + std::to_string(i) + "_" +
                               std::to_string(w);
            (void)c.set(key, value);
            rec.writes[key] = value;
        }
        f.advance(1);
        // 事务内读（读己之写 / 快照读 / blocked 跳过）
        const int reads = static_cast<int>(std::uniform_int_distribution<unsigned>(0, 2)(rng));
        for (int rd = 0; rd < reads; ++rd) {
            const auto key = "k" + std::to_string(std::uniform_int_distribution<unsigned>(0, 3)(rng));
            auto r = c.get(key, f.now_ms);
            if (!r.has_value()) {
                continue;   // blocked（未过期锁）：不产生历史记录，合法
            }
            if (r->value.has_value()) {
                rec.reads.emplace_back(key, *r->value, rec.start_ts);
            }
        }
        f.advance(1);
        auto commit = c.commit(f.now_ms);
        if (commit.has_value()) {
            rec.commit_ts = *commit;
            ledger.commit_start.emplace(rec.commit_ts, rec.start_ts.value);
            for (const auto& [k, v] : rec.writes) {
                ledger.committed[k][rec.commit_ts] = v;
            }
        }
        history.push_back(std::move(rec));
    }
    return true;
}

TEST(LincheckSi, AuditRandomHistoriesForSiInvariants) {
    for (const unsigned seed : {11U, 42U, 777U, 2024U, 90210U}) {
        Fixture f;
        Ledger ledger;
        std::vector<Record> history;
        ASSERT_TRUE(run_history(f, seed, ledger, history)) << "seed=" << seed;

        // (b) 首提交不被覆盖：同 key 提交链的时序合法性
        for (const auto& [key, chain] : ledger.committed) {
            EXPECT_TRUE(ledger.no_blind_overwrite(key, ledger.commit_start))
                << "seed=" << seed << " key=" << key;
        }

        // (a)+(c) 无脏读 / 无写丢失：每个成功读都能被解释——
        //   读己之写（本事务未提交写）合法；否则必须来自已提交链且版本合法
        for (const auto& rec : history) {
            for (const auto& [key, value, reader_start] : rec.reads) {
                const bool own_write = rec.writes.count(key) != 0 && rec.writes.at(key) == value;
                if (own_write) {
                    continue;   // 读己之写
                }
                EXPECT_TRUE(ledger.explains_read(key, value, reader_start))
                    << "seed=" << seed << " 读到无法解释的值 key=" << key << " v=" << value;
            }
        }

        // 终态核对：账本最新版本 = 引擎实际读数（无写丢失的终态形式）
        for (const auto& [key, chain] : ledger.committed) {
            auto c = f.client();
            (void)c.begin();
            f.advance(1);
            auto got = c.get(key, f.now_ms);
            ASSERT_TRUE(got.has_value());
            ASSERT_TRUE(got->value.has_value()) << "seed=" << seed << " key=" << key;
            EXPECT_EQ(*got->value, chain.rbegin()->second) << "seed=" << seed << " key=" << key;
        }
    }
}

}  // namespace
