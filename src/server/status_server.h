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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "common/error.h"

namespace tinykv::server {

// 请求行（METHOD /path?query HTTP/x）：⑦b 起按 method+path 分派
struct RequestLine {
    std::string method;
    std::string path;
    std::string query;
};

class StatusServer {
public:
    // provider 每次请求被调用一次，返回 JSON 文本（由调用方决定暴露哪些字段）
    // kv_handler 可选：处理 `GET /kv?key=&mode=`，为空时该路径返回 404。
    // 读语义由 handler 自行决定（read_index / lease / log），端点只负责传输。
    using KvHandler = std::function<std::string(const std::string& query)>;
    // 写处理器：请求体为制表符分隔的简单文本（put\tkey\tvalue / del\tkey）。
    // 刻意不引 JSON 库：CLI 只需表达两三种写操作，文本协议足够且两端都能一眼看懂。
    using KvWriteHandler = std::function<std::string(const std::string& body)>;
    // M4 ⑦b：观测/管理端点（docs/cli.md §10）。
    //   GET  /regions / /region / /region/peers / /scheduler/status（query 携带 id）
    //   POST /region/split、/trace（body 为制表符文本，与 /kv 同风格）
    // 返回空串 = 404（region 不存在 / 端点未装配）。
    using AdminHandler =
        std::function<std::string(const std::string& method, const std::string& path,
                                  const std::string& query, const std::string& body)>;
    // auth_token 非空 = 所有请求必须带 `Authorization: Bearer <token>`（⑩ M7-SEC-01）；
    // 为空 = 不鉴权（调用方负责"未鉴权时只绑回环"，见 ServerApp 的强制回环逻辑）
    // io_threads = io_context 并发线程数（M8 ⑪，SPEC-7 §7.2.1）：默认 1 = M7 行为逐字节；
    // >1 时 handler 并发执行——并发安全由组合根的审计清单保证（docs/落实/notes-m8.md §1）。
    [[nodiscard]] static result<std::unique_ptr<StatusServer>> start(
        const std::string& host, std::uint16_t port, std::function<std::string()> provider,
        KvHandler kv_handler = {}, KvWriteHandler kv_write = {}, AdminHandler admin_handler = {},
        std::string auth_token = {}, std::size_t io_threads = 1);

    [[nodiscard]] std::uint16_t port() const { return bound_port_; }

    StatusServer(const StatusServer&) = delete;
    StatusServer& operator=(const StatusServer&) = delete;
    StatusServer(StatusServer&&) = delete;
    StatusServer& operator=(StatusServer&&) = delete;
    ~StatusServer();

private:
    StatusServer() = default;
    // BUG-0002/P0-2：逐块累积到"头结束 + Content-Length 字节体"齐全再分派。
    // keep-alive（M7）：同一条连接上可连续解析多个请求——取走一帧后残余字节留给下一次，
    // 这是"每请求新建 TCP"被消掉的关键（串行与并发的差距主要来自建连）。
    struct FrameReader {
        enum class State { More, Done, Rejected };
        std::string raw;
        std::size_t header_end{0};
        std::size_t body_need{0};
        [[nodiscard]] State consume(std::string_view chunk);
        // 取走已完整的一帧并从缓冲区移除；无完整帧返回空串
        [[nodiscard]] std::string take_frame();
        // 复位解析状态（保留残余字节，供同一连接上的下一个请求）
        void reset_for_next() {
            header_end = 0;
            body_need = 0;
        }
        [[nodiscard]] bool pending_bytes() const { return !raw.empty(); }
    };
    void accept_loop();
    void serve(std::shared_ptr<asio::ip::tcp::socket> sock);
    void read_frame(std::shared_ptr<asio::ip::tcp::socket> sock,
                    std::shared_ptr<FrameReader> reader);
    // BUG-0002: 请求分派的唯一异常屏障——业务处理器抛出的异常到此为止
    [[nodiscard]] std::string dispatch(const std::string& request);
    // ⑦b：GET 分派（status/kv/admin 三路）；nullopt = 404
    [[nodiscard]] std::optional<std::string> handle_get(const RequestLine& line) const;
    // 回一段固定文本；keep_alive=true 时不断开，写完后用同一个 reader 继续处理残余帧
    void respond(std::shared_ptr<asio::ip::tcp::socket> sock, const std::string& text,
                 bool keep_alive, std::shared_ptr<FrameReader> reader);
    // 连接级循环：读帧 → 分派 → 回写 → 若 keep-alive 则继续（EOF/超时/close 才退出）
    void serve_loop(std::shared_ptr<asio::ip::tcp::socket> sock,
                    std::shared_ptr<FrameReader> reader);

    asio::io_context ctx_{1};
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::vector<std::thread> threads_;  // ⑪：io_context 线程池（1 = M7 单线程行为）
    std::uint16_t bound_port_{0};
    std::function<std::string()> provider_;
    KvHandler kv_handler_;
    KvWriteHandler kv_write_;
    AdminHandler admin_handler_;
    std::string auth_token_;  // 空 = 鉴权关闭
};

}  // namespace tinykv::server
