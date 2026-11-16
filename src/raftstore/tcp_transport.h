// 文件用途：asio TCP 传输——长度前缀分帧，收侧异步推入收件箱，发侧异步短连接（SPEC-2 §2.3.1）。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-03/04）。
//
// 线程模型：一个 io_context 跑在一个 std::thread 里（**join，不 detach**，ENG-04）。
//   * 收：acceptor 异步 accept，每条连接一个 session 反复读 [u32 长度][载荷] → 解出
//     raftpb::Message → 加锁推入 inbox；
//   * 发：send() 只 post 到 io_context，绝不阻塞 Raft 线程；连接失败即丢弃，
//     由 Raft 自己的心跳重传补上（这是 Raft 的语义，不是简化）。
//
// 为什么用短连接而不是长连接池：M2 只要正确，连接池是性能优化。设计文档 §6 的替代架构里
// "短连接降级"本来就是预案之一，这里直接把它作为首版实现，长连接留给 PERF 优化阶段。
// 代价：每次发消息一次 TCP 握手。本机回环下代价可接受，跨机时会被 PERF-01 的测量照出来。

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "raftstore/transport.h"

namespace tinykv::raftstore {

struct TcpOptions {
    PeerId self{0};
    std::string listen_host{"127.0.0.1"};
    std::uint16_t listen_port{0};   // 0 = 由系统分配（测试用），绑定后可用 port() 查询
};

class TcpTransport final : public Transport {
public:
    [[nodiscard]] static result<std::unique_ptr<TcpTransport>> start(const TcpOptions& opt);

    // 所有节点起好后一次性告知对端端口（PeerId → 端口，同 listen_host）
    void set_peers(const std::map<std::uint64_t, std::uint16_t>& peer_ports);

    [[nodiscard]] result<void> send(PeerId to, const raftpb::Message& msg) override;
    [[nodiscard]] std::vector<raftpb::Message> poll() override;

    [[nodiscard]] std::uint16_t port() const { return bound_port_; }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;
    ~TcpTransport() override;

private:
    explicit TcpTransport(std::string host) : host_(std::move(host)) {}

    void accept_loop();
    void read_frame(std::shared_ptr<asio::ip::tcp::socket> sock);

    std::string host_;
    asio::io_context ctx_{1};
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::thread thread_;
    std::uint16_t bound_port_{0};

    std::mutex peers_mu_;
    std::map<std::uint64_t, std::uint16_t> peer_ports_;

    std::mutex inbox_mu_;
    std::deque<raftpb::Message> inbox_;
};

}  // namespace tinykv::raftstore
