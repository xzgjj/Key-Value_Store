# 文件用途：tests/unit/storage 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M1 单机存储）。

add_tinykv_test(memtable_test SRCS memtable_test.cpp LIBS tinykv_storage)
add_tinykv_test(wal_test SRCS wal_test.cpp LIBS tinykv_storage)
