// 文件用途：测试沙箱——临时目录 RAII（构造创建、析构清理），避免测试污染真实数据目录。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace tinykv::testlib {

// 每个实例一个独立目录（路径带单调时钟后缀，避免并发测试互相覆盖）
class TempDir {
public:
    explicit TempDir(const std::string& tag = "tinykv") {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / (tag + "_" + std::to_string(tick));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);  // 清理失败不影响测试判定
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace tinykv::testlib
