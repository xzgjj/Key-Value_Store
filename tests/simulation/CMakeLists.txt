# 文件用途：tests/simulation 层测试注册（用例随里程碑增量登记，docs/testing.md §4）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(sim_chaos_test SRCS sim_chaos_test.cpp LIBS tinykv_raft LEVEL simulation)
add_tinykv_test(sim_confchange_test SRCS sim_confchange_test.cpp LIBS tinykv_raft LEVEL simulation)
# 调度器是 header-only（src/scheduler/scheduler.h），只依赖 proto 契约，故不链 tinykv_raft。
add_tinykv_test(sim_scheduler_test SRCS sim_scheduler_test.cpp LIBS tinykv_proto LEVEL simulation)
add_tinykv_test(sim_transfer_test SRCS sim_transfer_test.cpp LIBS tinykv_raft LEVEL simulation)
