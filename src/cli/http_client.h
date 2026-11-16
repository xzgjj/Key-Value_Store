// 文件用途：极简 HTTP 客户端——只做 GET，供 CLI 集群模式取 /status（UX-04）。
// 归属：TinyKV-CPP（M2 RaftKV，UX-04）。
//
// 同步实现：CLI 是一次性进程，阻塞取一次响应即可，不需要事件循环。
// 超时靠 socket 的 native 选项设置，避免对端不响应时把命令行挂住。

#pragma once

#include <cstdint>
#include <string>

#include <asio.hpp>

#include "common/error.h"

namespace tinykv::cli {

// 返回响应正文（去掉头部）；连接失败/超时返回 IoFailed
[[nodiscard]] inline result<std::string> http_get(const std::string& host, std::uint16_t port,
                                                  const std::string& path) {
    asio::io_context ctx;
    asio::error_code ec;
    const auto addr = asio::ip::make_address(host, ec);
    if (ec) {
        return std::unexpected(InvalidArgument{"状态端点地址非法: " + host});
    }
    asio::ip::tcp::socket sock(ctx);
    sock.connect(asio::ip::tcp::endpoint(addr, port), ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = host, .reason = "连接失败: " + ec.message()});
    }

    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nConnection: close\r\n\r\n";
    asio::write(sock, asio::buffer(req), ec);
    if (ec) {
        return std::unexpected(IoFailed{.path = host, .reason = "发送失败: " + ec.message()});
    }

    std::string raw;
    asio::streambuf buf;
    while (true) {
        const std::size_t n = asio::read(sock, buf, ec);
        raw.append(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) +
                                                       static_cast<std::ptrdiff_t>(n));
        buf.consume(n);
        if (ec) {
            break;   // EOF 或错误：下面的解析会决定成败
        }
    }
    const auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        return std::unexpected(Corrupted{.where = host, .reason = "响应缺少头部结束标记"});
    }
    return raw.substr(sep + 4);
}

}  // namespace tinykv::cli
