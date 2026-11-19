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

// 百分号解码（%XX / '+'）。客户端必须 encode，否则含空格/&/=/非 ASCII 的 key
// 会被 query 语法截断——旧实现客户端与服务端都不编码，key 里的 '&' 会凭空消失。
[[nodiscard]] inline std::string url_decode(std::string_view s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

[[nodiscard]] inline std::string http_query_param(const std::string& query,
                                                   const std::string& name) {
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const auto at = query.find('&', begin);
        const auto item = query.substr(begin, at == std::string::npos ? at : at - begin);
        const auto eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == name) {
            return url_decode(item.substr(eq + 1));
        }
        if (at == std::string::npos) {
            break;
        }
        begin = at + 1;
    }
    return {};
}

// JSON 字符串转义：除 " 与 \ 之外，控制字符（0x00-0x1F）也必须转义——
// 漏掉它们时，键值含换行/制表符就会产出非法 JSON，客户端解析直接失败（code-review P1-12）。
// 用 if 链而非 switch：switch 各分支体在规范化后高度相似，会被重复代码门禁误判。
[[nodiscard]] inline std::string escape(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (byte < 0x20) {
            out += "\\u00";   // 其余控制字符按 \u00XX（RFC 8259 允许，且字节级可逆）
            out.push_back(kHex[(byte >> 4) & 0x0F]);
            out.push_back(kHex[byte & 0x0F]);
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] inline std::string region_json(const raftstore::Region& r) {
    return "{\"id\":" + std::to_string(r.id.value) +
           ",\"start\":\"" + escape(r.start_key) + "\",\"end\":\"" + escape(r.end_key) +
           "\",\"version\":" + std::to_string(r.epoch.version) +
           ",\"conf_ver\":" + std::to_string(r.epoch.conf_ver) +
           ",\"size_hint\":" + std::to_string(r.size_hint) + "}";
}

}  // namespace tinykv::server::http
