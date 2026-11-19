# 文件用途：tests/unit/common 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(strong_id_test SRCS strong_id_test.cpp)
add_tinykv_test(kv_error_test SRCS kv_error_test.cpp)
add_tinykv_test(encode_test SRCS encode_test.cpp)
add_tinykv_test(common_log_test SRCS common_log_test.cpp)
add_tinykv_test(compat_test SRCS compat_test.cpp)
add_tinykv_test(config_test SRCS config_test.cpp LIBS tinykv_config)
