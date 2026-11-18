// 文件用途：tinykv-server——组合根入口（M6 第③层）：唯一负责"创建并连接全部组件"的进程。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 组装顺序（ServerApp 内部，Lifecycle 语义）：
//   config → Storage(Engine) → Raft(Peer) → Worker(泵循环) → StatusServer(HTTP)
// 信号触发优雅关闭：SIGINT/SIGTERM → 原子标志 → ServerApp::stop()（逆序下线：HTTP 先拒绝
//   新请求，再收 Worker/Peer/Engine）。
// 确定性时钟：主循环每秒推进 1000ms（注入口径，非墙钟），TTL/GC 判定因此可复现。

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

    const auto host = opt.host;   // opt 随即 move 进 app，日志保留实际监听地址
    tinykv::server::ServerApp app(std::move(opt));
    auto started = app.start();
    if (!started.has_value()) {
        std::cerr << "启动失败: " << tinykv::describe_kind(started.error()) << "\n";
        return 1;
    }
    std::cout << "tinykv-server 就绪: status=" << host << ':' << app.status_port() << "\n";

    // 主循环：每秒唤醒推进注入时钟，收到信号才退出（ENG-04 禁 sleep_for，用 cv::wait_for）。
    // 时钟推进是 TTL 锁决议与 GC 安全点在真实进程里可自愈的前提（P1 修复）。
    {
        std::unique_lock lock(g_mu);
        while (!g_stop.load()) {
            g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop.load(); });
            if (!g_stop.load()) {
                app.advance_clock(1000);
                app.sweep_sessions();   // 空闲会话回收：GC 安全点不被僵尸会话钉死
            }
        }
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
