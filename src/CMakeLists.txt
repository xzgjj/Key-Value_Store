# 文件用途：src 目录增量接入（CLAUDE.md §10：子目录按里程碑创建，禁一次建空树）。
# 归属：TinyKV-CPP（M0 基建）。

add_subdirectory(common)
add_subdirectory(config)
add_subdirectory(storage)
add_subdirectory(raft)
add_subdirectory(txn)       # header-only 接口库：raftstore(peer)/server/client 依赖
add_subdirectory(raftstore)
add_subdirectory(scheduler) # header-only：TSO（M5）
add_subdirectory(server)
add_subdirectory(cli)
