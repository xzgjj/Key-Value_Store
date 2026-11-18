// 文件用途：跨区事务崩溃写入器——按 kill_step 执行 2PC 的前 N 步后 _Exit(0)（等价 kill -9）。
// 归属：TinyKV-CPP（M7 演进，SPEC-6 §6.7 `txn_cross_region_chaos_random` 的工具，非测试本体）。
//
// 为什么需要独立进程：进程内"关掉再重开"走正常析构（会 flush），验证不到"只靠 WAL 恢复"
// 的崩溃语义（同 crash_writer.cpp 的论证）。跨区事务的中断窗口必须建立在
// "已完成的每一步都已 fsync、未完成的半途状态原样留在盘上"的前提上——随机 chaos 的
// 价值（发现未想到的窗口）只有在这种真崩溃语义下才成立。
//
// 协议步骤（与 TxnClient::commit_grouped 的阶段机一一对应）：
//   step 1  PREPARE 主组（primary 所在引擎，primary_required=true）
//   step 2  PREPARE 副组（另一引擎，primary_required=false，锚点由主组保证）
//   step 3  COMMIT_PRIMARY（原子锚点落定）
//   step 4  COMMIT_SECONDARY
// kill_step = 完成多少步后死亡：0=一步未做，4=全部做完正常退出。
// 每步完成即落盘（fsync_on_commit=true），kill 后父进程重开引擎只可能看到
// "前 kill_step 步的持久化结果"——这正是随机中断要枚举的状态空间。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "storage/engine.h"
#include "txn/mvcc.h"

namespace {

using tinykv::Engine;
using tinykv::Ts;
using tinykv::txn::Mutation;
using tinykv::txn::MvccTxn;

Mutation put(std::string key, std::string value) {
    return Mutation{.op = Mutation::Op::kPut, .key = std::move(key), .value = std::move(value)};
}

// 死亡点：每步完成后若计数已到 kill_step，打印并跳过析构退出。
void die_if(int done, int kill_step) {
    if (done >= kill_step) {
        std::printf("KILLED %d\n", done);
        std::fflush(stdout);
        std::_Exit(0);  // 不 flush 存储层、不做静态析构——"写到一半被强杀"
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "用法: txn_chaos_writer <主组目录> <副组目录> <primary_key> <other_key> "
                     "<kill_step 0..4>\n");
        return 2;
    }
    const std::string dir_primary = argv[1];
    const std::string dir_other = argv[2];
    const std::string key_primary = argv[3];
    const std::string key_other = argv[4];
    const int kill_step = std::atoi(argv[5]);
    if (kill_step < 0 || kill_step > 4) {
        std::fprintf(stderr, "kill_step 必须在 0..4\n");
        return 2;
    }

    auto primary_side = Engine::open({.dir = dir_primary, .fsync_on_commit = true});
    auto other_side = Engine::open({.dir = dir_other, .fsync_on_commit = true});
    if (!primary_side.has_value() || !other_side.has_value()) {
        std::fprintf(stderr, "打开引擎失败\n");
        return 3;
    }

    // 固定时钟：start_ts/commit_ts/now 由父进程约定，目录每轮唯一故无版本串扰
    const Ts start_ts{7000};
    const Ts commit_ts{7100};
    const std::uint64_t now_ms = 1000;
    const std::uint64_t ttl_ms = 3000;

    // step 1：PREPARE 主组（primary 必须在本批——决议锚点）
    {
        MvccTxn mvcc(**primary_side);
        const std::vector<Mutation> batch{put(key_primary, "vp")};
        if (!mvcc.prewrite(batch, key_primary, start_ts, ttl_ms, now_ms).has_value()) {
            std::fprintf(stderr, "主组 prewrite 失败\n");
            return 4;
        }
    }
    die_if(1, kill_step);

    // step 2：PREPARE 副组（primary 在别组 → primary_required=false）
    {
        MvccTxn mvcc(**other_side);
        const std::vector<Mutation> batch{put(key_other, "vo")};
        if (!mvcc.prewrite(batch, key_primary, start_ts, ttl_ms, now_ms, false).has_value()) {
            std::fprintf(stderr, "副组 prewrite 失败\n");
            return 4;
        }
    }
    die_if(2, kill_step);

    // step 3：COMMIT_PRIMARY——原子锚点。此步之后事务"已提交"，恢复路径只会正向推进
    {
        MvccTxn mvcc(**primary_side);
        const std::vector<std::string> keys{key_primary};
        if (!mvcc.commit(keys, start_ts, commit_ts).has_value()) {
            std::fprintf(stderr, "主组 commit 失败\n");
            return 4;
        }
    }
    die_if(3, kill_step);

    // step 4：COMMIT_SECONDARY。中断在这里也安全：读者遇副组锁 → 查 primary → 正向推进
    {
        MvccTxn mvcc(**other_side);
        const std::vector<std::string> keys{key_other};
        if (!mvcc.commit(keys, start_ts, commit_ts).has_value()) {
            std::fprintf(stderr, "副组 commit 失败\n");
            return 4;
        }
    }
    die_if(4, kill_step);

    std::printf("DONE\n");
    std::fflush(stdout);
    return 0;
}
