# 文件用途：tests/unit/raft 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M2 Raft 共识）。

add_tinykv_test(raft_log_test SRCS raft_log_test.cpp LIBS tinykv_raft)
add_tinykv_test(raft_step_test SRCS raft_step_test.cpp LIBS tinykv_raft)
add_tinykv_test(raft_kv_test SRCS raft_kv_test.cpp LIBS tinykv_raftstore)
add_tinykv_test(readindex_test SRCS readindex_test.cpp LIBS tinykv_raft)
