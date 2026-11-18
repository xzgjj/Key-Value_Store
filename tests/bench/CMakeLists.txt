# 文件用途：tests/bench 层测试注册（用例随里程碑增量登记，docs/testing.md §4）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(write_bench SRCS write_bench_test.cpp LIBS tinykv_storage LEVEL bench)

add_tinykv_test(read_bench SRCS read_bench_test.cpp LIBS tinykv_raftstore LEVEL bench)
add_tinykv_test(cluster_bench SRCS cluster_bench_test.cpp LIBS tinykv_raftstore LEVEL bench)
add_tinykv_test(tcp_perf_bench SRCS tcp_perf_bench_test.cpp LIBS tinykv_raftstore LEVEL bench)
add_tinykv_test(split_bench SRCS split_bench_test.cpp LIBS tinykv_raftstore LEVEL bench)
add_tinykv_test(heartbeat_bandwidth_bench SRCS heartbeat_bandwidth_bench_test.cpp LIBS tinykv_raft LEVEL bench)
add_tinykv_test(snapshot_bench SRCS snapshot_bench_test.cpp LIBS tinykv_raftstore LEVEL bench)
add_tinykv_test(region_concurrency_bench SRCS region_concurrency_bench_test.cpp LIBS tinykv_server tinykv_cli LEVEL bench)
