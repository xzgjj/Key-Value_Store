// 文件用途：存储类测试的共享构造——Put/Delete 批次、字节视图转换的便捷入口。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 存在的理由：多个引擎用例都要"造一个批次"，各写一份会被 CI 的 dup_code 判为复制粘贴
// （实测已被拦下两次）。共享到 testlib 是正确解法，而不是加白名单。

#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "storage/storage.h"

namespace tinykv::testlib {

// 一次 Put：指定列族
[[nodiscard]] inline ModifyVec put_batch(Cf cf, std::string key, std::string value) {
    return ModifyVec{Modify{Put{.cf = cf, .key = std::move(key), .value = std::move(value)}}};
}

// 一次 Put：默认列族（最常用，省掉 CF 参数）
[[nodiscard]] inline ModifyVec put_batch(std::string key, std::string value) {
    return put_batch(Cf::Default, std::move(key), std::move(value));
}

// 一次 Delete：默认列族
[[nodiscard]] inline ModifyVec delete_batch(std::string key) {
    return ModifyVec{Modify{Delete{.cf = Cf::Default, .key = std::move(key)}}};
}

}  // namespace tinykv::testlib
