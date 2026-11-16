// 文件用途：极简 HTTP `/status` 端点——一个端口、一个路径、返回 JSON（UX-04，SPEC-2 D2-6）。
// 归属：TinyKV-CPP（M2 RaftKV，UX-04）。
//
// 为什么手写而不引 HTTP 框架：D2-6 明确"不引入独立 HTTP 框架"。这里只需要
// "收一个 GET /status，回一段 JSON"——120 行足够，引框架反而多一层依赖与构建风险。
// **已知偏差**：D2-6 原意是复用 RPC 端口（同端口按首字节区分协议）。当前 RPC 端口是
// Raft 帧协议（4 字节长度前缀），与其在同一 socket 上嗅探 HTTP 会让两条协议互相污染，
// 故先独立端口。偏差记入 implementation-notes，M3 引入统一 RPC 时再合并。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <asio.hpp>

#include "common/error.h"

namespace tinykv::server {

class StatusServer {
public:
    // provider 每次请求被调用一次，返回 JSON 文本（由调用方决定暴露哪些字段）
    // kv_handler 可选：处理 `GET /kv?key=&mode=`，为空时该路径返回 404。
    // 读语义由 handler 自行决定（read_index / lease / log），端点只负责传输。
    using KvHandler = std::function<std::string(const std::string& query)>;
    // 写处理器：请求体为制表符分隔的简单文本（put\tkey\tvalue / del\tkey）。
    // 刻意不引 JSON 库：CLI 只需表达两三种写操作，文本协议足够且两端都能一眼看懂。
    using KvWriteHandler = std::function<std::string(const std::string& body)>;
    [[nodiscard]] static result<std::unique_ptr<StatusServer>> start(
        const std::string& host, std::uint16_t port, std::function<std::string()> provider,
        KvHandler kv_handler = {}, KvWriteHandler kv_write = {});

    [[nodiscard]] std::uint16_t port() const { return bound_port_; }

    StatusServer(const StatusServer&) = delete;
    StatusServer& operator=(const StatusServer&) = delete;
    StatusServer(StatusServer&&) = delete;
    StatusServer& operator=(StatusServer&&) = delete;
    ~StatusServer();

private:
    StatusServer() = default;
    void accept_loop();
    void serve(std::shared_ptr<asio::ip::tcp::socket> sock);
    void serve_write(std::shared_ptr<asio::ip::tcp::socket> sock, const std::string& req);

    asio::io_context ctx_{1};
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::thread thread_;
    std::uint16_t bound_port_{0};
    std::function<std::string()> provider_;
    KvHandler kv_handler_;
    KvWriteHandler kv_write_;
};

}  // namespace tinykv::server
