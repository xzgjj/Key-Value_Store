// 文件用途：status_server.h 的实现——accept → 读请求头 → 回 JSON。
// 归属：TinyKV-CPP（M2 RaftKV，UX-04）。

#include "server/status_server.h"

#include <array>
#include <utility>

namespace tinykv::server {
namespace {

std::string http_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

constexpr const char* kNotFound =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

}  // namespace

result<std::unique_ptr<StatusServer>> StatusServer::start(
    const std::string& host, std::uint16_t port, std::function<std::string()> provider) {
    auto server = std::unique_ptr<StatusServer>(new StatusServer());   // NOLINT：构造私有
    server->provider_ = std::move(provider);

    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host, ec), port);
    if (ec) {
        return std::unexpected(IoFailed{.path = host, .reason = "状态端点地址非法"});
    }
    server->acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(server->ctx_);
    server->acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = host, .reason = "open 失败: " + ec.message()});
    }
    server->acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    server->acceptor_->bind(endpoint, ec);
    if (ec) {
        return std::unexpected(
            IoFailed{.path = host, .reason = "bind 失败（端口占用？）: " + ec.message()});
    }
    server->acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = host, .reason = "listen 失败: " + ec.message()});
    }
    server->bound_port_ = server->acceptor_->local_endpoint(ec).port();

    server->guard_ = std::make_unique<
        asio::executor_work_guard<asio::io_context::executor_type>>(server->ctx_.get_executor());
    server->accept_loop();
    server->thread_ = std::thread([raw = server.get()] { raw->ctx_.run(); });
    return server;
}

StatusServer::~StatusServer() {
    if (acceptor_ != nullptr) {
        asio::error_code ec;
        acceptor_->close(ec);
    }
    guard_.reset();
    ctx_.stop();
    if (thread_.joinable()) {
        thread_.join();   // 绝不 detach
    }
}

void StatusServer::accept_loop() {
    auto sock = std::make_shared<asio::ip::tcp::socket>(ctx_);
    acceptor_->async_accept(*sock, [this, sock](const asio::error_code& ec) {
        if (!ec) {
            serve(sock);
        }
        if (acceptor_ != nullptr && acceptor_->is_open()) {
            accept_loop();
        }
    });
}

void StatusServer::serve(std::shared_ptr<asio::ip::tcp::socket> sock) {
    auto buf = std::make_shared<std::array<char, 1024>>();
    sock->async_read_some(asio::buffer(*buf), [this, sock, buf](const asio::error_code& ec,
                                                               std::size_t n) {
        if (ec) {
            return;
        }
        const std::string request(buf->data(), n);
        const bool is_status = request.rfind("GET /status", 0) == 0;
        const std::string resp = is_status ? http_response(provider_()) : std::string(kNotFound);
        asio::async_write(*sock, asio::buffer(resp), [sock](const asio::error_code&, std::size_t) {
            asio::error_code ig;
            sock->shutdown(asio::ip::tcp::socket::shutdown_both, ig);
            sock->close(ig);
        });
    });
}

}  // namespace tinykv::server
