// 文件用途：common/error 结构化错误单元测试——六变体字段与 UX-03 三要素（SPEC-0 §0.3.1/§0.7）。
// 归属：TinyKV-CPP（M0 基建，ENG-01/UX-03）。
// 使用场景理解：KvError 每个变体都必须能回答 what/why/hint 三问（字段即答案），
// 上游据此决定重试/刷新路由/重定向；本测试按 §0.3.1 触发条件矩阵逐变体断言字段可达。

#include "common/error.h"

#include <gtest/gtest.h>

#include <variant>

namespace {

using namespace tinykv;  // NOLINT(google-build-using-namespace) —— 测试内便捷引用

// ---------- result 别名：错误必须可显式取用（清单 #5/#6） ----------

TEST(KvError, ResultCarriesValueOrError) {
    result<int> ok = 42;
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(ok.value(), 42);

    result<int> err = std::unexpected(InvalidArgument{"limit < 0"});
    ASSERT_FALSE(err.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(err.error()));
}

// ---------- 六变体字段可达性（UX-03：what/why/hint 全在字段里） ----------

TEST(KvError, KeyNotInRegionCarriesKeyAndRegion) {
    const KvError e = KeyNotInRegion{.key = "user-a", .region_id = RegionId{1}};
    const auto& v = std::get<KeyNotInRegion>(e);
    EXPECT_EQ(v.key, "user-a");
    EXPECT_EQ(v.region_id, RegionId{1});
}

TEST(KvError, StaleEpochCarriesHaveAndWant) {
    // 触发条件：请求 epoch 落后 region 当前 epoch；hint = 携带 have/want 供刷新路由
    const KvError e = StaleEpoch{.region_id = RegionId{7},
                                 .have = RegionEpoch{.version = 1, .conf_ver = 0},
                                 .want = RegionEpoch{.version = 2, .conf_ver = 1}};
    const auto& v = std::get<StaleEpoch>(e);
    EXPECT_EQ(v.region_id, RegionId{7});
    EXPECT_LT(v.have, v.want);  // 落后事实可判定
    EXPECT_NE(v.have, v.want);
}

TEST(KvError, NotLeaderHintIsOptional) {
    // hint = leader_hint 供直接重定向；未知时 must be nullopt（不伪造 hint）
    const KvError no_hint = NotLeader{.region_id = RegionId{3}, .leader_hint = std::nullopt};
    EXPECT_FALSE(std::get<NotLeader>(no_hint).leader_hint.has_value());

    const KvError hinted = NotLeader{.region_id = RegionId{3}, .leader_hint = PeerId{9}};
    EXPECT_EQ(std::get<NotLeader>(hinted).leader_hint, PeerId{9});
}

TEST(KvError, IoAndCorruptionCarryLocation) {
    const KvError io = IoFailed{.path = "data/wal/seg-0001.log", .reason = "permission denied"};
    const auto& v = std::get<IoFailed>(io);
    EXPECT_EQ(v.path, "data/wal/seg-0001.log");  // what：出错位置
    EXPECT_EQ(v.reason, "permission denied");    // why：OS 原因（hint = 查磁盘/权限）

    const KvError corrupt =
        Corrupted{.where = "wal:seg-0001:offset-4096", .reason = "crc mismatch"};
    const auto& c = std::get<Corrupted>(corrupt);
    EXPECT_EQ(c.where, "wal:seg-0001:offset-4096");
    EXPECT_EQ(c.reason, "crc mismatch");
}

// ---------- 分派可用性：调用方按变体分流（重试/刷新/重定向策略各不相同） ----------

TEST(KvError, VariantDispatchByType) {
    KvError e = InvalidArgument{"key 为空"};
    const std::string rendered = std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, InvalidArgument>) {
                return "invalid: " + v.what;
            } else {
                return "unreachable";
            }
        },
        e);
    EXPECT_EQ(rendered, "invalid: key 为空");
}

}  // namespace
