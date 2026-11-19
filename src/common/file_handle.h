// 文件用途：std::FILE* 的 RAII 句柄（clang 静态分析 owning-memory 的收口点）。
// 归属：TinyKV-CPP（M9 还债：异常安全）。
//
// 为什么需要它：sst/meta_store 都有一段"fopen → 读写 → fclose"的手写流程。
// 正常路径都记得 fclose，但 std::string 追加可能抛 bad_alloc——异常穿出时 fclose 被跳过，
// 句柄泄漏。把 FILE* 交给 unique_ptr 后，"作用域退出必关闭"由类型系统保证，成本为零。

#pragma once

#include <cstdio>
#include <memory>

namespace tinykv {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) {
            (void)std::fclose(f);
        }
    }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace tinykv
