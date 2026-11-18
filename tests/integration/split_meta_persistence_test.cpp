// 文件用途：⑧/M7-REL-02 集成用例——split 后重启，路由与数据都还在（SPEC-6 §6.7）。
// 归属：TinyKV-CPP（M7 演进）。
//
// 使用场景理解：M6 的行为是"split 之后重启回退成单 region"——`StoreMeta` 只在内存，
// 而 `regions/<id>` 目录里的数据重启后无人纳管（孤儿目录）。这条用例走真实装配路径
// （ServerApp + 真实 HTTP + 真实数据目录），断言三件事：
//   1) 重启后 `/regions` 与重启前一致（元数据落盘）；
//   2) 右半 region 的数据可读（不是"目录在但没人管"）；
//   3) 重启后还能继续 split（next_region_id 不倒退、不与既有 id 冲突）。

#include "cli/cli.h"
#include "server/server_app.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "raft_test_base.h"

namespace {

using tinykv::server::ServerApp;
using tinykv::server::ServerOptions;
using tinykv::testlib::TempDir;

// 一段守护进程生命周期：起 → 跑命令 → 停。返回最后一条命令的输出。
std::string run_daemon(const std::filesystem::path& data_dir,
                       const std::vector<std::vector<std::string>>& steps, bool* all_ok) {
    ServerApp app(ServerOptions{.data_dir = data_dir,
                                .status_port = 0,
                                .worker_interval = std::chrono::milliseconds(2),
                                .log_sink = {},
                                .auth_token = {}});
    const auto started = app.start();
    if (!started.has_value()) {
        *all_ok = false;
        return {};
    }
    const std::string endpoint = "127.0.0.1:" + std::to_string(app.status_port());
    std::string last;
    for (const auto& step : steps) {
        std::vector<std::string> full{"--endpoint", endpoint};
        full.insert(full.end(), step.begin(), step.end());
        std::ostringstream out;
        std::ostringstream err;
        if (tinykv::cli::run_cli(full, out, err) != 0) {
            *all_ok = false;
        }
        last = out.str();
    }
    (void)app.stop();
    return last;
}

// 从 /regions 的 JSON 文本里数出 region 个数（不引 JSON 库：只数 "id": 出现次数）
std::size_t count_regions(const std::string& json) {
    std::size_t n = 0;
    for (std::size_t at = json.find("\"id\":"); at != std::string::npos;
         at = json.find("\"id\":", at + 1)) {
        ++n;
    }
    return n;
}

TEST(SplitMetaPersistenceTest, TopologyAndRightHalfDataSurviveRestart) {
    TempDir tmp{"tinykv_split_meta"};
    const auto data_dir = tmp.path() / "db";
    bool ok = true;

    // 第一段：写两个 key（一个在左半、一个在右半），然后 split
    const std::string before = run_daemon(
        data_dir, {{"put", "a", "va"}, {"put", "z", "vz"}, {"region", "split", "1", "m"}}, &ok);
    ASSERT_TRUE(ok) << "首段命令失败: " << before;

    // 第二段：重启后先看拓扑，再读右半数据
    const std::string regions = run_daemon(data_dir, {{"regions"}}, &ok);
    ASSERT_TRUE(ok) << regions;
    EXPECT_EQ(count_regions(regions), 2U) << "重启后回退成单 region（meta 没落盘）: " << regions;

    const std::string got = run_daemon(data_dir, {{"get", "z"}}, &ok);
    ASSERT_TRUE(ok) << got;
    EXPECT_NE(got.find("vz"), std::string::npos) << "右半数据重启后读不到: " << got;

    // 第三段：重启后继续 split 右半（id 不得与既有冲突）
    const std::string again = run_daemon(data_dir, {{"region", "split", "2", "t"}}, &ok);
    ASSERT_TRUE(ok) << "重启后再 split 失败: " << again;
    const std::string regions3 = run_daemon(data_dir, {{"regions"}}, &ok);
    ASSERT_TRUE(ok) << regions3;
    EXPECT_EQ(count_regions(regions3), 3U) << "二次 split 后应为 3 个 region: " << regions3;
}

}  // namespace
