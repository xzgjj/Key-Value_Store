// 文件用途：配置解析边界用例（UX-05）——溢出、范围上限、未知键必须当场失败。
// 归属：TinyKV-CPP（M9 还债：参数检查）。
//
// 使用场景理解：配置解析最危险的失败模式是"静默给出一个能跑的值"——
// 旧 parse_size 用 acc*10+digit 无溢出检查，写个天文数字会回绕成 0/小值；
// http_threads/lock_shards 没有上限，写错一位就会启动时创建海量线程/桶。
// 本文件把这些边界的"必须失败"钉死。

#include "config/config.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "temp_dir.h"

namespace {

using tinykv::load_config;
using tinykv::testlib::TempDir;

// 写一个临时 YAML 并加载
tinykv::result<tinykv::Config> load_text(const TempDir& tmp, const std::string& text) {
    const auto path = tmp.path() / "cfg.yaml";
    std::ofstream out(path, std::ios::trunc);
    out << text;
    out.close();
    return load_config(path);
}

TEST(ConfigTest, LoadsValidSections) {
    TempDir tmp{"tinykv_cfg_ok"};
    auto cfg = load_text(tmp, R"(engine:
  dir: ./data
  fsync_on_commit: false
  memtable_limit_mb: 32
  wal_segment_mb: 64
log:
  level: debug
  dir: logs
server:
  host: 127.0.0.1
  auth_token: ""
  lock_shards: 4
  http_threads: 1
  tls_cert: ""
  tls_key: ""
txn:
  cross_region_enabled: false
)");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->engine.dir, "./data");
    EXPECT_FALSE(cfg->engine.fsync_on_commit);
    EXPECT_EQ(cfg->engine.memtable_limit_mb, 32U);
    EXPECT_EQ(cfg->engine.wal_segment_mb, 64U);
    EXPECT_EQ(cfg->log.level, "debug");
    EXPECT_EQ(cfg->server.lock_shards, 4U);
    EXPECT_EQ(cfg->server.http_threads, 1U);
    EXPECT_FALSE(cfg->txn.cross_region_enabled);
}

TEST(ConfigTest, OverflowingSizeIsRejected) {
    TempDir tmp{"tinykv_cfg_overflow"};
    // 远超 size_t：旧实现会回绕成小值并被当成合法配置
    auto cfg = load_text(tmp, "engine:\n  memtable_limit_mb: 99999999999999999999999\n");
    ASSERT_FALSE(cfg.has_value());
    EXPECT_TRUE(std::holds_alternative<tinykv::InvalidArgument>(cfg.error()));
}

TEST(ConfigTest, OutOfRangeLimitsAreRejected) {
    TempDir tmp{"tinykv_cfg_range"};
    EXPECT_FALSE(load_text(tmp, "engine:\n  memtable_limit_mb: 0\n").has_value());
    EXPECT_FALSE(load_text(tmp, "engine:\n  wal_segment_mb: 65537\n").has_value());
    EXPECT_FALSE(load_text(tmp, "server:\n  lock_shards: 0\n").has_value());
    EXPECT_FALSE(load_text(tmp, "server:\n  lock_shards: 4097\n").has_value());
    EXPECT_FALSE(load_text(tmp, "server:\n  http_threads: 257\n").has_value());
    // 边界值本身合法
    EXPECT_TRUE(load_text(tmp, "server:\n  http_threads: 0\n").has_value());
    EXPECT_TRUE(load_text(tmp, "server:\n  lock_shards: 4096\n").has_value());
}

TEST(ConfigTest, UnknownKeyAndBadBoolRejected) {
    TempDir tmp{"tinykv_cfg_unknown"};
    EXPECT_FALSE(load_text(tmp, "engine:\n  no_such_key: 1\n").has_value());
    EXPECT_FALSE(load_text(tmp, "engine:\n  fsync_on_commit: maybe\n").has_value());
}

}  // namespace
