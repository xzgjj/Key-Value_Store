// 文件用途：测试用有界 socket 等待——select 分片轮询，ENG-04 无 sleep_for 合规 + 防挂死
// （BUG-0024 配套测试面：服务端沉默时测试必须以失败收场而不是永久阻塞）。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。

#pragma once

#include <asio.hpp>

#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace tinykv::testlib {

// deadline 内轮询 socket 读就绪（5ms select 分片）；到期返回 false（调用方以失败收场）。
// 调用方需先把 socket 置 non_blocking，read_some 的 would_block 由轮询循环消化。
template <typename Stream>
[[nodiscard]] bool wait_readable(Stream& sock, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto raw = sock.native_handle();
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(raw, &reads);
        timeval slice{0, 5000};  // 5ms 分片：尽快返回，不引入长阻塞
        const int rc =
            ::select(static_cast<int>(raw) + 1, &reads, nullptr, nullptr, &slice);
        if (rc > 0) {
            return true;
        }
        if (rc < 0) {
            return false;  // select 本身出错：让调用方的 read_some 以错误收场
        }
    }
    return false;
}

}  // namespace tinykv::testlib
