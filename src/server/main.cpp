// 文件用途：tinykv-server——组合根入口（M6 第③层）：唯一负责"创建并连接全部组件"的进程。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 组装顺序（ServerApp 内部，Lifecycle 语义）：
//   config → Storage(Engine) → Raft(Peer) → Worker(泵循环) → StatusServer(HTTP)
// 信号触发优雅关闭：SIGINT/SIGTERM → 原子标志 → ServerApp::stop()（逆序下线：HTTP 先拒绝
//   新请求，再收 Worker/Peer/Engine）。
// 确定性时钟：now 由主循环推进（每秒 +1000ms），TTL/GC 判定可复现；接真实时钟时换输入源。

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "server/server_app.h"

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_mu;
std::condition_variable g_cv;

void on_signal(int) {
    g_stop.store(true);
    g_cv.notify_all();   // 唤醒主循环（cv 是进程内安全的通知路径）
}

}  // namespace

int main(int argc, char** argv) {
    tinykv::server::ServerOptions opt;
    // 参数极简：tinykv-server <data_dir> [status_port]（更多配置随 daemon 化演进进 config）
    if (argc >= 2) {
        opt.data_dir = argv[1];
    }
    if (argc >= 3) {
        opt.status_port = static_cast<std::uint16_t>(std::stoul(argv[2]));
    }
    if (opt.data_dir.empty()) {
        std::cerr << "用法: tinykv-server <data_dir> [status_port]\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    tinykv::server::ServerApp app(std::move(opt));
    auto started = app.start();
    if (!started.has_value()) {
        std::cerr << "启动失败: " << tinykv::describe_kind(started.error()) << "\n";
        return 1;
    }
    std::cout << "tinykv-server 就绪: status=127.0.0.1:" << app.status_port() << "\n";

    // 主循环：等待停止信号（cv 等待，非 sleep_for——ENG-04）
    {
        std::unique_lock lock(g_mu);
        g_cv.wait(lock, [] { return g_stop.load(); });
    }
    std::cout << "收到停止信号，优雅关闭中…\n";
    auto stopped = app.stop();
    if (!stopped.has_value()) {
        std::cerr << "关闭异常: " << tinykv::describe_kind(stopped.error()) << "\n";
        return 1;
    }
    std::cout << "已退出。\n";
    return 0;
}
