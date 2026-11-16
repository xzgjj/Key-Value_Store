// 文件用途：config.h 的实现——一行一条 `键: 值`，一级缩进分组，`#` 起注释。
// 归属：TinyKV-CPP（M1 单机存储，UX-05）。

#include "config/config.h"

#include <fstream>
#include <string_view>
#include <vector>

namespace tinykv {
namespace {

constexpr std::string_view kIndent = "  ";

std::string trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

// 去掉引号：'v' / "v" 都接受，裸值原样返回
std::string unquote(const std::string& v) {
    if (v.size() >= 2) {
        const char f = v.front();
        if ((f == '"' || f == '\'') && v.back() == f) {
            return v.substr(1, v.size() - 2);
        }
    }
    return v;
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "true") {
        out = true;
        return true;
    }
    if (v == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parse_size(const std::string& v, std::size_t& out) {
    if (v.empty()) {
        return false;
    }
    std::size_t acc = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') {
            return false;
        }
        acc = acc * 10 + static_cast<std::size_t>(c - '0');
    }
    out = acc;
    return true;
}

// 未知键一律报错：配置写错了必须当场看见，而不是启动后行为诡异
result<void> apply_key(Config& cfg, const std::string& section, const std::string& key,
                       const std::string& value, int line_no) {
    const auto bad = [&](const std::string& why) {
        return result<void>(std::unexpected(InvalidArgument{
            "配置第 " + std::to_string(line_no) + " 行：" + why}));
    };
    if (section == "engine") {
        if (key == "dir") {
            cfg.engine.dir = unquote(value);
            return {};
        }
        if (key == "fsync_on_commit") {
            return parse_bool(value, cfg.engine.fsync_on_commit)
                       ? result<void>{}
                       : bad("fsync_on_commit 只能是 true/false");
        }
        if (key == "memtable_limit_mb") {
            return parse_size(value, cfg.engine.memtable_limit_mb) ? result<void>{}
                                                                  : bad("memtable_limit_mb 必须是整数");
        }
        if (key == "wal_segment_mb") {
            return parse_size(value, cfg.engine.wal_segment_mb) ? result<void>{}
                                                                : bad("wal_segment_mb 必须是整数");
        }
        return bad("engine 段未知键 '" + key + "'");
    }
    if (section == "log") {
        if (key == "level") {
            cfg.log.level = unquote(value);
            return {};
        }
        if (key == "dir") {
            cfg.log.dir = unquote(value);
            return {};
        }
        return bad("log 段未知键 '" + key + "'");
    }
    return bad("未知配置段 '" + section + "'");
}

}  // namespace

result<Config> load_config(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::unexpected(IoFailed{.path = path.string(), .reason = "配置文件打不开"});
    }

    Config cfg;
    std::string section;
    std::string raw;
    int line_no = 0;
    while (std::getline(in, raw)) {
        ++line_no;
        const auto hash = raw.find('#');
        if (hash != std::string::npos) {
            raw.erase(hash);
        }
        if (trim(raw).empty()) {
            continue;
        }
        const bool nested = raw.rfind(kIndent, 0) == 0;
        const auto colon = raw.find(':');
        if (colon == std::string::npos) {
            return std::unexpected(
                InvalidArgument{"配置第 " + std::to_string(line_no) + " 行缺少冒号"});
        }
        const std::string key = trim(raw.substr(0, colon));
        const std::string value = trim(raw.substr(colon + 1));
        if (!nested) {
            if (!value.empty()) {
                // 顶层只允许"组名:"，本项目没有顶层标量
                return std::unexpected(InvalidArgument{
                    "配置第 " + std::to_string(line_no) + " 行：顶层键 '"
                    + key + "' 不接受标量值（本项目配置只有一级分组）"});
            }
            section = key;
            continue;
        }
        if (section.empty()) {
            return std::unexpected(
                InvalidArgument{"配置第 " + std::to_string(line_no) + " 行缩进出现在任何分组之前"});
        }
        if (auto r = apply_key(cfg, section, key, value, line_no); !r) {
            return std::unexpected(r.error());
        }
    }
    return cfg;
}

}  // namespace tinykv
