# 文件用途：TinyKV 工程根构建入口——语言标准/警告基线 + 子目录按里程碑增量接入。
# 归属：TinyKV-CPP（M0 基建，SPEC-0 §0.2/§0.4；目录克制原则见 CLAUDE.md §10）。

cmake_minimum_required(VERSION 3.22)

project(tinykv
    VERSION 0.1.0
    DESCRIPTION "TinyKV-CPP：Raft 分布式 KV 存储的工程化复刻"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 警告即失败（SPEC.md §3 阈值总表 / check_code.py 同口径 -Wall -Wextra -Werror）
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

# src/ 子目录按里程碑增量创建（SPEC-0 红线 6，禁一次建空树）：
# EXISTS 守卫保证任一提交点都可配置构建；目录落地后守卫即生效为常规接入。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proto/CMakeLists.txt")
    add_subdirectory(proto)
endif()
