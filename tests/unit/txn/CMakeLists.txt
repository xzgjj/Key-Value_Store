# 文件用途：tests/unit/txn 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M5 Percolator 事务）。

add_tinykv_test(tso_test SRCS tso_test.cpp LIBS tinykv_proto LEVEL unit)
add_tinykv_test(latch_concurrency_test SRCS latch_concurrency_test.cpp LIBS tinykv_common LEVEL simulation)
add_tinykv_test(mvcc_test SRCS mvcc_test.cpp LIBS tinykv_storage LEVEL unit)
add_tinykv_test(si_semantics_test SRCS si_semantics_test.cpp LIBS tinykv_storage LEVEL unit)
add_tinykv_test(txn_resolve_test SRCS txn_resolve_test.cpp LIBS tinykv_storage LEVEL unit)
add_tinykv_test(txn_ambiguity_test SRCS txn_ambiguity_test.cpp LIBS tinykv_storage LEVEL unit)
add_tinykv_test(txn_peer_test SRCS txn_peer_test.cpp LIBS tinykv_raftstore tinykv_proto LEVEL unit)
add_tinykv_test(gc_test SRCS gc_test.cpp LIBS tinykv_storage tinykv_raftstore LEVEL unit)
add_tinykv_test(region_grouper_test SRCS region_grouper_test.cpp LEVEL unit)
