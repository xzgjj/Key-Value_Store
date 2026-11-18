# 文件用途：TinyKV 工程根构建入口——语言标准/警告基线 + 子目录按里程碑增量接入。
# 归属：TinyKV-CPP（M0 基建，SPEC-0 §0.2/§0.4；目录克制原则见 CLAUDE.md §10）。

cmake_minimum_required(VERSION 3.22)

project(tinykv
    VERSION 0.1.0
    DESCRIPTION "TinyKV-CPP：Raft 分布式 KV 存储的工程化复刻"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 语言标准（默认 23；20/17 用于拓宽支持面）
#   为什么可调：C++23 的 std::expected / std::span 在 17 下不存在，项目用
#   src/common/compat.h 提供等价物；C++20 是"原生 std::expected + 无 concept 依赖"的中间档。
#   17 档的两条已知代价（已在 compat.h 与 docs 里写明）：
#     1) 指定初始化器（.field = v）按 GNU 扩展使用，故 17 档不启用 -Wpedantic；
#     2) std::string::starts_with / operator<=> 由 compat.h 的等价函数替代。
# ---------------------------------------------------------------------------
set(TINYKV_CXX_STANDARD "23" CACHE STRING "C++ 标准：23（默认）/20/17")
set(CMAKE_CXX_STANDARD ${TINYKV_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 依赖在根作用域解析：IMPORTED 目标全树可见（cmake/AddTest.cmake 需读取其 DLL 位置）
find_package(protobuf CONFIG QUIET)

# FetchContent 产物统一落 build/.deps（点目录）：
# 1) 全部构建目录（含 TinyKV-CI 的 build/ci）共用一份下载，避免重复拉取；
# 2) 三方源码自带的 .md 文档不得进入文档检查范围（docs CI 只认点前缀为工具噪声），
#    规则资产不动，以目录约定隔离。
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/.deps" CACHE PATH
    "FetchContent 产物目录（build/.deps：gitignored + 文档检查跳过点目录）")

# 本地已落地的三方源码优先复用：两棵构建树（win-debug / ci）共享同一份 build/.deps，
# 但 FetchContent 的"已填充"状态是按构建目录记账的——删掉某棵树后它会重跑 git clone。
# 本机 git 的 HTTPS 后端可能不可用（"Unsupported SSL backend 'schannel'"），届时 configure 直接失败。
# 因此只要源码目录已在，就把它固定为源目录（版本仍由 tests/CMakeLists.txt 的 GIT_TAG 表达；
# 要升级 gtest 请先删掉 build/.deps/googletest-src 再配置）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/.deps/googletest-src/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST
        "${CMAKE_CURRENT_SOURCE_DIR}/build/.deps/googletest-src" CACHE PATH
        "本地 googletest 源码（存在即跳过网络拉取）")
endif()

# 警告即失败（SPEC.md §3 阈值总表 / check_code.py 同口径 -Wall -Wextra -Werror）
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
    # -Wpedantic 只在 20 及以上启用：17 档的指定初始化器是 GNU 扩展，
    # 开着它等于把"可编译"变成"不可编译"（见项目根 CMakeLists 语言标准一节）。
    if(NOT TINYKV_CXX_STANDARD STREQUAL "17")
        add_compile_options(-Wpedantic)
    endif()
endif()

# MinGW（Windows + GNU）运行时静态链接：可执行自包含，规避 PATH 上异源 MinGW DLL
# 抢占解析（如 Git for Windows 自带旧 libstdc++-6.dll 导致 0xc0000139）；Linux/MSVC 不受影响。
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static)
endif()

# ---------------------------------------------------------------------------
# 产物体积 / 编译并发上限（2026-11-18 实测后收口）
#   实测：71 个测试可执行，单个 47MB 中 32MB(89%) 是 DWARF 调试段，代码数据仅 3.6MB；
#         build/win-debug 与 build/ci 各存一份 → build/ 合计 4.0GB。
#   默认保持工具链的 -g（=gcc -g2：类型/变量齐全，无宏信息），不牺牲调试能力；
#   体积靠"不留重复构建树 + 按需清理"控制，而不是降调试信息。
#   开关（均可 -D 覆盖）：
#     TINYKV_MIN_DEBUG=ON    改 -g1（仅行号，产物约减半，但 info locals 不可用）
#     TINYKV_COMPILE_JOBS=N  并发编译上限（0=不限制）；实测单个 cc1plus 峰值约 1.4GB
# ---------------------------------------------------------------------------
option(TINYKV_MIN_DEBUG "Debug 用 -g1（仅保留文件/行号，丢局部变量；省约一半体积）" OFF)

set(TINYKV_COMPILE_JOBS "8" CACHE STRING
    "并发编译上限（0=不限制，跟随 ninja -j）；实测单个 cc1plus 峰值约 1.4GB")
set(TINYKV_LINK_JOBS "2" CACHE STRING "并发链接上限（0=不限制）；静态链接峰值约 1.5~2GB")

# 只做"附加"，不覆盖 CMAKE_CXX_FLAGS_DEBUG：保留工具链默认值与用户自定义入口
if(NOT MSVC AND TINYKV_MIN_DEBUG)
    add_compile_options($<$<CONFIG:Debug>:-g1>)
endif()

# Ninja job pool：先于任何 add_subdirectory 设置，目标创建时才继承 CMAKE_JOB_POOL_*
# 语法口径：CMAKE_JOB_POOLS 是 "名称=深度" 的分号列表（不是 ninja 原生的 "名称:深度"）
if(CMAKE_GENERATOR MATCHES "Ninja")
    set(_TINYKV_POOLS "")
    if(NOT TINYKV_COMPILE_JOBS STREQUAL "0")
        list(APPEND _TINYKV_POOLS "compile=${TINYKV_COMPILE_JOBS}")
    endif()
    if(NOT TINYKV_LINK_JOBS STREQUAL "0")
        list(APPEND _TINYKV_POOLS "link=${TINYKV_LINK_JOBS}")
    endif()
    if(_TINYKV_POOLS)
        set(CMAKE_JOB_POOLS "${_TINYKV_POOLS}" CACHE STRING "Ninja 并发池" FORCE)
        if(NOT TINYKV_COMPILE_JOBS STREQUAL "0")
            set(CMAKE_JOB_POOL_COMPILE compile)
        endif()
        if(NOT TINYKV_LINK_JOBS STREQUAL "0")
            set(CMAKE_JOB_POOL_LINK link)
        endif()
    endif()
endif()

enable_testing()

# src/ 子目录按里程碑增量创建（SPEC-0 红线 6，禁一次建空树）：
# EXISTS 守卫保证任一提交点都可配置构建；目录落地后守卫即生效为常规接入。
# proto 先于 src：raft 库链接 tinykv_proto，目标必须先存在
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proto/CMakeLists.txt")
    add_subdirectory(proto)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()
# proto 先于 tests：add_tinykv_test 需读取 protobuf 导入目标的 DLL 位置注入测试进程 PATH
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
