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

# FetchContent 产物统一落 build/.deps（点目录）：
# 1) 全部构建目录（含 TinyKV-CI 的 build/ci）共用一份下载，避免重复拉取；
# 2) 三方源码自带的 .md 文档不得进入文档检查范围（docs CI 只认点前缀为工具噪声），
#    规则资产不动，以目录约定隔离。
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/.deps" CACHE PATH
    "FetchContent 产物目录（build/.deps：gitignored + 文档检查跳过点目录）")

# 警告即失败（SPEC.md §3 阈值总表 / check_code.py 同口径 -Wall -Wextra -Werror）
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# MinGW（Windows + GNU）运行时静态链接：可执行自包含，规避 PATH 上异源 MinGW DLL
# 抢占解析（如 Git for Windows 自带旧 libstdc++-6.dll 导致 0xc0000139）；Linux/MSVC 不受影响。
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static)
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
