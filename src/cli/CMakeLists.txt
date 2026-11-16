# 文件用途：tinykv-cli——CLI 逻辑库 + 可执行入口（逻辑与 main 分离以便用例直调）。
# 归属：TinyKV-CPP（M1 单机存储，UX-02）。

find_path(TINYKV_ASIO_INCLUDE_DIR asio.hpp)
add_library(tinykv_cli STATIC cli.cpp)
target_include_directories(tinykv_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${TINYKV_ASIO_INCLUDE_DIR})
if(WIN32)
    target_link_libraries(tinykv_cli PUBLIC ws2_32 mswsock)
endif()
target_link_libraries(tinykv_cli PUBLIC tinykv_storage tinykv_config)

add_executable(tinykv-cli main.cpp)
target_link_libraries(tinykv-cli PRIVATE tinykv_cli)
