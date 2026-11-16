# 文件用途：tests/unit/cli 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M1 单机存储）。

add_tinykv_test(cli_local_smoke SRCS cli_local_smoke_test.cpp LIBS tinykv_cli tinykv_config)
add_tinykv_test(cli_status_test SRCS cli_status_test.cpp LIBS tinykv_cli tinykv_config tinykv_server)
