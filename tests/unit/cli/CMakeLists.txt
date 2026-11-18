# 文件用途：tests/unit/cli 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M1 单机存储）。

add_tinykv_test(cli_local_smoke SRCS cli_local_smoke_test.cpp LIBS tinykv_cli tinykv_config)
add_tinykv_test(cli_status_test SRCS cli_status_test.cpp LIBS tinykv_cli tinykv_config tinykv_server)
add_tinykv_test(cli_cluster_get_test SRCS cli_cluster_get_test.cpp LIBS tinykv_cli tinykv_config tinykv_server tinykv_raftstore)
add_tinykv_test(cli_region_test SRCS cli_region_test.cpp LIBS tinykv_cli tinykv_config tinykv_server tinykv_raftstore tinykv_proto LEVEL unit)
add_tinykv_test(cli_txn_smoke SRCS cli_txn_smoke_test.cpp LIBS tinykv_cli tinykv_config LEVEL unit)
add_tinykv_test(http_client_timeout_test SRCS http_client_timeout_test.cpp LIBS tinykv_cli tinykv_server tinykv_config LEVEL unit)
