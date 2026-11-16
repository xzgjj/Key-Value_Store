# 文件用途：tinykv_raft——Raft 纯状态机与日志（不依赖网络与线程，便于确定性测试）。
# 归属：TinyKV-CPP（M2 Raft 共识）。

add_library(tinykv_raft STATIC raftlog.cpp)
target_include_directories(tinykv_raft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_raft PUBLIC tinykv_common tinykv_storage tinykv_proto)
