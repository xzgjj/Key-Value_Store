# 文件用途：tinykv_storage 存储引擎库（WAL / SST / Engine），依赖 common，不依赖任何上层。
# 归属：TinyKV-CPP（M1 单机存储，SPEC-1 §1.2）。

add_library(tinykv_storage STATIC
    wal.cpp
    sst.cpp
    engine.cpp)
target_include_directories(tinykv_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_storage PUBLIC tinykv_common)
