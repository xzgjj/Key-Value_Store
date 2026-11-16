// 文件用途：CF 前缀编码与事务时间戳编码——存储层字节布局的唯一实现点
//          （SPEC-0 §0.3.1；SPEC-1 §127 红线：引擎/事务不得自拼 key）。
// 归属：TinyKV-CPP（M0 基建，ENG-01；FUNC-01/13 前置）。
//
// 布局（红线 1：编码一律大端，字节序 = 比较序 = Scan 迭代序）：
//   encode_key(cf, key)  = [cf_id:1B][user_key]      —— cf_id 定序实现 CF 隔离，组内保序可前缀扫描
//   encode_ts_key(k, ts) = [user_key][ts:8B 大端]    —— 同 key 多版本字节序有序（SPEC-4 §21）

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

// 事务时间戳编码：user_key + 大端 ts 后缀（ts 排序序 = 数值序，SPEC-4 §21）
[[nodiscard]] result<std::string> encode_ts_key(std::span<const std::byte> user_key, Ts ts);

}  // namespace tinykv
