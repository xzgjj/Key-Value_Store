# 文件用途：tinykv_server——服务层（Raw API 头文件 + 状态端点实现）。
# 归属：TinyKV-CPP（M2 RaftKV，UX-04）。

find_path(TINYKV_ASIO_INCLUDE_DIR asio.hpp)
if(NOT TINYKV_ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 asio.hpp（本机 MSYS2: pacman -S asio；CI: apt install libasio-dev）")
endif()

# M8 ⑭（SPEC-7 §7.2.4）：TLS 传输依赖 OpenSSL——与 asio 同一套 find_package 纪律
# （本机 MSYS2: pacman -S mingw-w64-ucrt-x86_64-openssl；CI: libssl-dev / vcpkg openssl）
find_package(OpenSSL REQUIRED)
if(NOT OPENSSL_VERSION VERSION_GREATER_EQUAL "1.1.1")
    message(FATAL_ERROR "OpenSSL >= 1.1.1 required (found ${OPENSSL_VERSION})")
endif()

add_library(tinykv_server STATIC status_server.cpp)
target_include_directories(tinykv_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${TINYKV_ASIO_INCLUDE_DIR})
target_link_libraries(tinykv_server PUBLIC tinykv_common tinykv_raftstore tinykv_txn tinykv_cli OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(tinykv_server PUBLIC ws2_32 mswsock)
endif()
find_package(Threads REQUIRED)
target_link_libraries(tinykv_server PUBLIC Threads::Threads)

# M6 ③：组合根可执行——唯一负责"创建并连接全部组件"的入口
add_executable(tinykv-server main.cpp)
target_link_libraries(tinykv-server PRIVATE tinykv_server tinykv_cli)
