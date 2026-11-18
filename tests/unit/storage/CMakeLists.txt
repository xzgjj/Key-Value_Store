# 文件用途：tests/unit/storage 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M1 单机存储）。

add_tinykv_test(memtable_test SRCS memtable_test.cpp LIBS tinykv_storage)
add_tinykv_test(wal_test SRCS wal_test.cpp LIBS tinykv_storage)
add_tinykv_test(sst_test SRCS sst_test.cpp LIBS tinykv_storage)
add_tinykv_test(wal_crash_test SRCS wal_crash_test.cpp LIBS tinykv_storage)
add_tinykv_test(wal_group_commit_test SRCS wal_group_commit_test.cpp LIBS tinykv_storage)
add_tinykv_test(engine_get_put_test SRCS engine_get_put_test.cpp LIBS tinykv_storage)
add_tinykv_test(engine_scan_test SRCS engine_scan_test.cpp LIBS tinykv_storage)
add_tinykv_test(engine_cf_test SRCS engine_cf_test.cpp LIBS tinykv_storage)
add_tinykv_test(engine_scan_concurrent_test SRCS engine_scan_concurrent_test.cpp LIBS tinykv_storage)
add_tinykv_test(engine_compaction_test SRCS engine_compaction_test.cpp LIBS tinykv_storage)
add_tinykv_test(durability_guard_test SRCS durability_guard_test.cpp LIBS tinykv_storage tinykv_raftstore LEVEL unit)
