// 文件用途：status_server.h 的实现——accept → 读请求头 → 回 JSON。
// 归属：TinyKV-CPP（M2 RaftKV，UX-04）。

#include "server/status_server.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace tinykv::server {
namespace {

constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr std::size_t kMaxBodyBytes = 1U << 20U;

// keep_alive=true 时回 Connection: keep-alive（HTTP/1.1 默认持久连接，写明便于抓包辨认）
std::string http_response(const std::string& body, bool keep_alive = false) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) +
           (keep_alive ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n") +
           body;
}

std::string not_found(bool keep_alive) {
    return keep_alive ? "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: keep-alive\r\n\r\n"
                      : "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
}

[[nodiscard]] char lower_ascii(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] bool starts_with_ci(std::string_view line, std::string_view key) {
    if (line.size() < key.size()) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (lower_ascii(line[i]) != key[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t parse_size(std::string_view value) {
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    std::size_t out = 0;
    const auto res = std::from_chars(value.data(), value.data() + value.size(), out, 10);
    return res.ec == std::errc{} && res.ptr == value.data() + value.size() ? out : 0;
}

// 头部里大小写不敏感地找 Content-Length；无异常解析，畸形值一律按 0 处理
[[nodiscard]] std::size_t content_length_of(std::string_view header) {
    constexpr std::string_view key = "content-length:";
    std::size_t pos = 0;
    while ((pos = header.find('\n', pos)) != std::string_view::npos) {
        ++pos;
        const auto eol = header.find('\r', pos);
        if (eol == std::string_view::npos || eol <= pos) {
            continue;
        }
        auto line = header.substr(pos, eol - pos);
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        if (starts_with_ci(line, key)) {
            return parse_size(line.substr(key.size()));
        }
    }
    return 0;
}

// 客户端是否要求关闭（HTTP/1.0 默认关；显式 Connection: close 也关）
[[nodiscard]] bool wants_close(const std::string& request) {
    const auto header_end = request.find("\r\n\r\n");
    const std::string_view header = header_end == std::string::npos
                                        ? std::string_view{request}
                                        : std::string_view{request}.substr(0, header_end);
    if (header.rfind("GET /", 0) != 0 && header.rfind("POST /", 0) != 0) {
        return true;  // 畸形请求行：不接受持久连接
    }
    const auto sp = header.find(' ');
    if (sp == std::string_view::npos) {
        return true;
    }
    const auto sp2 = header.find(' ', sp + 1);
    if (sp2 == std::string_view::npos || !starts_with_ci(header.substr(sp2 + 1), "http/1.1")) {
        return true;  // HTTP/1.0 或未知版本：不退化为持久连接
    }
    // 逐行扫头部（按 CRLF 切；最后一行没有自己的 CRLF，用 header.size() 兜底）
    std::size_t pos = header.find("\r\n");
    while (pos != std::string_view::npos) {
        const auto next = header.find("\r\n", pos + 2);
        auto line = header.substr(
            pos + 2, (next == std::string_view::npos ? header.size() : next) - (pos + 2));
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        if (starts_with_ci(line, "connection:") && starts_with_ci(line.substr(11), " close")) {
            return true;
        }
        pos = next;
    }
    return false;
}

// 常量时间字符串比较：Token 比对不能用 ==（长度/前缀差异会经计时泄漏）
[[nodiscard]] bool equals_constant_time(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// 取 `Authorization: Bearer <token>` 的 token 部分；不存在返回空
[[nodiscard]] std::string_view bearer_token(std::string_view header) {
    std::size_t pos = header.find("\r\n");
    while (pos != std::string_view::npos) {
        const auto next = header.find("\r\n", pos + 2);
        auto line = header.substr(
            pos + 2, (next == std::string_view::npos ? header.size() : next) - (pos + 2));
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        constexpr std::string_view kKey = "authorization:";
        if (starts_with_ci(line, kKey)) {
            auto value = line.substr(kKey.size());
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            constexpr std::string_view kScheme = "bearer ";
            if (starts_with_ci(value, kScheme)) {
                return value.substr(kScheme.size());
            }
            return {};  // 非 Bearer：视为未提供
        }
        pos = next;
    }
    return {};
}

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

}  // namespace

result<std::unique_ptr<StatusServer>> StatusServer::start(
    const std::string& host, std::uint16_t port, std::function<std::string()> provider,
    KvHandler kv_handler, KvWriteHandler kv_write, AdminHandler admin_handler,
    std::string auth_token, std::size_t io_threads, TlsOptions tls) {
    auto server = std::unique_ptr<StatusServer>(new StatusServer());  // NOLINT：构造私有
    server->provider_ = std::move(provider);
    server->kv_handler_ = std::move(kv_handler);
    server->kv_write_ = std::move(kv_write);
    server->admin_handler_ = std::move(admin_handler);
    server->auth_token_ = std::move(auth_token);
    server->tls_opt_ = std::move(tls);
    // ⑭：证书/私钥在启动时装载——文件缺失/格式错 = 启动失败（错误优于静默明文）
    if (server->tls_opt_.enabled()) {
        server->tls_ctx_ = std::make_unique<asio::ssl::context>(
            asio::ssl::context::tls_server);
        asio::error_code tls_ec;
        server->tls_ctx_->use_certificate_chain_file(server->tls_opt_.cert_path, tls_ec);
        if (tls_ec) {
            return tinykv::unexpected(InvalidArgument{
                "证书/私钥加载失败: " + server->tls_opt_.cert_path + ": " + tls_ec.message()});
        }
        server->tls_ctx_->use_private_key_file(server->tls_opt_.key_path,
                                               asio::ssl::context::pem, tls_ec);
        if (tls_ec) {
            return tinykv::unexpected(InvalidArgument{
                "证书/私钥加载失败: " + server->tls_opt_.key_path + ": " + tls_ec.message()});
        }
    }

    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host, ec), port);
    if (ec) {
        return tinykv::unexpected(IoFailed{.path = host, .reason = "状态端点地址非法"});
    }
    server->acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(server->ctx_);
    server->acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        return tinykv::unexpected(IoFailed{.path = host, .reason = "open 失败: " + ec.message()});
    }
    server->acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    server->acceptor_->bind(endpoint, ec);
    if (ec) {
        return tinykv::unexpected(
            IoFailed{.path = host, .reason = "bind 失败（端口占用？）: " + ec.message()});
    }
    server->acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return tinykv::unexpected(IoFailed{.path = host, .reason = "listen 失败: " + ec.message()});
    }
    server->bound_port_ = server->acceptor_->local_endpoint(ec).port();

    server->guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        server->ctx_.get_executor());
    server->accept_loop();
    // ⑪：io_context 线程池——0/1 都归一为单线程（M7 行为）；async_accept 单点投递，
    // 多线程只是并发消化 handler，accept 顺序性不受影响。
    const auto thread_count =
        io_threads == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                        : io_threads;
    for (std::size_t i = 0; i < thread_count; ++i) {
        server->threads_.emplace_back([raw = server.get()] { raw->ctx_.run(); });
    }
    return server;
}

StatusServer::~StatusServer() {
    if (acceptor_ != nullptr) {
        asio::error_code ec;
        acceptor_->close(ec);
    }
    guard_.reset();
    ctx_.stop();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();  // 绝不 detach
        }
    }
}

void StatusServer::accept_loop() {
    // ⑭：TLS 模式由 accept_tls 独立承担 accept（握手成功才进帧解析）；明文 = M7 路径逐字节
    if (tls_ctx_ != nullptr) {
        accept_tls();
        return;
    }
    auto sock = std::make_shared<asio::ip::tcp::socket>(ctx_);
    acceptor_->async_accept(*sock, [this, sock](const asio::error_code& ec) {
        if (!ec) {
            serve_loop(sock, std::make_shared<FrameReader>());
        }
        if (acceptor_ != nullptr && acceptor_->is_open()) {
            accept_loop();
        }
    });
}

// ⑭ TLS accept：握手成功才进 serve_loop；失败断连（对端问题，不记业务错误计数）。
// 握手在 accept 回调链内继续，之后 accept_loop 重启（单点 accept，顺序性不受影响）。
void StatusServer::accept_tls() {
    auto stream =
        std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(ctx_, *tls_ctx_);
    acceptor_->async_accept(stream->next_layer(), [this, stream](const asio::error_code& ec) {
        if (!ec) {
            stream->async_handshake(
                asio::ssl::stream_base::server, [this, stream](const asio::error_code& hec) {
                    if (!hec) {
                        serve_loop(stream, std::make_shared<FrameReader>());
                    }
                    // 握手失败 = Corrupted{where="tls-handshake"} 语义：直接断连，不回应
                });
        }
        if (acceptor_ != nullptr && acceptor_->is_open()) {
            accept_loop();
        }
    });
}

std::optional<std::string> StatusServer::handle_get(const RequestLine& line) const {
    if (line.path == "/status") {
        return provider_();
    }
    if (kv_handler_ && line.path == "/kv" && !line.query.empty()) {
        return kv_handler_(line.query);
    }
    if (admin_handler_) {
        // M6：admin 兜底任意非保留路径（/status 与 /kv 之外）——txn/观测端点由组合根裁决，
        // 传输层不再维护端点白名单（空串 = 404）
        auto body = admin_handler_("GET", line.path, line.query, "");
        if (!body.empty()) {
            return body;
        }
    }
    return std::nullopt;
}

StatusServer::FrameReader::State StatusServer::FrameReader::consume(std::string_view chunk) {
    raw.append(chunk);
    if (header_end == 0) {
        const auto at = raw.find("\r\n\r\n");
        if (at == std::string::npos) {
            return raw.size() > kMaxHeaderBytes ? State::Rejected : State::More;
        }
        header_end = at + 4;
        body_need = content_length_of(std::string_view(raw).substr(0, at));
        if (body_need > kMaxBodyBytes) {
            return State::Rejected;
        }
    }
    return raw.size() - header_end >= body_need ? State::Done : State::More;
}

std::string StatusServer::FrameReader::take_frame() {
    if (header_end == 0 || raw.size() < header_end + body_need) {
        return {};
    }
    const std::size_t total = header_end + body_need;
    std::string frame = raw.substr(0, total);
    raw.erase(0, total);
    reset_for_next();
    return frame;
}

std::string StatusServer::dispatch(const std::string& request) {
    const bool keep_alive = !wants_close(request);
    // ⑩ 鉴权（M7-SEC-01）：在业务分派之前拦下。响应与日志都不得回显 token。
    if (!auth_token_.empty()) {
        const auto header_end = request.find("\r\n\r\n");
        const std::string_view header = header_end == std::string::npos
                                            ? std::string_view{request}
                                            : std::string_view{request}.substr(0, header_end);
        if (!equals_constant_time(bearer_token(header), auth_token_)) {
            return "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";  // 401 不建立持久连接，也不回显任何凭据
        }
    }
    // BUG-0002: 业务异常到此为止；一个畸形请求只回错误，不允许打崩常驻进程
    try {
        const auto line = parse_request_line(request);
        if (!line.has_value()) {
            return not_found(false);  // 畸形请求：不建立持久连接
        }
        const auto body = [&] {
            const auto at = request.find("\r\n\r\n");
            return at == std::string::npos ? std::string{} : request.substr(at + 4);
        }();
        if (line->method == "POST") {
            if (kv_write_ && line->path == "/kv") {
                return http_response(kv_write_(body), keep_alive);
            }
            if (admin_handler_) {
                return http_response(admin_handler_("POST", line->path, "", body), keep_alive);
            }
            return not_found(keep_alive);
        }
        if (line->method == "GET") {
            auto out = handle_get(*line);
            return out.has_value() ? http_response(*out, keep_alive) : not_found(keep_alive);
        }
        return not_found(keep_alive);
    } catch (const std::exception&) {
        return http_response("ERR bad-request", keep_alive);
    } catch (...) {
        return http_response("ERR bad-request", keep_alive);
    }
}

// ---- 连接级处理（⑭ 对传输类型模板化：明文 socket 与 ssl::stream 跑同一套帧解析）----

// 回一段固定文本；keep_alive=true 时不断开，写完后用同一个 reader 继续处理残余帧。
// 非 keep-alive 写完即 shutdown/close（明文）；TLS 流由 ssl 层 shutdown 处理（close 只关底层）。
template <typename Stream>
void StatusServer::respond(std::shared_ptr<Stream> sock, const std::string& text,
                           bool keep_alive, std::shared_ptr<FrameReader> reader) {
    if (!keep_alive) {
        asio::async_write(*sock, asio::buffer(text), [sock](const asio::error_code&, std::size_t) {
            asio::error_code ig;
            auto& raw = sock->lowest_layer();
            raw.shutdown(asio::ip::tcp::socket::shutdown_both, ig);
            raw.close(ig);
        });
        return;
    }
    // keep-alive：写完继续用同一个 reader——残余字节（客户端连发）不能丢。
    // 写失败说明对端已断，直接关连接（对端先走是常态，不记错误）。
    asio::async_write(*sock, asio::buffer(text),
                      [this, sock, reader](const asio::error_code& ec, std::size_t) {
                          if (ec) {
                              asio::error_code ig;
                              sock->lowest_layer().close(ig);
                              return;
                          }
                          serve_loop(sock, reader);
                      });
}

// 连接级循环：一次只推进一帧。缓冲区里已有完整帧（keep-alive 下客户端可能连发）就直接处理，
// 否则去读。只有 EOF / 写失败 / Rejected 才退出——一条 TCP 上可跑任意多个请求。
template <typename Stream>
void StatusServer::serve_loop(std::shared_ptr<Stream> sock, std::shared_ptr<FrameReader> reader) {
    if (reader->pending_bytes()) {
        const auto state = reader->consume("");
        if (state == FrameReader::State::Rejected) {
            respond(sock, http_response("ERR bad-request"), false, reader);
            return;
        }
        if (state == FrameReader::State::Done) {
            const std::string frame = reader->take_frame();
            if (!frame.empty()) {
                const bool keep_alive = !wants_close(frame);
                respond(sock, dispatch(frame), keep_alive, reader);
                return;
            }
        }
    }
    read_frame(std::move(sock), std::move(reader));
}

template <typename Stream>
void StatusServer::read_frame(std::shared_ptr<Stream> sock, std::shared_ptr<FrameReader> reader) {
    auto buf = std::make_shared<std::array<char, 4096>>();
    sock->async_read_some(asio::buffer(*buf),
                          [this, sock, reader, buf](const asio::error_code& ec, std::size_t n) {
                              if (ec) {
                                  return;  // 对端关闭/超时：连接结束（keep-alive 的正常收尾路径）
                              }
                              const auto state = reader->consume(std::string_view(buf->data(), n));
                              if (state == FrameReader::State::More) {
                                  read_frame(sock, reader);  // 帧未齐：继续读，不再丢尾部
                                  return;
                              }
                              if (state == FrameReader::State::Rejected) {
                                  respond(sock, http_response("ERR bad-request"), false, reader);
                                  return;
                              }
                              const std::string frame = reader->take_frame();
                              if (frame.empty()) {
                                  return;  // 理论不可达（Done 必有完整帧）；防御性返回
                              }
                              const bool keep_alive = !wants_close(frame);
                              respond(sock, dispatch(frame), keep_alive, reader);
                          });
}

// 模板只在 .cpp 内实例化（明文 socket 与 ssl::stream 两个 TU 内点），避免头文件膨胀
template void StatusServer::respond<asio::ip::tcp::socket>(
    std::shared_ptr<asio::ip::tcp::socket>, const std::string&, bool,
    std::shared_ptr<FrameReader>);
template void StatusServer::serve_loop<asio::ip::tcp::socket>(
    std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<FrameReader>);
template void StatusServer::read_frame<asio::ip::tcp::socket>(
    std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<FrameReader>);
template void StatusServer::respond<asio::ssl::stream<asio::ip::tcp::socket>>(
    std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>>, const std::string&, bool,
    std::shared_ptr<FrameReader>);
template void StatusServer::serve_loop<asio::ssl::stream<asio::ip::tcp::socket>>(
    std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>>, std::shared_ptr<FrameReader>);
template void StatusServer::read_frame<asio::ssl::stream<asio::ip::tcp::socket>>(
    std::shared_ptr<asio::ssl::stream<asio::ip::tcp::socket>>, std::shared_ptr<FrameReader>);

}  // namespace tinykv::server
