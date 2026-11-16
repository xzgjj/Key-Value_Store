# 文件用途：tests/simulation 层测试注册（用例随里程碑增量登记，docs/testing.md §4）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(sim_chaos_test SRCS sim_chaos_test.cpp LIBS tinykv_raft LEVEL simulation)
add_tinykv_test(sim_confchange_test SRCS sim_confchange_test.cpp LIBS tinykv_raft LEVEL simulation)
