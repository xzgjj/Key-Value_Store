# 文件用途：tests/unit/proto 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M0 基建）。

add_tinykv_test(proto_roundtrip_test SRCS proto_roundtrip_test.cpp LIBS tinykv_proto)
