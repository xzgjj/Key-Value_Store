// 文件用途：⑦ 分组内核用例——划分正确、主组定位、顺序确定、路由缺失即报错。
// 归属：TinyKV-CPP（M7 演进 ⑦ 第一步）。
//
// 使用场景理解：跨区 2PC 的错误最终都会表现成"某一组没提交/提交错组"，
// 而分组的唯一职责是把每个 mutation 放进且只放进一个组，并指出 primary 在哪组。
// 这三件事一旦错，后面状态机写得再对也没用——所以这里把分组单独钉住。

#include "txn/region_grouper.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using tinykv::InvalidArgument;
using tinykv::KeyNotInRegion;
using tinykv::RegionId;
using tinykv::txn::group_by_region;
using tinykv::txn::Mutation;

Mutation put(std::string key, std::string value = "v") {
    return Mutation{.op = Mutation::Op::kPut, .key = std::move(key), .value = std::move(value)};
}

// 测试用路由：key < "m" 属 region 1，其余属 region 2（与 split 后的布局同构）
std::optional<RegionId> route_two_regions(std::string_view key) {
    return key < "m" ? std::optional<RegionId>{RegionId{1}} : std::optional<RegionId>{RegionId{2}};
}

TEST(RegionGrouperTest, SplitsMutationsByRegionAndLocatesPrimary) {
    const std::vector<Mutation> muts{put("a"), put("z"), put("b")};
    auto grouped = group_by_region(muts, "a", route_two_regions);
    ASSERT_TRUE(grouped.has_value());
    EXPECT_EQ(grouped->region_count(), 2U);
    EXPECT_EQ(grouped->primary_region.value, 1U);
    EXPECT_EQ(grouped->primary_index, 0U);
    // region 1 组：[a, b]（保持输入相对顺序）；region 2 组：[z]
    ASSERT_EQ(grouped->groups[0].mutations.size(), 2U);
    EXPECT_EQ(grouped->groups[0].mutations[0].key, "a");
    EXPECT_EQ(grouped->groups[0].mutations[1].key, "b");
    ASSERT_EQ(grouped->groups[1].mutations.size(), 1U);
    EXPECT_EQ(grouped->groups[1].mutations[0].key, "z");
    // 划分不重不漏
    std::size_t total = 0;
    for (const auto& g : grouped->groups) {
        total += g.mutations.size();
    }
    EXPECT_EQ(total, muts.size());
}

TEST(RegionGrouperTest, SingleRegionIsReportedSoCallerCanSkipCoordinator) {
    const std::vector<Mutation> muts{put("a"), put("b")};
    auto grouped = group_by_region(muts, "a", route_two_regions);
    ASSERT_TRUE(grouped.has_value());
    EXPECT_TRUE(grouped->single_region()) << "单区必须能被识别出来（走原路径，零额外开销）";
    EXPECT_EQ(grouped->primary_index, 0U);
}

// primary 落在第二组时，primary_index 必须指向它——否则"提交锚点"会打错组
TEST(RegionGrouperTest, PrimaryInLaterGroupIsIndexedCorrectly) {
    const std::vector<Mutation> muts{put("a"), put("z")};
    auto grouped = group_by_region(muts, "z", route_two_regions);
    ASSERT_TRUE(grouped.has_value());
    EXPECT_EQ(grouped->primary_region.value, 2U);
    ASSERT_EQ(grouped->primary_index, 1U);
    EXPECT_EQ(grouped->groups[grouped->primary_index].region_id.value, 2U);
}

TEST(RegionGrouperTest, MissingRouteIsAnErrorNotAGuess) {
    const std::vector<Mutation> muts{put("a")};
    auto blocked =
        group_by_region(muts, "a", [](std::string_view) { return std::optional<RegionId>{}; });
    ASSERT_FALSE(blocked.has_value()) << "路由缺失不得兜底到某个 region";
    EXPECT_TRUE(std::holds_alternative<KeyNotInRegion>(blocked.error()));
}

TEST(RegionGrouperTest, PrimaryMustBeAmongMutations) {
    const std::vector<Mutation> muts{put("a")};
    auto bad = group_by_region(muts, "z", route_two_regions);  // z 属 region 2，不在 mutations
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(bad.error()));
}

TEST(RegionGrouperTest, EmptyMutationsRejected) {
    const std::vector<Mutation> none;
    auto bad = group_by_region(none, "a", route_two_regions);
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(bad.error()));
}

}  // namespace
