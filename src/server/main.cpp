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
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "config/config.h"
#include "server/server_app.h"

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_mu;
std::condition_variable g_cv;

void on_signal(int) {
    g_stop.store(true);
    g_cv.notify_all();  // 唤醒主循环（cv 是进程内安全的通知路径）
}

}  // namespace

namespace {

void print_usage() {
    std::cerr << "用法: tinykv-server <data_dir> [status_port] [--config <file>]\n"
                 "  <data_dir>       数据目录（内含 manifest / wal/ / sst/）\n"
                 "  [status_port]    业务 HTTP 端口；0 或缺省 = 系统分配\n"
                 "  --config <file>  加载 YAML 配置（engine.fsync_on_commit 等，见 demo.yaml）\n";
}

// 解析结果：配置路径与"是否已就绪"分开表达，避免用返回值兼表两种失败
struct ParsedArgs {
    tinykv::server::ServerOptions opt;
    std::optional<std::string> config_path;
    bool help{false};
    bool ok{true};
};

// 参数解析：前两个位置参数保持 M1 起的 <data_dir> [status_port] 语义（脚本兼容），
// --config 为可选项；配置里的 engine.dir 只在没给位置参数时生效（命令行优先）。
ParsedArgs parse_args(int argc, char** argv) {
    ParsedArgs out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            out.config_path = argv[++i];
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            out.help = true;
            return out;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "未知选项: " << arg << '\n';
            out.ok = false;
            return out;
        }
        if (out.opt.data_dir.empty()) {
            out.opt.data_dir = arg;
        } else if (out.opt.status_port == 0) {
            out.opt.status_port = static_cast<std::uint16_t>(std::stoul(arg));
        } else {
            std::cerr << "多余的参数: " << arg << '\n';
            out.ok = false;
            return out;
        }
    }
    return out;
}

// 配置接线（code-review P0-1）：fsync 不再是"装配点写死 false"，而是读配置、默认 true。
// 之前 main 根本不读 demo.yaml，配置模块只服务 CLI——REL-02 的承诺因此在生产路径上落空。
[[nodiscard]] bool apply_config(const std::optional<std::string>& config_path,
                                tinykv::server::ServerOptions& opt) {
    if (!config_path.has_value()) {
        return true;
    }
    auto cfg = tinykv::load_config(*config_path);
    if (!cfg.has_value()) {
        std::cerr << "配置加载失败: " << tinykv::describe_kind(cfg.error()) << '\n';
        return false;
    }
    if (opt.data_dir.empty()) {
        opt.data_dir = cfg->engine.dir;
    }
    opt.fsync_on_commit = cfg->engine.fsync_on_commit;
    // ⑩ 安全边界：token 为空即不鉴权（此时 ServerApp 会强制绑回环并把它写进 /status），
    // host 只在配置里显式给出时覆盖（命令行/默认 127.0.0.1 优先）
    if (!cfg->server.auth_token.empty()) {
        opt.auth_token = cfg->server.auth_token;
    }
    if (!cfg->server.host.empty() && cfg->server.host != "127.0.0.1") {
        opt.host = cfg->server.host;
    }
    opt.cross_region_enabled = cfg->txn.cross_region_enabled;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // stdout 默认全缓冲：重定向到日志文件时，"就绪"行会在缓冲区里躺到进程退出才出现，
    // 脚本化部署只能轮询端口（M7 验证清单里的第 3 条）。行缓冲 + 每行 flush 让
    // "tinykv-server 就绪: ..." 一写出就落盘，启动脚本可以正儿八经地等这一行。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto parsed = parse_args(argc, argv);
    if (parsed.help) {
        print_usage();
        return 0;
    }
    tinykv::server::ServerOptions opt = std::move(parsed.opt);
    if (!parsed.ok || !apply_config(parsed.config_path, opt) || opt.data_dir.empty()) {
        print_usage();
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const auto host = opt.host;  // opt 随即 move 进 app，日志保留实际监听地址
    tinykv::server::ServerApp app(std::move(opt));
    auto started = app.start();
    if (!started.has_value()) {
        std::cerr << "启动失败: " << tinykv::describe_kind(started.error()) << "\n";
        return 1;
    }
    std::cout << "tinykv-server 就绪: status=" << host << ':' << app.status_port()
              << " observe=" << host << ':' << app.observe_port() << std::endl;  // 立即 flush

    // 主循环：每秒唤醒推进注入时钟，收到信号才退出（ENG-04 禁 sleep_for，用 cv::wait_for）。
    // 时钟推进是 TTL 锁决议与 GC 安全点在真实进程里可自愈的前提（P1 修复）。
    {
        std::unique_lock lock(g_mu);
        while (!g_stop.load()) {
            g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop.load(); });
            if (!g_stop.load()) {
                app.advance_clock(1000);
                app.sweep_sessions();  // 空闲会话回收：GC 安全点不被僵尸会话钉死
            }
        }
    }
    std::cout << "收到停止信号，优雅关闭中…" << std::endl;
    auto stopped = app.stop();
    if (!stopped.has_value()) {
        std::cerr << "关闭异常: " << tinykv::describe_kind(stopped.error()) << "\n";
        return 1;
    }
    std::cout << "已退出。" << std::endl;
    return 0;
}
