// 文件用途：TCP 相关测试的统一入口头——一行引入传输助手与常用类型名。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 与 storage_test_base.h / raft_test_base.h 同理：多个用例文件各写一段 include + using
// 会被 CI 判成复制粘贴（dup_code 窗口 8 行）。集中到一处后，新用例的样板只有两行。

#pragma once

#include <raft_cmdpb.pb.h>

#include "raft_test_base.h"
#include "tcp_node_set.h"

namespace tinykv::testlib {

namespace cmdpb = ::tinykv::cmdpb;

using tinykv::raftstore::Peer;
using tinykv::raftstore::TcpOptions;
using tinykv::raftstore::TcpTransport;

constexpr int kNodes = 3;   // 本目录下所有 TCP 用例都用三节点

}  // namespace tinykv::testlib
