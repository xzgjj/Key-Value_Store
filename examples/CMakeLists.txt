# 文件用途：示例目标——只保证编译通过（UX-07：示例进 CI 编译检查），不注册为测试。
# 归属：TinyKV-CPP（M1 单机存储）。

add_executable(embed_example embed_example.cpp)
target_link_libraries(embed_example PRIVATE tinykv_storage)
