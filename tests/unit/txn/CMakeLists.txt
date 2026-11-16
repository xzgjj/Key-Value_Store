# 文件用途：tests/unit/txn 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M5 Percolator 事务）。

add_tinykv_test(tso_test SRCS tso_test.cpp LIBS tinykv_proto LEVEL unit)
add_tinykv_test(latch_concurrency_test SRCS latch_concurrency_test.cpp LIBS tinykv_common LEVEL simulation)
