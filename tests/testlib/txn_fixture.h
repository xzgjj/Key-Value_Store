// 文件用途：M5 事务用例共享夹具（si_semantics/txn_resolve/后续 chaos 共用）。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 时钟纪律：now_ms 由测试注入（确定性，不读墙钟）；TSO 为进程内单调分配器。
// alloc_ts() 封装"真实分配一个 ts"——测试里用 current()+1 占号会被后续 begin 撞号
// （si_semantics 首跑踩坑：两个事务拿到同一 start_ts，读己之写分支误命中）。

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "scheduler/tso.h"
#include "storage/engine.h"
#include "temp_dir.h"
#include "txn/client.h"

namespace tinykv::testlib {

class TxnFixture {
public:
    TempDir tmp{"tinykv_txn"};
    std::unique_ptr<Engine> engine;
    tso::MonotonicTsProvider tso;
    std::uint64_t now_ms{0};

    TxnFixture() {
        auto opened = Engine::open({.dir = tmp.path() / "db", .fsync_on_commit = false});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
    }

    TxnFixture(const TxnFixture&) = delete;
    TxnFixture& operator=(const TxnFixture&) = delete;

    [[nodiscard]] txn::TxnClient client() { return txn::TxnClient(*engine, tso); }

    // 真实分配一个 ts（消费计数器，杜绝撞号）
    [[nodiscard]] Ts alloc_ts() { return tso.get_timestamps(1)->front(); }

    void advance(std::uint64_t ms) { now_ms += ms; }

    // 独立提交一个外部事务（模拟"别人"）
    void commit_external(const std::string& key, const std::string& value) {
        auto c = client();
        advance(10);
        (void)c.begin();
        (void)c.set(key, value);
        auto r = c.commit(now_ms);
        EXPECT_TRUE(r.has_value()) << "外部事务提交不应失败";
    }
};

}  // namespace tinykv::testlib
