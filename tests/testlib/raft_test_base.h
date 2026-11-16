// 文件用途：Raft 测试的统一入口头——一行引入集群夹具与常用类型名。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 与 storage_test_base.h 同理：多个用例文件各写一段 include + using 会被 CI 判成
// 复制粘贴（dup_code 窗口 8 行，include+namespace+using 恰好越线）。

#pragma once

#include "raft_cluster.h"
#include "storage_test_base.h"

namespace tinykv::testlib {

using tinykv::LogIndex;
using tinykv::PeerId;
using tinykv::Term;
using tinykv::raft::Entry;
using tinykv::raft::Raft;
using tinykv::raft::RaftLog;
using tinykv::raftpb::Message;

}  // namespace tinykv::testlib
