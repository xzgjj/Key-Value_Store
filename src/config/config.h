// 文件用途：单文件配置加载——最小 YAML 子集（一层缩进的键值对），拒绝其余语法并报错（UX-05）。
// 归属：TinyKV-CPP（M1 单机存储，UX-05）。
//
// 为什么不引三方 YAML 库：技术栈锁定（CLAUDE.md §4）里没有它，而本项目需要的只是
// "一层分组 + 标量值"——按用户"优先语言机制"的规则，用 80 行解析器比拉一个依赖更划算。
// 明确不支持的语法（数组、多行字符串、锚点）一律报错而不是静默忽略：配置静默失效比启动失败更糟。

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "common/error.h"

namespace tinykv {

// engine.* 段（语义见 docs/SPEC-1-standalone.md 配置项表）
struct EngineSection {
    std::string dir{"./demo.db"};
    bool fsync_on_commit{true};
    std::size_t memtable_limit_mb{64};
    std::size_t wal_segment_mb{128};
};

// log.* 段
struct LogSection {
    std::string level{"info"};
    std::string dir{"log"};
};

// server.* 段（M7 ⑩：安全边界与监听面）
struct ServerSection {
    std::string host{"127.0.0.1"};  // 未启用鉴权时强制回环（见 docs/cli.md 安全一节）
    std::string auth_token;         // 空 = 不鉴权（启动告警 + 强制绑回环）
};

// txn.* 段（M7 ⑦：跨区事务开关）
struct TxnSection {
    bool cross_region_enabled{false};  // 关 = 跨区 prewrite 明确拒绝（安全侧默认）
};

struct Config {
    EngineSection engine;
    LogSection log;
    ServerSection server;
    TxnSection txn;
};

// 加载配置；文件不存在或语法不支持均返回 InvalidArgument/IoFailed，不使用默认值掩盖错误
[[nodiscard]] result<Config> load_config(const std::filesystem::path& path);

}  // namespace tinykv
