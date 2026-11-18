// 文件用途：Peer 端事务执行器的集群适配器（M6：执行实现已上移 src/server/peer_txn.h）。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 本文件只做"多副本收敛策略"的注入：生产执行器默认等本 peer apply 到位；
// 测试要求三副本全部到位（否则读 follower 会看到提交前的锁残留旧态）。
// 单一实现留在 src/server/peer_txn.h——这里不再复制执行逻辑。

#pragma once

#include <cstdint>

#include <gtest/gtest.h>

#include "peer_cluster.h"
#include "server/peer_txn.h"

namespace tinykv::testlib {

class PeerTxnExecutor final : public tinykv::server::PeerTxnExecutor {
public:
    // region_of 可选：注入 key→region 路由后启用跨区护栏（ARCH-1 P0-1 的用例入口）
    explicit PeerTxnExecutor(PeerCluster& cluster, RegionOf region_of = {})
        : tinykv::server::PeerTxnExecutor(
              [&cluster](std::string_view) -> tinykv::raftstore::Peer& {
                  const int idx = cluster.leader();
                  if (idx < 0) {
                      ADD_FAILURE() << "事务执行需要 leader";
                      return cluster.peer(0);
                  }
                  return cluster.peer(idx);
              },
              [&cluster](std::string_view) {
                  // 测试主动驱动：双泵 + 全组 tick（生产由 Worker 后台驱动）
                  cluster.pump();
                  for (int n = 0; n < cluster.size(); ++n) {
                      cluster.peer(n).tick();
                  }
                  cluster.pump();
              },
              [&cluster](std::string_view, std::uint64_t index) {
                  // 收敛判据：全部副本 apply 到位（不只 leader）
                  for (int n = 0; n < cluster.size(); ++n) {
                      if (!cluster.peer(n).applied_at_least(index)) {
                          return false;
                      }
                  }
                  return true;
              },
              nullptr,   // 测试装置单线程泵，无需共享闸
              400, {}, std::move(region_of)) {}
};

}  // namespace tinykv::testlib
