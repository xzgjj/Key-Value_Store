// 文件用途：CLI 事务子命令 smoke（M5 第⑥层，UX-02(txn)，SPEC-4 §3.7 cli_txn_smoke）。
// 归属：TinyKV-CPP（M5 集成层）。
// 使用场景理解：SPEC 验收口径是"--help + 示例"。CLI 是一次性进程，无法跨调用持有事务
// 状态（start_ts 与 writes_ 都在进程内），故交互式子命令按契约返回退出码 1 并指向 --help；
// demo 子命令输出可执行脚本。可判定面：用法/退出码契约 + 示例脚本与 SPEC-4 流程一致。

#include "cli/cli.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

int run(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    return tinykv::cli::run_cli(args, out, err);
}

TEST(CliTxnSmoke, HelpListsSubcommandsAndExitZero) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"txn", "--help"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("demo"), std::string::npos);
    EXPECT_NE(out.str().find("begin"), std::string::npos);
    EXPECT_NE(out.str().find("commit"), std::string::npos);
    EXPECT_NE(out.str().find("rollback"), std::string::npos);
}

TEST(CliTxnSmoke, TopLevelUsageMentionsTxn) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--help"}, out, err), 0);
    EXPECT_NE(out.str().find("txn"), std::string::npos) << "顶层用法必须露出事务入口";
}

TEST(CliTxnSmoke, DemoPrintsPercolatorFlow) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"txn", "demo"}, out, err), 0) << err.str();
    // 示例脚本与 SPEC-4 流程一致：begin → set → get（读己之写）→ commit → 集群读验证
    EXPECT_NE(out.str().find("begin"), std::string::npos);
    EXPECT_NE(out.str().find("set"), std::string::npos);
    EXPECT_NE(out.str().find("commit"), std::string::npos);
}

TEST(CliTxnSmoke, InteractiveSubcommandsExitOneWithPointer) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"txn", "begin"}, out, err), 1);
    EXPECT_NE(err.str().find("txn --help"), std::string::npos) << "必须指向用法";
    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"txn", "commit"}, out2, err2), 1);
}

}  // namespace
