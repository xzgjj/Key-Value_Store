// 文件用途：极简 HTTP 客户端——只做 GET/POST，供 CLI 与测试访问 daemon 端点（UX-04）。
// 归属：TinyKV-CPP（M2 RaftKV，UX-04；M8 后还债批重写为带超时的原生 socket 实现）。
//
// 同步实现：CLI 是一次性进程，阻塞取一次响应即可，不需要事件循环。
// BUG-0024（code-review r2 N-1 客户端面）：旧实现的头注释声称「超时靠 socket 的 native
// 选项设置」，但没有任何超时代码——connect 与 asio::read 无限阻塞，靠 EOF 收尾。
// 服务端只要「收了请求但不应答也不关连接」，CLI/测试就永久挂死。现改为原生 socket：
//   connect：非阻塞 + select（默认 5s）；
//   send/recv：SO_SNDTIMEO / SO_RCVTIMEO（默认 10s， blocking socket 上两平台都生效）。
// 刻意不用 asio 计时器：与 status_server 同因——io_context 内部 timer 线程析构 join
// 在 MinGW/IOCP 上会挂死（BUG-0023 同款根因），客户端短命 io_context 同样踩。

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "common/error.h"

namespace tinykv::cli {

// 超时预算（毫秒）：connect 5s / 单次 send-recv 10s。测试可注入更小值做确定性用例。
constexpr std::uint32_t kConnectTimeoutMs = 5000;
constexpr std::uint32_t kIoTimeoutMs = 10000;

namespace detail {

#ifdef _WIN32
inline void net_init() {
    static const bool inited = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)inited;
}
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int close_socket(socket_t s) { return closesocket(s); }
inline int last_error() { return WSAGetLastError(); }
constexpr int kWouldBlock = WSAEWOULDBLOCK;
inline void set_nonblock(socket_t s, bool on) {
    u_long mode = on ? 1 : 0;
    (void)ioctlsocket(s, FIONBIO, &mode);
}
#else
inline void net_init() {}
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int close_socket(socket_t s) { return ::close(s); }
inline int last_error() { return errno; }
constexpr int kWouldBlock = EWOULDBLOCK;
inline void set_nonblock(socket_t s, bool on) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    (void)::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

// RAII 句柄：作用域退出必关 socket。旧实现每个错误分支手写 close，
// 一旦在 socket() 之后新增可能抛异常的步骤（std::string 构造/追加），句柄就会漏。
struct SocketGuard {
    explicit SocketGuard(socket_t s) : fd(s) {}
    ~SocketGuard() {
        if (fd != kInvalidSocket) {
            (void)close_socket(fd);
        }
    }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    socket_t fd;
};

}  // namespace detail

// 通用请求：GET 时 body 为空且不带 Content-Length；POST 时携带。
// 返回响应正文（去掉头部）；连接失败/超时返回 IoFailed。
//
// I/O 全程 select + deadline（socket 保持非阻塞）：实测本环境（MinGW/UCRT）阻塞 socket
// 的 SO_RCVTIMEO/SO_SNDTIMEO 不生效（setsockopt 成功但 recv 永久阻塞，独立样例复现），
// 故两平台统一走 select 路线——deadline 到点即报 IoFailed，绝不无限等待。
[[nodiscard]] inline result<std::string> request(const std::string& verb, const std::string& host,
                                                 std::uint16_t port, const std::string& path,
                                                 const std::string& body,
                                                 std::uint32_t connect_timeout_ms =
                                                     kConnectTimeoutMs,
                                                 std::uint32_t io_timeout_ms = kIoTimeoutMs) {
    detail::net_init();
    const auto addr = ::inet_addr(host.c_str());
    if (addr == INADDR_NONE || addr == INADDR_ANY) {
        // 与旧实现口径一致：只收数字 IP（CLI 端点契约本就是 host:port 数字对）
        return tinykv::unexpected(InvalidArgument{"状态端点地址非法: " + host});
    }
    detail::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == detail::kInvalidSocket) {
        return tinykv::unexpected(IoFailed{.path = host, .reason = "socket() 失败"});
    }
    detail::SocketGuard socket_guard{fd};  // 所有错误/异常路径统一由它关闭

    // ---- connect：非阻塞 + select 写就绪（默认 5s）----
    detail::set_nonblock(fd, true);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(port);
    endpoint.sin_addr.s_addr = addr;
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint));
    bool connected = rc == 0;
    if (!connected) {
        const int err = detail::last_error();
        if (err == detail::kWouldBlock
#ifndef _WIN32
            || err == EINPROGRESS
#endif
        ) {
            fd_set writes;
            FD_ZERO(&writes);
            FD_SET(fd, &writes);
            timeval wait{static_cast<long>(connect_timeout_ms / 1000),
                         static_cast<long>((connect_timeout_ms % 1000) * 1000)};
            rc = ::select(static_cast<int>(fd) + 1, nullptr, &writes, nullptr, &wait);
            if (rc > 0) {
                int so_err = 0;
                socklen_t len = sizeof(so_err);
                (void)::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char*>(&so_err), &len);
                connected = so_err == 0;
            }
        }
    }
    if (!connected) {
        return tinykv::unexpected(
            IoFailed{.path = host, .reason = "连接失败/超时: " + std::to_string(port)});
    }

    // ---- send：非阻塞 + select 写就绪，deadline 内发完全部请求 ----
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(io_timeout_ms);
    auto remaining_ms = [&deadline] -> long {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  std::chrono::steady_clock::now())
                .count();
        return left < 0 ? 0 : static_cast<long>(left);
    };
    std::string req = verb + " " + path + " HTTP/1.1\r\nHost: " + host;
    if (!body.empty()) {
        req += "\r\nContent-Length: " + std::to_string(body.size());
    }
    req += "\r\nConnection: close\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < req.size()) {
        if (remaining_ms() <= 0) {
            return tinykv::unexpected(
                IoFailed{.path = host, .reason = "发送超时（对端不收也不关连接）"});
        }
        fd_set writes;
        FD_ZERO(&writes);
        FD_SET(fd, &writes);
        timeval wait{remaining_ms() / 1000, (remaining_ms() % 1000) * 1000};
        rc = ::select(static_cast<int>(fd) + 1, nullptr, &writes, nullptr, &wait);
        if (rc <= 0) {
            continue;  // 超时分片/中断：由 remaining_ms 兜底收口
        }
        const int n = static_cast<int>(
            ::send(fd, req.data() + sent, static_cast<int>(req.size() - sent), 0));
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        const int err = detail::last_error();
        if (err != detail::kWouldBlock) {
            return tinykv::unexpected(
                IoFailed{.path = host, .reason = "发送失败 code=" + std::to_string(err)});
        }
    }

    // ---- recv：非阻塞 + select 读就绪，deadline 内收完（EOF = 正常收尾）----
    std::string raw;
    char buf[4096];
    for (;;) {
        if (remaining_ms() <= 0) {
            return tinykv::unexpected(IoFailed{
                .path = host, .reason = "响应超时（对端不应答也不关连接）"});
        }
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        timeval wait{remaining_ms() / 1000, (remaining_ms() % 1000) * 1000};
        rc = ::select(static_cast<int>(fd) + 1, &reads, nullptr, nullptr, &wait);
        if (rc <= 0) {
            continue;  // 超时分片/中断：由 remaining_ms 兜底收口
        }
        const int n = static_cast<int>(::recv(fd, buf, sizeof(buf), 0));
        if (n > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;  // EOF：服务端 Connection: close 的正常收尾
        }
        const int err = detail::last_error();
        if (err != detail::kWouldBlock) {
            return tinykv::unexpected(IoFailed{
                .path = host, .reason = "接收失败 code=" + std::to_string(err)});
        }
    }

    const auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        return tinykv::unexpected(Corrupted{.where = host, .reason = "响应缺少头部结束标记"});
    }
    return raw.substr(sep + 4);
}

// 读：GET，返回响应正文（去掉头部）；连接失败/超时返回 IoFailed
[[nodiscard]] inline result<std::string> http_get(const std::string& host, std::uint16_t port,
                                                  const std::string& path) {
    return request("GET", host, port, path, {});
}

// 写：POST，请求体为纯文本（写操作只需两三种形态，不引 JSON 库）
[[nodiscard]] inline result<std::string> http_post(const std::string& host, std::uint16_t port,
                                                   const std::string& path,
                                                   const std::string& body) {
    return request("POST", host, port, path, body);
}

}  // namespace tinykv::cli
