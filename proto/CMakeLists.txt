# 文件用途：proto 生成目标——三个契约文件编译为 tinykv_proto 库（接口文档即代码）。
# 归属：M0 基建（SPEC-0 §0.1/§0.3.2）。所有层可依赖；proto 目标不依赖任何 src/ 层。

# 根 CMakeLists 已 QUIET 解析一次（建目标）；此处 REQUIRED 兜底并保持自描述
find_package(protobuf CONFIG REQUIRED)

add_library(tinykv_proto STATIC)

# CONFIG 模式：protobuf_generate 由 protobuf 包提供，生成源直接挂到目标上
protobuf_generate(
    TARGET tinykv_proto
    PROTOS raft.proto kv.proto raft_cmdpb.proto)

target_include_directories(tinykv_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(tinykv_proto PUBLIC protobuf::libprotobuf)

