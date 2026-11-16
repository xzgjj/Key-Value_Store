# 文件用途：tests/integration 层测试注册（用例随里程碑增量登记，docs/testing.md §4）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(tcp_cluster_test SRCS tcp_cluster_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(raft_chaos_n150 SRCS raft_chaos_n150_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(lincheck_readindex SRCS lincheck_readindex_test.cpp LIBS tinykv_raftstore LEVEL integration)
