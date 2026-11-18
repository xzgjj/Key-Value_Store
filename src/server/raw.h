// 文件用途：Raw API——单机进程内直调的服务层薄壳，把用户视角的 key/value 转成存储层批次。
// 归属：TinyKV-CPP（M1 单机存储，SPEC-1 §1.2）。
//
// 为什么还留这一层：CLI 与嵌入方都不该直接拼 Modify 批次，也不该关心 span 转换；
// M2 起同一组方法会改为经 Raft 提案（接口不变，实现换掉），所以边界现在就要分开。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/compat.h"
#include "common/error.h"
#include "storage/storage.h"

namespace tinykv {

class RawServer {
public:
    explicit RawServer(Storage& storage) : storage_(storage) {}

    [[nodiscard]] result<GetResult> get(Cf cf, std::string_view key) const {
        return storage_.get(cf, view(key));
    }

    [[nodiscard]] result<void> put(Cf cf, std::string_view key, std::string_view value) {
        ModifyVec batch{
            Modify{Put{.cf = cf, .key = std::string(key), .value = std::string(value)}}};
        return storage_.write(batch);
    }

    [[nodiscard]] result<void> remove(Cf cf, std::string_view key) {
        ModifyVec batch{Modify{Delete{.cf = cf, .key = std::string(key)}}};
        return storage_.write(batch);
    }

    // 扫描结果一次性取回（M1 量级；流式迭代随 M2 的 range 语义一起改）
    [[nodiscard]] result<std::vector<std::pair<std::string, std::string>>> scan(
        Cf cf, std::string_view start, std::size_t limit) const {
        auto reader = storage_.create_reader();
        if (!reader) {
            return tinykv::unexpected(reader.error());
        }
        auto it = (*reader)->scan(cf, view(start), limit);
        if (!it) {
            return tinykv::unexpected(it.error());
        }
        std::vector<std::pair<std::string, std::string>> rows;
        for (; (**it).valid(); (**it).next()) {
            rows.emplace_back(std::string((**it).key()), std::string((**it).value()));
        }
        return rows;
    }

private:
    [[nodiscard]] static tinykv::span<const std::byte> view(std::string_view s) {
        return tinykv::as_bytes(tinykv::span{s.data(), s.size()});
    }

    Storage& storage_;
};

}  // namespace tinykv
