// 文件用途：CF 前缀编码与事务时间戳编码——存储层字节布局的唯一实现点
//          （SPEC-0 §0.3.1；SPEC-1 §127 红线：引擎/事务不得自拼 key）。
// 归属：TinyKV-CPP（M0 基建，ENG-01；FUNC-01/13 前置）。
//
// 布局（红线 1：编码一律大端，字节序 = 比较序 = Scan 迭代序）：
//   encode_key(cf, key)      = [cf_id:1B][user_key]
//   encode_ts_key(cf, k, ts) = [cf_id:1B][user_key][ts:8B 大端]
//
// 两个函数都带 cf_id 前缀（2026-11-16 统一）：此前 encode_ts_key 不带 CF，调用方必须
// 记住"哪个函数已经带了前缀、哪个还要自己补"，而这正是"唯一实现点"要消灭的知识。
// 现在只有一条规则：凡进引擎的 key 都经这两个函数之一，前缀由它们负责。
//
// 为什么单字节前缀能同时干两件事：cf_id 定序（Default<Write<Lock）使不同 CF 的同名 key
// 天然不冲突（CF 隔离），同 CF 内保持 user_key 字节序（前缀扫描可用）。

#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "common/error.h"
#include "common/ids.h"

namespace tinykv {

// 列族（P4 事务直接复用：Write/Lock 为 Percolator 两列）
enum class Cf : uint8_t { Default = 0, Write = 1, Lock = 2 };

// CF 前缀编码：空 user_key 返回 InvalidArgument
[[nodiscard]] result<std::string> encode_key(Cf cf, std::span<const std::byte> user_key);

// 事务时间戳编码：CF 前缀 + user_key + 大端 ts 后缀（ts 排序序 = 数值序，SPEC-4 §21）
[[nodiscard]] result<std::string> encode_ts_key(Cf cf, std::span<const std::byte> user_key, Ts ts);

}  // namespace tinykv
