# 文件用途：src/txn 构建定义（header-only：MVCC/事务执行端均为头文件实现）。
# 归属：TinyKV-CPP（M5 Percolator 事务）。

add_library(tinykv_txn INTERFACE)
target_include_directories(tinykv_txn INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_txn INTERFACE tinykv_common tinykv_proto)
