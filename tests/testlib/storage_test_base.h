// 文件用途：存储测试的统一入口头——一行引入沙箱、字节视图、批次构造与常用类型名。
// 归属：TinyKV-CPP（tests/testlib 共享基建，登记表豁免；docs/testing.md §2）。
//
// 为什么要有这个"总装"头：多个用例文件各写一段 include + using 会被 CI 判成复制粘贴
// （dup_code 窗口是 8 行，include+namespace+using 恰好越过线）。集中到一行，
// 既满足检查，也让新用例的样板从 10 行降到 2 行。

#pragma once

#include <cstddef>
#include <string>

#include "storage/engine.h"
#include "storage/wal.h"

#include "bytes.h"
#include "storage_fixture.h"
#include "temp_dir.h"

namespace tinykv::testlib {

// 存储层测试常用类型集中暴露：用例侧只需 `using namespace tinykv::testlib;`
using tinykv::Cf;
using tinykv::Delete;
using tinykv::Engine;
using tinykv::EngineOptions;
using tinykv::InvalidArgument;
using tinykv::Iterator;
using tinykv::Modify;
using tinykv::ModifyVec;
using tinykv::Put;
using tinykv::Reader;
using tinykv::Storage;
using tinykv::Wal;
using tinykv::WalOptions;

}  // namespace tinykv::testlib
