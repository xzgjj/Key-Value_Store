# 文件用途：tests/unit/server 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M6 运行时）。

add_tinykv_test(lifecycle_test SRCS lifecycle_test.cpp LIBS tinykv_server LEVEL unit)
add_tinykv_test(worker_test SRCS worker_test.cpp LIBS tinykv_server LEVEL unit)
add_tinykv_test(server_app_test SRCS server_app_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(interceptor_test SRCS interceptor_test.cpp LIBS tinykv_server LEVEL unit)
add_tinykv_test(peer_registry_test SRCS peer_registry_test.cpp LIBS tinykv_server LEVEL unit)
add_tinykv_test(http_frame_test SRCS http_frame_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(multi_replica_test SRCS multi_replica_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(lock_registry_test SRCS lock_registry_test.cpp LIBS tinykv_server LEVEL unit)
add_tinykv_test(lock_shards_wiring_test SRCS lock_shards_wiring_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(auth_test SRCS auth_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(admin_endpoints_test SRCS admin_endpoints_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
add_tinykv_test(http_concurrency_test SRCS http_concurrency_test.cpp LIBS tinykv_server tinykv_cli LEVEL unit)
