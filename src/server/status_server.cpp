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

[[nodiscard]] std::optional<RequestLine> parse_request_line(const std::string& request) {
    const auto sp1 = request.find(' ');
    if (sp1 == std::string::npos) {
        return std::nullopt;
    }
    const auto sp2 = request.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        return std::nullopt;
    }
    RequestLine out;
    out.method = request.substr(0, sp1);
    const std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto qmark = target.find('?');
    if (qmark == std::string::npos) {
        out.path = target;
    } else {
        out.path = target.substr(0, qmark);
        out.query = target.substr(qmark + 1);
    }
    return out;
}

// admin GET 端点表（docs/cli.md §10）：命中即交给 admin_handler_，空串=404
[[nodiscard]] bool is_admin_get_path(const std::string& path) {
    return path == "/regions" || path == "/region" || path == "/region/peers" ||
           path == "/scheduler/status";
}

}  // namespace

result<std::unique_ptr<StatusServer>> StatusServer::start(
    const std::string& host, std::uint16_t port, std::function<std::string()> provider,
    KvHandler kv_handler, KvWriteHandler kv_write, AdminHandler admin_handler) {
    auto server = std::unique_ptr<StatusServer>(new StatusServer());   // NOLINT：构造私有
    server->provider_ = std::move(provider);
    server->kv_handler_ = std::move(kv_handler);
    server->kv_write_ = std::move(kv_write);
    server->admin_handler_ = std::move(admin_handler);

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

void StatusServer::serve_write(std::shared_ptr<asio::ip::tcp::socket> sock,
                               const std::string& req) {
    // 只支持一次读到的完整请求（帧很小，CLI 一次 write 就发完）
    const auto sep = req.find("\r\n\r\n");
    const std::string body = sep == std::string::npos ? std::string{} : req.substr(sep + 4);
    respond(sock, http_response(kv_write_(body)));
}

void StatusServer::serve_admin_write(std::shared_ptr<asio::ip::tcp::socket> sock,
                                     const std::string& req, const std::string& path) {
    const auto sep = req.find("\r\n\r\n");
    const std::string body = sep == std::string::npos ? std::string{} : req.substr(sep + 4);
    respond(sock, http_response(admin_handler_("POST", path, "", body)));
}

void StatusServer::respond(std::shared_ptr<asio::ip::tcp::socket> sock, const std::string& text) {
    asio::async_write(*sock, asio::buffer(text), [sock](const asio::error_code&, std::size_t) {
        asio::error_code ig;
        sock->shutdown(asio::ip::tcp::socket::shutdown_both, ig);
        sock->close(ig);
    });
}

std::optional<std::string> StatusServer::handle_get(const RequestLine& line) const {
    if (line.path == "/status") {
        return provider_();
    }
    if (kv_handler_ && line.path == "/kv" && !line.query.empty()) {
        return kv_handler_(line.query);
    }
    if (admin_handler_ && is_admin_get_path(line.path)) {
        auto body = admin_handler_("GET", line.path, line.query, "");
        if (!body.empty()) {
            return body;   // 空串 = region 不存在 / 无数据（404）
        }
    }
    return std::nullopt;
}

void StatusServer::serve(std::shared_ptr<asio::ip::tcp::socket> sock) {
    auto buf = std::make_shared<std::array<char, 1024>>();
    sock->async_read_some(asio::buffer(*buf), [this, sock, buf](const asio::error_code& ec,
                                                               std::size_t n) {
        if (ec) {
            return;
        }
        const std::string request(buf->data(), n);
        const auto line = parse_request_line(request);
        if (!line.has_value()) {
            return;   // 连 malformed 请求都不必回——一次性进程语义，直接断开
        }
        if (line->method == "POST") {
            if (kv_write_ && line->path == "/kv") {
                serve_write(sock, request);
                return;
            }
            if (admin_handler_ && (line->path == "/region/split" || line->path == "/trace")) {
                serve_admin_write(sock, request, line->path);
                return;
            }
        }
        if (line->method == "GET") {
            auto body = handle_get(*line);
            respond(sock, body.has_value() ? http_response(*body) : std::string(kNotFound));
            return;
        }
        respond(sock, std::string(kNotFound));
    });
}

}  // namespace tinykv::server
