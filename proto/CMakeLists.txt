# 文件用途：proto 生成目标——契约文件编译为 tinykv_proto 库（接口文档即代码）。
# 归属：M0 基建（SPEC-0 §0.1/§0.3.2）。所有层可依赖；proto 目标不依赖任何 src/ 层。

# 根 CMakeLists 已 QUIET 解析一次（建目标）；此处 REQUIRED 兜底并保持自描述
find_package(protobuf CONFIG REQUIRED)

add_library(tinykv_proto STATIC)

# CONFIG 模式：protobuf_generate 由 protobuf 包提供，生成源直接挂到目标上
protobuf_generate(
    TARGET tinykv_proto
    PROTOS common.proto raft.proto kv.proto raft_cmdpb.proto scheduler.proto)

target_include_directories(tinykv_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(tinykv_proto PUBLIC protobuf::libprotobuf)


# 生成代码不属于本项目的代码质量责任：-Wall -Werror 会被 protoc 产物里的
# clang 特有写法（_Nullable/_Nonnull 等）判红。这里只对"生成库"关掉这几条，
# 项目自身代码仍走全量告警（多编译器支持的前提，见 docs/testing.md 编译器矩阵）。
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(tinykv_proto PRIVATE
        -Wno-nullability-extension
        -Wno-unknown-warning-option
        -Wno-unused-parameter)
endif()
