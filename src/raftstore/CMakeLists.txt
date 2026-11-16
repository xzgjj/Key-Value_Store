# 文件用途：tinykv_raftstore——Raft 之上的状态机与提案入口（Peer）。
# 归属：TinyKV-CPP（M2 RaftKV）。

find_path(TINYKV_ASIO_INCLUDE_DIR asio.hpp)
if(NOT TINYKV_ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 asio.hpp（本机 MSYS2: pacman -S asio；CI: apt install libasio-dev）")
endif()

add_library(tinykv_raftstore STATIC peer.cpp tcp_transport.cpp)
target_include_directories(tinykv_raftstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(tinykv_raftstore PUBLIC ${TINYKV_ASIO_INCLUDE_DIR})
target_link_libraries(tinykv_raftstore PUBLIC tinykv_raft tinykv_storage tinykv_proto)
if(WIN32)
    target_link_libraries(tinykv_raftstore PUBLIC ws2_32 mswsock)
endif()
find_package(Threads REQUIRED)
target_link_libraries(tinykv_raftstore PUBLIC Threads::Threads)
