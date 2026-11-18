# 文件用途：src/scheduler 构建定义（header-only：调度器 + TSO）。
# 归属：TinyKV-CPP（M4/M5）。

add_library(tinykv_scheduler INTERFACE)
target_include_directories(tinykv_scheduler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_scheduler INTERFACE tinykv_common tinykv_proto)
