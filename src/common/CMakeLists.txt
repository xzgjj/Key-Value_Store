# 文件用途：tinykv_common 公共库（强类型 ID / 结构化错误 / 编码 / 日志封装，被所有层依赖）。
# 归属：M0 基建（SPEC-0 §0.2 依赖方向：common 不依赖任何上层）。

find_package(spdlog CONFIG REQUIRED)

add_library(tinykv_common STATIC
    encode.cpp
    log.cpp)
target_include_directories(tinykv_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tinykv_common PUBLIC spdlog::spdlog_header_only)
