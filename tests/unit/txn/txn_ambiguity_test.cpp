// 文件用途：BUG-0020（code-review r2 N-1）回归——跨区 2PC primary 提交歧义。
// 归属：TinyKV-CPP（M5 事务的 M8 后还债，LEVEL=unit → fast）。
//
// 使用场景理解：COMMIT_PRIMARY 的调用失败有两种可能——真没提交（安全回滚）或
// apply 已成功但调用方收到超时（BUG-0009 同款窗口）。旧实现无条件回滚 secondary，
// 会造出"primary 已提交 + secondary 墓碑"的部分提交，且不可自愈（正向决议遇
// rollback 墓碑永远失败）。修复后先 check_txn_status(primary) 判生死：
//   committed   → 正向推进（secondary 用判定的 commit_ts 补提交）；
//   确认未提交  → 才允许回滚 others；
//   未决        → 保持现状（锁决议/重试给最终结论）。
// 注入方式：包一层 LocalExecutor，控制 primary 组 commit 的故障形态（歧义/真失败），
// 时钟全部注入（now_ms），无需真实等待。

#include "txn/client.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error_format.h"
#include "txn_fixture.h"

namespace {

using Fixture = tinykv::testlib::TxnFixture;
using tinykv::Ts;
using tinykv::result;
using tinykv::txn::TxnClient;

// 注入执行端：组合 LocalExecutor（它是 final，只能包不能继承），控制 primary 组
// commit 调用的两种故障形态——
//   kApplyThenFail：commit 真实落盘（apply 已发生）后谎报失败——正是歧义窗口；
//   kFailNoApply：  直接失败不落盘——真未提交。
class AmbiguityExecutor final : public tinykv::txn::TxnExecutor {
public:
    explicit AmbiguityExecutor(tinykv::Storage& storage) : local_(storage) {}

    void inject_primary_timeout(const std::string& primary_key) {
        primary_key_ = primary_key;
        mode_ = Mode::kApplyThenFail;
    }
    void inject_primary_fail(const std::string& primary_key) {
        primary_key_ = primary_key;
        mode_ = Mode::kFailNoApply;
    }

    [[nodiscard]] result<std::optional<std::string>> get(std::string_view key,
                                                         Ts start_ts) override {
        return local_.get(key, start_ts);
    }
    [[nodiscard]] result<void> prewrite(tinykv::span<const tinykv::txn::Mutation> mutations,
                                        std::string_view primary, Ts start_ts,
                                        std::uint64_t ttl_ms, std::uint64_t now_ms) override {
        return local_.prewrite(mutations, primary, start_ts, ttl_ms, now_ms);
    }
    [[nodiscard]] result<void> prewrite_routed(std::string_view route_key,
                                               tinykv::span<const tinykv::txn::Mutation> mutations,
                                               std::string_view primary, Ts start_ts,
                                               std::uint64_t ttl_ms, std::uint64_t now_ms,
                                               bool cross_region) override {
        return local_.prewrite_routed(route_key, mutations, primary, start_ts, ttl_ms, now_ms,
                                      cross_region);
    }
    [[nodiscard]] result<void> commit(tinykv::span<const std::string> keys, Ts start_ts,
                                      Ts commit_ts) override {
        const bool is_primary_call = mode_ != Mode::kNone && keys.size() == 1 &&
                                     std::string(keys.front()) == primary_key_;
        if (is_primary_call) {
            const auto mode = mode_;
            mode_ = Mode::kNone;  // 只注入一次：secondary 组的正常提交不受影响
            if (mode == Mode::kApplyThenFail) {
                auto r = local_.commit(keys, start_ts, commit_ts);  // apply 真实成功
                if (!r.has_value()) {
                    return r;
                }
                return tinykv::unexpected(tinykv::IoFailed{
                    .path = "injected", .reason = "primary commit call lost (applied)"});  // 谎报
            }
            return tinykv::unexpected(tinykv::IoFailed{
                .path = "injected", .reason = "primary commit failed (not applied)"});
        }
        return local_.commit(keys, start_ts, commit_ts);
    }
    [[nodiscard]] result<void> rollback(tinykv::span<const std::string> keys,
                                        Ts start_ts) override {
        return local_.rollback(keys, start_ts);
    }
    [[nodiscard]] result<tinykv::txn::TxnStatus> check_txn_status(std::string_view primary,
                                                                  Ts lock_start_ts,
                                                                  std::uint64_t now_ms) override {
        return local_.check_txn_status(primary, lock_start_ts, now_ms);
    }
    [[nodiscard]] result<std::size_t> resolve_locks_of(std::string_view primary, Ts lock_start_ts,
                                                       std::optional<Ts> commit_to) override {
        return local_.resolve_locks_of(primary, lock_start_ts, commit_to);
    }

private:
    enum class Mode { kNone, kApplyThenFail, kFailNoApply };
    Mode mode_{Mode::kNone};
    std::string primary_key_;
    tinykv::txn::LocalExecutor local_;
};

// 路由：key < "m" → region 1，否则 region 2——两个组，primary "a"（字典序最小）在主组
std::optional<tinykv::RegionId> two_regions(std::string_view key) {
    return key < "m" ? std::optional<tinykv::RegionId>{tinykv::RegionId{1}}
                     : std::optional<tinykv::RegionId>{tinykv::RegionId{2}};
}

// 歧义主形态：primary apply 已成功但调用方收到失败 → 必须判 committed 并正向推进，
// 事务整体提交成功（旧实现：secondary 被写墓碑，部分提交 + 不可自愈）
TEST(TxnAmbiguity, PrimaryAppliedButReportLostForwardsToCommit) {
    Fixture f;
    AmbiguityExecutor exec(*f.engine);
    exec.inject_primary_timeout("a");
    TxnClient t(exec, f.tso);
    t.set_region_router(two_regions);
    (void)t.begin();
    (void)t.set("a", "va");
    (void)t.set("z", "vz");
    f.advance(10);
    const auto start = t.start_ts();
    auto r = t.commit(f.now_ms);
    ASSERT_TRUE(r.has_value())
        << "primary 实际已提交：歧义必须判为 committed 并正向推进，而不是回滚 secondary";

    // 两 key 在判定出的 commit_ts 之后可见（secondary 被补提交，不是墓碑）
    tinykv::txn::MvccTxn mvcc(*f.engine);
    const auto snap = f.alloc_ts();
    for (const std::string k : {"a", "z"}) {
        auto got = mvcc.get(k, snap);
        ASSERT_TRUE(got.has_value()) << tinykv::describe(got.error());
        ASSERT_TRUE(got->has_value()) << k << " 应随事务提交可见";
        EXPECT_EQ(**got, k == "a" ? "va" : "vz");
    }
    auto st = mvcc.check_txn_status("z", start, f.now_ms);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(st->committed) << "secondary 必须是已提交，而不是 rollback 墓碑（不可自愈态）";
    EXPECT_FALSE(st->rolled_back);
}

// 跨区双写开局（两条未决/过期用例共用，dup_code 门禁消重）：注入 primary 失败 +
// 两区路由 + begin/set/set/advance，返回已缓冲完毕的 client（primary="a"，secondary="z"）
[[nodiscard]] TxnClient begin_grouped_txn(AmbiguityExecutor& exec, Fixture& f) {
    exec.inject_primary_fail("a");
    TxnClient t(exec, f.tso);
    t.set_region_router(two_regions);
    (void)t.begin();
    (void)t.set("a", "va");
    (void)t.set("z", "vz");
    f.advance(10);
    return t;
}

// 未决形态：primary 真未提交且锁未过期 → 保持现状，secondary 不得被提前回滚
// （旧实现：无条件写墓碑——歧义若事后判 committed，墓碑让正向决议永远失败）
TEST(TxnAmbiguity, PendingPrimaryKeepsSecondaryUntouched) {
    Fixture f;
    AmbiguityExecutor exec(*f.engine);
    TxnClient t = begin_grouped_txn(exec, f);
    const auto start = t.start_ts();
    auto r = t.commit(f.now_ms);
    ASSERT_FALSE(r.has_value()) << "primary 提交失败应上抛，由调用方决议/重试";

    tinykv::txn::MvccTxn mvcc(*f.engine);
    auto st = mvcc.check_txn_status("z", start, f.now_ms);
    ASSERT_TRUE(st.has_value());
    EXPECT_FALSE(st->rolled_back) << "未决事务的 secondary 不得在歧义窗口被回滚（宁挂勿错）";
    EXPECT_FALSE(st->committed);
    EXPECT_GT(st->lock_ttl_remaining_ms, 0U) << "锁应保持活跃，等待 TTL/决议给出最终结论";
    auto st_a = mvcc.check_txn_status("a", start, f.now_ms);
    ASSERT_TRUE(st_a.has_value());
    EXPECT_FALSE(st_a->committed);
    EXPECT_GT(st_a->lock_ttl_remaining_ms, 0U);
}

// 防过修守卫：确认未提交（TTL 已尽 → remaining=0）时回滚 secondary 仍是协议允许的收尾
TEST(TxnAmbiguity, ConfirmedUncommittedPrimaryStillRollsBackSecondary) {
    Fixture f;
    AmbiguityExecutor exec(*f.engine);
    TxnClient t = begin_grouped_txn(exec, f);
    t.set_lock_ttl(0);  // 锁出生即过期：check_txn_status 返回 remaining=0 → 确认未提交分支
    const auto start = t.start_ts();
    auto r = t.commit(f.now_ms);
    ASSERT_FALSE(r.has_value());

    tinykv::txn::MvccTxn mvcc(*f.engine);
    auto st = mvcc.check_txn_status("z", start, f.now_ms);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(st->rolled_back) << "确认未提交后回滚 secondary 是安全路径，不得被过修禁掉";
}

}  // namespace
