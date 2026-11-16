# 文件用途：tinykv_raftstore——Raft 之上的状态机与提案入口（Peer）。
# 归属：TinyKV-CPP（M2 RaftKV）。

add_library(tinykv_raftstore STATIC peer.cpp)
target_include_directories(tinykv_raftstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_raftstore PUBLIC tinykv_raft tinykv_storage tinykv_proto)
