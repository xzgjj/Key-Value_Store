// 文件用途：Region 元数据仓储（SPEC-6 ⑧ / M7-REL-02）——路由真相落盘与按 meta 重建。
// 归属：TinyKV-CPP（M7 演进：多 Region 的元数据不再只活在内存里）。
//
// 为什么独立文件而不是塞进 Engine 键空间（SPEC-6 §6.2.3 D6-1）：
//   元数据必须在"引擎可能打不开"的情况下仍可读——恢复期恰恰最需要它。
//   代价是多一个文件与一次 fsync，可接受。
//
// 文件布局（`data_dir/meta/regions.pb`，名字沿用 SPEC，内容是本项目自定义二进制：
// 不引 protobuf 只为这种定长小结构——一个 region 的字段全是整数与两个短串）：
//   [magic:4 "TKM1"][count:4][每条 region 变长记录...][crc32c:4]
//   region 记录：[id:8][version:8][conf_ver:8][size_hint:8][start_len:4][start][end_len:4][end]
// 写入顺序：先写 tmp → fsync → 旧文件改名 .bak → tmp 改名正式名。任一步崩溃都至少留一份可读。
// 读取：CRC 不符或长度异常 → 回退 .bak；再不行则报 Corrupted，绝不"修补后继续"（红线 4）。

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/error.h"
#include "raftstore/region.h"

namespace tinykv::raftstore {

class StoreMeta;

class StoreMetaStore {
public:
    // meta_dir 缺省 = data_dir/meta；meta 引用在 load/persist 期间独占访问（调用方保证加锁）
    StoreMetaStore(std::filesystem::path data_dir, StoreMeta& meta, std::string subdir = "meta");

    // 读盘并灌入 StoreMeta。文件不存在 = 首次启动（返回 false 表示"无 meta，按单 region
    // 兼容路径"）； 存在但损坏 = Corrupted（调用方决定是否降级，不允许静默建新库）。
    [[nodiscard]] result<bool> load();

    // 把当前 StoreMeta 全量写盘（apply 路径内调用，幂等）
    [[nodiscard]] result<void> persist();

    [[nodiscard]] std::filesystem::path path() const { return dir_ / kFileName; }

    static constexpr const char* kFileName = "regions.pb";
    static constexpr const char* kBackupName = "regions.pb.bak";

private:
    [[nodiscard]] result<std::vector<Region>> parse_file(const std::filesystem::path& file) const;
    [[nodiscard]] static std::string encode(const std::vector<Region>& regions);

    std::filesystem::path dir_;
    StoreMeta& meta_;
};

}  // namespace tinykv::raftstore
