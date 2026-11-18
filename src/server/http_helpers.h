// 文件用途：HTTP 边界的无状态工具——查询解析、帧拆分、JSON 转义、region 序列化。
// 归属：TinyKV-CPP（M6 运行时 / M7 ⑥ 拆 God class）。
//
// 为什么独立：这些函数不持有任何 ServerApp 状态，却占了组合根 ~60 行；抽出后
// 组合根只留"装配 + 路由"，且测试 fixture（cli_region_test 等）可复用同一口径，
// 避免多处 copy 出不一致的解析规则。BUG-0002 的 from_chars 护栏也只此一份。

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "raftstore/region.h"

namespace tinykv::server::http {

// 无异常解析 u64：不抛、不接受尾随字符/负号/空串（BUG-0002）
[[nodiscard]] inline std::optional<std::uint64_t> parse_u64(std::string_view text) {
    std::uint64_t out{0};
    if (text.empty()) {
        return std::nullopt;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto res = std::from_chars(first, last, out, 10);
    if (res.ec != std::errc{} || res.ptr != last) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline std::vector<std::string> split_frame(const std::string& body) {
    std::vector<std::string> parts;
    std::istringstream in(body);
    std::string item;
    while (std::getline(in, item, '\t')) {
        parts.push_back(item);
    }
    return parts;
}

[[nodiscard]] inline std::string http_query_param(const std::string& query,
                                                   const std::string& name) {
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const auto at = query.find('&', begin);
        const auto item = query.substr(begin, at == std::string::npos ? at : at - begin);
        const auto eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == name) {
            return item.substr(eq + 1);
        }
        if (at == std::string::npos) {
            break;
        }
        begin = at + 1;
    }
    return {};
}

[[nodiscard]] inline std::string escape(const std::string& s) {
    std::string out;
    for (const char ch : s) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

[[nodiscard]] inline std::string region_json(const raftstore::Region& r) {
    return "{\"id\":" + std::to_string(r.id.value) +
           ",\"start\":\"" + r.start_key + "\",\"end\":\"" + r.end_key +
           "\",\"version\":" + std::to_string(r.epoch.version) +
           ",\"conf_ver\":" + std::to_string(r.epoch.conf_ver) +
           ",\"size_hint\":" + std::to_string(r.size_hint) + "}";
}

}  // namespace tinykv::server::http
