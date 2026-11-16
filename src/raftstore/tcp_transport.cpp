// 文件用途：tcp_transport.h 的实现——accept 循环、分帧读写、异步发送。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-03/04）。

#include "raftstore/tcp_transport.h"

#include <array>
#include <utility>

namespace tinykv::raftstore {
namespace {

constexpr std::size_t kLenBytes = 4;
constexpr std::uint32_t kMaxFrame = 64U << 20U;   // 单帧上限，防坏长度导致巨量分配

std::string frame_of(const std::string& payload) {
    std::string frame;
    frame.reserve(kLenBytes + payload.size());
    const auto len = static_cast<std::uint32_t>(payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((len >> shift) & 0xFFU));   // 大端长度
    }
    frame.append(payload);
    return frame;
}

std::uint32_t length_of(const std::array<char, kLenBytes>& head) {
    std::uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        len = (len << 8) | static_cast<std::uint8_t>(head[static_cast<std::size_t>(i)]);
    }
    return len;
}

}  // namespace

result<std::unique_ptr<TcpTransport>> TcpTransport::start(const TcpOptions& opt) {
    auto transport = std::unique_ptr<TcpTransport>(new TcpTransport(opt.listen_host));   // NOLINT

    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(opt.listen_host, ec),
                                           opt.listen_port);
    if (ec) {
        return std::unexpected(IoFailed{.path = opt.listen_host, .reason = "监听地址非法"});
    }
    transport->acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(transport->ctx_);
    transport->acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = opt.listen_host, .reason = "open 失败: " + ec.message()});
    }
    transport->acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    transport->acceptor_->bind(endpoint, ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = opt.listen_host,
                                        .reason = "bind 失败（端口可能被占用）: " + ec.message()});
    }
    transport->acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = opt.listen_host, .reason = "listen 失败: " + ec.message()});
    }
    transport->bound_port_ = transport->acceptor_->local_endpoint(ec).port();

    transport->guard_ = std::make_unique<
        asio::executor_work_guard<asio::io_context::executor_type>>(transport->ctx_.get_executor());
    transport->accept_loop();
    transport->thread_ = std::thread([raw = transport.get()] { raw->ctx_.run(); });
    return transport;
}

TcpTransport::~TcpTransport() {
    if (acceptor_ != nullptr) {
        asio::error_code ec;
        acceptor_->close(ec);
    }
    guard_.reset();          // 释放 work guard，run() 才会自然返回
    ctx_.stop();
    if (thread_.joinable()) {
        thread_.join();      // 绝不 detach：线程生命周期由对象负责
    }
}

void TcpTransport::set_peers(const std::map<std::uint64_t, std::uint16_t>& peer_ports) {
    std::lock_guard lock(peers_mu_);
    peer_ports_ = peer_ports;
}

void TcpTransport::accept_loop() {
    auto sock = std::make_shared<asio::ip::tcp::socket>(ctx_);
    acceptor_->async_accept(*sock, [this, sock](const asio::error_code& ec) {
        if (!ec) {
            read_frame(sock);
        }
        if (acceptor_ != nullptr && acceptor_->is_open()) {
            accept_loop();   // 继续接下一个
        }
    });
}

void TcpTransport::read_frame(std::shared_ptr<asio::ip::tcp::socket> sock) {
    auto head = std::make_shared<std::array<char, kLenBytes>>();
    asio::async_read(*sock, asio::buffer(*head),
                     [this, sock, head](const asio::error_code& ec, std::size_t) {
                         if (ec) {
                             asio::error_code ig;
                             sock->close(ig);
                             return;
                         }
                         const auto len = length_of(*head);
                         if (len == 0 || len > kMaxFrame) {
                             asio::error_code ig;
                             sock->close(ig);
                             return;
                         }
                         auto body = std::make_shared<std::string>(len, '\0');
                         asio::async_read(*sock, asio::buffer(*body),
                                          [this, sock, body](const asio::error_code& err2,
                                                             std::size_t) {
                                              if (!err2) {
                                                  raftpb::Message msg;
                                                  if (msg.ParseFromString(*body)) {
                                                      std::lock_guard lock(inbox_mu_);
                                                      inbox_.push_back(std::move(msg));
                                                  }
                                              }
                                              read_frame(sock);   // 继续读下一帧（同一条连接可复用）
                                          });
                     });
}

result<void> TcpTransport::send(PeerId to, const raftpb::Message& msg) {
    std::uint16_t port = 0;
    {
        std::lock_guard lock(peers_mu_);
        const auto it = peer_ports_.find(to.value);
        if (it == peer_ports_.end()) {
            return std::unexpected(InvalidArgument{"未知对端 PeerId=" + std::to_string(to.value)});
        }
        port = it->second;
    }

    std::string payload;
    if (!msg.SerializeToString(&payload)) {
        return std::unexpected(Corrupted{.where = "tcp:send", .reason = "消息序列化失败"});
    }
    auto frame = std::make_shared<std::string>(frame_of(payload));
    const std::string host = host_;

    // 全异步：Raft 线程只投递，不等网络。连接失败直接放弃——Raft 会重传。
    asio::post(ctx_, [this, frame, host, port] {
        asio::error_code addr_ec;
        const auto addr = asio::ip::make_address(host, addr_ec);
        if (addr_ec) {
            return;
        }
        auto sock = std::make_shared<asio::ip::tcp::socket>(ctx_);
        const asio::ip::tcp::endpoint ep(addr, port);
        sock->async_connect(ep, [sock, frame](const asio::error_code& ec) {
            if (ec) {
                return;   // 对端未起：丢弃，等下一轮心跳重传
            }
            asio::async_write(*sock, asio::buffer(*frame),
                              [sock](const asio::error_code&, std::size_t) {
                                  asio::error_code ig;
                                  sock->shutdown(asio::ip::tcp::socket::shutdown_both, ig);
                                  sock->close(ig);
                              });
        });
    });
    return {};
}

std::vector<raftpb::Message> TcpTransport::poll() {
    std::vector<raftpb::Message> out;
    std::lock_guard lock(inbox_mu_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
    return out;
}

}  // namespace tinykv::raftstore
