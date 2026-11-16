# 文件用途：tests/unit/raftstore 用例注册（登记表 docs/testing.md §4 是唯一事实来源）。
# 归属：TinyKV-CPP（M4 Multi-Raft 路由层）。

add_tinykv_test(region_route_test SRCS region_route_test.cpp LIBS tinykv_raftstore)
