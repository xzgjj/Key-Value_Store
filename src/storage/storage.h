// 文件用途：存储层对上接口——一次写操作的类型、只读快照 Reader、范围迭代器（SPEC-1 §1.3.1）。
// 归属：TinyKV-CPP（M1 单机存储，FUNC-01）。
//
// 设计约束（来自工作区 API 指南 §10 与本项目红线）：
//   * 不返回 pair/tuple：Put/Delete 是具体类型，Modify 用 variant 承载（清单 #9）；
//   * key 一律 span<const byte>，不传 ptr+len（清单 #4）；
//   * 全部接口 [[nodiscard]] + result<T>，失败必带结构化原因（清单 #5/#6）；
//   * Reader 是 RAII 快照句柄，不提供 open/close 配对（清单 #7/#8）。
//   * Cf 复用 common/encode.h 的定义，存储层不得自建列族枚举（红线：编码唯一实现点）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/encode.h"
#include "common/error.h"

namespace tinykv {

// 一次写操作；cf 与 user_key 都未编码，编码在存储层内部完成（调用方不拼前缀）
struct Put {
    Cf cf;
    std::string key;
    std::string value;
};

struct Delete {
    Cf cf;
    std::string key;
};

struct Modify {
    std::variant<Put, Delete> op;
};

using ModifyVec = std::vector<Modify>;   // 批量是一等公民：一次 write 一次落盘

// Get 结果：用 optional 区分"值不存在"与"值为空串"
struct GetResult {
    std::optional<std::string> value;
};

// 范围迭代器：只在 valid() 为真时允许取 key/value；析构释放底层资源（RAII）
class Iterator {
public:
    Iterator() = default;
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&&) = delete;
    Iterator& operator=(Iterator&&) = delete;
    virtual ~Iterator() = default;

    [[nodiscard]] virtual bool valid() const = 0;
    virtual void next() = 0;
    // 返回已解码 CF 前缀的用户 key；迭代器失效前有效
    [[nodiscard]] virtual std::string_view key() const = 0;
    [[nodiscard]] virtual std::string_view value() const = 0;
};

// 只读快照：同一 Reader 内多次 get/scan 看到同一份数据，不受后续 flush 影响（SPEC-1 红线 3）
class Reader {
public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;
    virtual ~Reader() = default;

    [[nodiscard]] virtual result<GetResult> get(Cf cf,
                                                std::span<const std::byte> key) const = 0;
    [[nodiscard]] virtual result<std::unique_ptr<Iterator>> scan(
        Cf cf, std::span<const std::byte> start, std::size_t limit) const = 0;
};

class Storage {
public:
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;
    virtual ~Storage() = default;

    [[nodiscard]] virtual result<GetResult> get(Cf cf, std::span<const std::byte> key) = 0;
    [[nodiscard]] virtual result<void> write(std::span<const Modify> batch) = 0;
    [[nodiscard]] virtual result<std::unique_ptr<Reader>> create_reader() = 0;
};

}  // namespace tinykv
