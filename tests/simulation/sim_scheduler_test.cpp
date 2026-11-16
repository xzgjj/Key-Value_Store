// 文件用途：PD-lite 调度器模拟用例（M4 第⑥层，FUNC-12，SPEC-3 §3.7 sim_scheduler_test）。
// 归属：TinyKV-CPP（M4 Multi-Raft 控制面）。
// 使用场景理解：调度器只动元数据——收集 store/region 心跳、按 size 差给迁移建议、AskSplit 预分配 id。
// 三条必须证明的边界：(a) 心跳能正确汇总；(b) AskSplit 分配 id 单调且 peer 数等长；
// (c) size 差超阈值给出迁移建议、迁移后收敛（不再建议），且限速生效。

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "scheduler/scheduler.h"

namespace {

using tinykv::scheduler::BalanceConfig;
using tinykv::scheduler::Scheduler;

// 契约包名是 tinykv.schedulerpb（proto/scheduler.proto），此处只需短别名，不改契约。
namespace schedulerpb = tinykv::schedulerpb;

schedulerpb::StoreHeartbeatReq store_hb(std::uint64_t id, std::uint64_t size,
                                        std::uint32_t regions) {
    schedulerpb::StoreHeartbeatReq req;
    req.set_store_id(id);
    req.set_total_size(size);
    req.set_region_count(regions);
    req.set_capacity(1U << 30U);
    return req;
}

schedulerpb::RegionHeartbeatReq region_hb(std::uint64_t id, std::uint64_t size,
                                          const std::vector<std::uint64_t>& stores) {
    schedulerpb::RegionHeartbeatReq req;
    req.set_region_id(id);
    req.set_approximate_size(size);
    req.mutable_epoch()->set_version(1);
    req.mutable_epoch()->set_conf_ver(1);
    for (const auto s : stores) {
        auto* p = req.add_peers();
        p->set_id(100 + s);
        p->set_store_id(s);
    }
    return req;
}

// 心跳汇总：store/region 计数与内容正确
TEST(Scheduler, CollectsHeartbeats) {
    Scheduler sch;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        sch.on_store_heartbeat(store_hb(id, 100 * id, static_cast<std::uint32_t>(id)));
    }
    sch.on_region_heartbeat(region_hb(7, 500, {1, 2, 3}));
    sch.on_region_heartbeat(region_hb(8, 300, {1, 2}));

    EXPECT_EQ(sch.store_count(), 3U);
    EXPECT_EQ(sch.region_count(), 2U);
    ASSERT_TRUE(sch.stores().count(2) == 1);
    EXPECT_EQ(sch.stores().at(2).total_size, 200U);
}

// AskSplit：新 region id 单调递增、不与已见 region/peer 撞号、peer_ids 与副本数等长
TEST(Scheduler, AskSplitAllocatesIds) {
    Scheduler sch;
    sch.on_store_heartbeat(store_hb(1, 100, 1));
    sch.on_region_heartbeat(region_hb(5, 400, {1}));   // 已见 region 5，peer id=101

    schedulerpb::AskSplitReq req;
    req.set_region_id(5);
    req.set_split_key("m");
    auto r1 = sch.ask_split(req, 3);
    auto r2 = sch.ask_split(req, 3);

    EXPECT_GT(r1.new_region_id(), 5U) << "新 region id 必须大于已见最大 region id，不得撞号";
    EXPECT_GT(r2.new_region_id(), r1.new_region_id()) << "新 region id 必须单调递增";
    EXPECT_EQ(r1.new_peer_ids_size(), 3) << "新 peer id 数必须与副本数等长";
    EXPECT_EQ(r2.new_peer_ids_size(), 3);
    EXPECT_NE(r1.new_peer_ids(0), r1.new_peer_ids(1));
    EXPECT_GT(r1.new_peer_ids(0), 101U) << "新 peer id 必须大于已见最大 peer id，不得撞号";
    EXPECT_EQ(sch.split_count(), 2U);
}

// size 均衡：差超阈值给建议，且每迁一个差就收窄，最终无建议（单调收敛）
TEST(Scheduler, BalanceConvergesAfterMoves) {
    Scheduler sch;
    BalanceConfig cfg;
    cfg.trigger_diff = 0.2;
    cfg.max_moves_per_round = 1;

    // store1 2000、store2 空（新建 store 上报 0 必须被当成 0，不能被回填）
    sch.on_store_heartbeat(store_hb(1, 1800, 3));
    sch.on_store_heartbeat(store_hb(2, 0, 0));
    sch.on_region_heartbeat(region_hb(9, 600, {1, 2}));   // store2 已有副本 → 不该迁这个
    sch.on_region_heartbeat(region_hb(10, 600, {1}));     // 只在 store1 → 可迁
    sch.on_region_heartbeat(region_hb(11, 600, {1}));     // 同上

    // 第 1 轮：1800 vs 0（差 2.0×avg）→ 迁 region 10
    auto r1 = sch.balance(cfg);
    ASSERT_EQ(r1.size(), 1U) << "size 差 1800 vs 0 应触发一次迁移建议";
    EXPECT_EQ(r1[0].from_store, 1U);
    EXPECT_EQ(r1[0].to_store, 2U);
    EXPECT_EQ(r1[0].region_id, 10U) << "有 store2 副本的 region 9 不应被迁";

    // 第 2 轮：迁完 1200 vs 600（差 0.67×avg）仍超阈值 → 再迁 region 11
    sch.on_region_heartbeat(region_hb(10, 600, {2}));
    sch.on_store_heartbeat(store_hb(1, 1200, 2));
    sch.on_store_heartbeat(store_hb(2, 600, 1));
    auto r2 = sch.balance(cfg);
    ASSERT_EQ(r2.size(), 1U) << "尚未收敛时仍应继续给建议";
    EXPECT_EQ(r2[0].region_id, 11U);

    // 第 3 轮：900 vs 900 相等 → 不再建议
    sch.on_region_heartbeat(region_hb(11, 600, {2}));
    sch.on_store_heartbeat(store_hb(1, 900, 1));
    sch.on_store_heartbeat(store_hb(2, 900, 2));
    EXPECT_TRUE(sch.balance(cfg).empty()) << "迁移后 size 已相等，不应再建议迁移";
}

// 限速：max_moves_per_round 生效
TEST(Scheduler, BalanceRespectsRateLimit) {
    Scheduler sch;
    sch.on_store_heartbeat(store_hb(1, 1000, 2));
    sch.on_store_heartbeat(store_hb(2, 0, 0));
    sch.on_region_heartbeat(region_hb(20, 500, {1}));
    sch.on_region_heartbeat(region_hb(21, 500, {1}));

    BalanceConfig cfg;
    cfg.trigger_diff = 0.2;
    cfg.max_moves_per_round = 1;
    EXPECT_EQ(sch.balance(cfg).size(), 1U);
}

}  // namespace
