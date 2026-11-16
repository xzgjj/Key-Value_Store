# 文件用途：一切测试的唯一注册入口（禁止散装 add_test，docs/testing.md §2 硬规则）。
# 归属：TinyKV-CPP（M0 基建，SPEC-0 §0.4；对账脚本 tools/check_test_registry.py）。
#
# 用法：
#   add_tinykv_test(<NAME>
#       SRCS <源文件...>
#       [LIBS <额外链接库...>]
#       [LEVEL unit|simulation|integration|bench]  # → ctest LABEL（docs/testing.md §3 词表）
#       [ARGS <传给可执行参数...>]                 # 同一二进制换跑法 = 换登记名（一登记一跑法）
#       [ENV <VAR=value...>]
#       [TIMEOUT <秒>]
#       [RUN_SERIAL <ON|OFF>]
#       [SKIP_RETURN_CODE <码>])                   # 环境缺失优雅跳过（继承 Mooncake 范式）
#
# 登记表（docs/testing.md §4）是唯一事实来源，本函数只做投影；
# 新增测试必须同步登记（docs/testing.md §5 SOP），否则 CI 第一步即失败。

function(add_tinykv_test NAME)
    cmake_parse_arguments(T "" "TIMEOUT;RUN_SERIAL;SKIP_RETURN_CODE"
        "SRCS;LIBS;ARGS;ENV;LEVEL" ${ARGN})

    # Level 词表守卫（testing.md §6：受控词表，禁止自造）
    set(_TINYKV_LEVELS unit simulation integration bench)
    if(NOT T_LEVEL)
        set(T_LEVEL unit)
    endif()
    if(NOT T_LEVEL IN_LIST _TINYKV_LEVELS)
        message(FATAL_ERROR "add_tinykv_test(${NAME})：非法 LEVEL '${T_LEVEL}'，"
            "允许值：${_TINYKV_LEVELS}")
    endif()

    add_executable(${NAME} ${T_SRCS})
    target_link_libraries(${NAME} PRIVATE tinykv_common GTest::gtest_main ${T_LIBS})
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/tests/testlib)

    add_test(NAME ${NAME} COMMAND ${NAME} ${T_ARGS})

    set_tests_properties(${NAME} PROPERTIES
        LABELS "${T_LEVEL}"
        ENVIRONMENT "${T_ENV}")
    if(T_TIMEOUT)
        set_property(TEST ${NAME} PROPERTY TIMEOUT ${T_TIMEOUT})
    endif()
    if(T_RUN_SERIAL)
        set_property(TEST ${NAME} PROPERTY RUN_SERIAL ${T_RUN_SERIAL})
    endif()
    if(T_SKIP_RETURN_CODE)
        set_property(TEST ${NAME} PROPERTY SKIP_RETURN_CODE ${T_SKIP_RETURN_CODE})
    endif()
endfunction()
