// 文件用途：多节点 TcpTransport 启动助手——端口自动分配并互相 set_peers。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
// 抽出来的理由：integration 与 bench 两处都要起 3 个节点，复制粘贴已被 CI 判重复。

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "raftstore/tcp_transport.h"

namespace tinykv::testlib {

// 启动 n 个传输层（listen_port = 0 由系统分配），返回 PeerId → 端口；失败项不出现在结果里
[[nodiscard]] inline std::map<std::uint64_t, std::uint16_t> start_transports(
    int n, std::vector<std::unique_ptr<tinykv::raftstore::TcpTransport>>& out) {
    std::map<std::uint64_t, std::uint16_t> ports;
    for (int i = 0; i < n; ++i) {
        auto started = tinykv::raftstore::TcpTransport::start(
            {.self = PeerId{static_cast<std::uint64_t>(i) + 1}, .listen_port = 0});
        if (!started.has_value()) {
            continue;
        }
        ports[static_cast<std::uint64_t>(i) + 1] = (*started)->port();
        out.push_back(std::move(*started));
    }
    for (auto& t : out) {
        t->set_peers(ports);
    }
    return ports;
}

}  // namespace tinykv::testlib
