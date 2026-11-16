# 文件用途：tests/integration 层测试注册（用例随里程碑增量登记，docs/testing.md §4）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(tcp_cluster_test SRCS tcp_cluster_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(raft_chaos_n150 SRCS raft_chaos_n150_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(lincheck_readindex SRCS lincheck_readindex_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(snapshot_test SRCS snapshot_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(mr_chaos_n150 SRCS mr_chaos_test.cpp LIBS tinykv_raftstore LEVEL integration)
add_tinykv_test(lincheck_si_test SRCS lincheck_si_test.cpp LIBS tinykv_storage LEVEL integration)
add_tinykv_test(txn_chaos_n150 SRCS txn_chaos_test.cpp LIBS tinykv_raftstore tinykv_proto LEVEL integration)

# 崩溃写入器：独立进程 + _Exit(0)，供 REL-02 的"被强杀"验证使用
add_executable(crash_writer ../tools/crash_writer.cpp)
target_link_libraries(crash_writer PRIVATE tinykv_storage)
if(MINGW)
    # 与 AddTest 一致：静态链接让产物自包含，否则被父进程 spawn 时找不到运行时 DLL
    target_link_options(crash_writer PRIVATE -static)
endif()
add_tinykv_test(crash_recovery_test SRCS crash_recovery_test.cpp LIBS tinykv_storage LEVEL integration)
target_compile_definitions(crash_recovery_test PRIVATE
    CRASH_WRITER_PATH=\"$<TARGET_FILE:crash_writer>\")   # 展开为带引号的字符串字面量
