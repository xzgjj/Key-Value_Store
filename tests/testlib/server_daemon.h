// 文件用途：ServerApp 级测试装置——daemon 生命周期 + CLI 通道 + 会话号解析（M7 起多测试共用）。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 为什么共享：txn_cross_region_test 与 lock_shards_wiring_test 各自复制了一份几乎相同的
// Daemon 装置（组合根构造 + --endpoint CLI 通道），CI 的 dup_code 检查将其判为复制粘贴——
// 提取到 testlib 是消除重复的正规路径，而不是放宽阈值。
//
// 并发约定：装置本身只提供串行通道（每次 run_cli 一次连接）；并发压测类用例
// （region_concurrency_bench）自行开线程，不经本装置。

#pragma once

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// server_app.h 必须先于 raft_test_base.h：后者经 asio 引入 Windows DELETE 宏，
// 展开状态下会破坏 kvpb 枚举（server_app.h 文件头注释有完整论证）。
#include "server/server_app.h"
#include "raft_test_base.h"

namespace tinykv::testlib {

using tinykv::server::ServerApp;      // 装置字段用（本命名空间内免限定）
using tinykv::server::ServerOptions;

struct ServerDaemon {
    TempDir tmp;
    ServerApp app;

    // lock_shards：⑨a 分片数（16=默认分片档；1=一把大锁回退档）；cross_region：⑦ 开关
    ServerDaemon(std::size_t lock_shards, bool cross_region, const std::string& tag)
        : tmp(tag), app(make_options(tmp.path() / "db", lock_shards, cross_region)) {
        EXPECT_TRUE(app.start().has_value());
    }
    ~ServerDaemon() { (void)app.stop(); }

    static ServerOptions make_options(const std::filesystem::path& dir, std::size_t lock_shards,
                                      bool cross_region) {
        ServerOptions opt;
        opt.data_dir = dir;
        opt.status_port = 0;
        opt.worker_interval = std::chrono::milliseconds(2);
        opt.lock_shards = lock_shards;
        opt.cross_region_enabled = cross_region;
        return opt;
    }

    // CLI 通道：自动带 --endpoint，stdout/stderr 分开收集（stderr 存 last_err 供诊断）
    [[nodiscard]] int cli(const std::vector<std::string>& args, std::string* out = nullptr) {
        std::vector<std::string> full{"--endpoint",
                                      "127.0.0.1:" + std::to_string(app.status_port())};
        full.insert(full.end(), args.begin(), args.end());
        std::ostringstream o;
        std::ostringstream e;
        const int rc = tinykv::cli::run_cli(full, o, e);
        last_err = e.str();
        if (out != nullptr) {
            *out = o.str();
        }
        return rc;
    }
    std::string last_err;
};

// 从 "OK session=tN" 里取 session id（截断到首个空白：CLI 输出带换行）
inline std::string session_of(const std::string& text) {
    const auto at = text.find("session=");
    if (at == std::string::npos) {
        return {};
    }
    std::string sid;
    for (std::size_t i = at + 8; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n' || ch == '\r' || ch == ' ') {
            break;
        }
        sid.push_back(ch);
    }
    return sid;
}

}  // namespace tinykv::testlib
