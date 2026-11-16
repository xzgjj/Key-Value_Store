# 文件用途：tests/ 四层测试总入口——gtest 锁版接入 + 四层子目录挂载（docs/testing.md §2）。
# 归属：TinyKV-CPP（M0 基建，implementation_plan.md §2 任务 0.3）。
#
# 目录层级与执行语义（登记表 docs/testing.md §4 是唯一事实来源）：
#   testlib/      共享测试基建（豁免登记，header-only 优先）
#   unit/         L1 单元：进程内，≤10s，每次 PR
#   simulation/   L2 确定性模拟：虚拟时钟+网络，种子可复现，≤60s
#   integration/  L3 集成：多进程真网络，夜跑 150 轮纪律
#   bench/        L4 基准：采数不做硬失败

include(FetchContent)

# gtest 锁版（本机 MSYS2 无该包；CI/vcpkg 与本机 FetchContent 同版本，见 vcpkg.json）
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0)
# MSVC 下与工程运行时库统一（MinGW/GCC 无影响）
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(AddTest)

add_subdirectory(testlib)
add_subdirectory(unit)
add_subdirectory(simulation)
add_subdirectory(integration)
add_subdirectory(bench)
