# 文件用途：tinykv_config——单文件配置加载（最小 YAML 子集，UX-05）。
# 归属：TinyKV-CPP（M1 单机存储）。

add_library(tinykv_config STATIC config.cpp)
target_include_directories(tinykv_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_config PUBLIC tinykv_common)
