// 文件用途：region 观测命令端到端——CLI → HTTP admin 端点 → StoreMeta/Peer/Scheduler/RegionTrace
//          （M4 第⑦b层，UX-04，docs/cli.md §10）。
// 归属：TinyKV-CPP（M4 集成层）。
// 使用场景理解：6 条命令的契约在 §10 已核对定版，这里证明的是"命令真的能跑通并给出可判定输出"：
//   (a) regions / region / region peers 从 StoreMeta 读出真实元数据（split 后能看到两个 region）；
//   (b) region split 走 admin 提案→共识→apply→路由更新全链路，开区间外被拒（红线1）；
//   (c) scheduler status 反映⑥层调度器的真实视图；
//   (d) trace region on|off 落 log/trace-region-<id>.log（rotating 10MB×5）；
//   (e) 非法 id / 不存在 region / 非法 trace 参数分别按退出码 1/3/1 拒绝（§10.2、§4）。

#include "cli/cli.h"
#include "storage/engine.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"
#include "scheduler/scheduler.h"
#include "server/region_trace.h"
#include "server/status_server.h"
#include "common/error_format.h"

#include <gtest/gtest.h>

#include <filesystem>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "temp_dir.h"

namespace {

using namespace tinykv::testlib;   // NOLINT(google-build-using-namespace)
using tinykv::raftstore::Peer;
using tinykv::raftstore::Region;
using tinykv::raftstore::StoreMeta;
using tinykv::Engine;
using tinykv::scheduler::Scheduler;
using tinykv::server::RegionTrace;
using tinykv::server::StatusServer;

// ⑥层契约包名短别名（tinykv.schedulerpb）
namespace schedulerpb = tinykv::schedulerpb;

int run(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    return tinykv::cli::run_cli(args, out, err);
}

// 解析制表符管理帧：split\t<id>\t<key> / trace\t<id>\ton|off
std::vector<std::string> split_frame(const std::string& body) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const auto at = body.find('\t', begin);
        if (at == std::string::npos) {
            parts.push_back(body.substr(begin));
            break;
        }
        parts.push_back(body.substr(begin, at - begin));
        begin = at + 1;
    }
    return parts;
}

std::uint64_t query_id(const std::string& query) {
    const auto at = query.find("id=");
    if (at == std::string::npos) {
        return 0;
    }
    return std::stoull(query.substr(at + 3));
}

// 单节点 store：Peer + StoreMeta + Scheduler + RegionTrace + StatusServer（admin handler 直调）
struct Fixture {
    TempDir tmp{"tinykv_cli_region"};
    std::unique_ptr<tinykv::Engine> engine;
    std::unique_ptr<Peer> peer;
    std::unique_ptr<StoreMeta> meta;
    Scheduler scheduler;
    RegionTrace trace;
    std::unique_ptr<StatusServer> server;
    std::uint64_t client_seq{0};

    explicit Fixture(std::string start = "", std::string end = "")
        : trace(tmp.path() / "log") {
        auto opened = Engine::open({.dir = tmp.path() / "db"});
        EXPECT_TRUE(opened.has_value());
        engine = std::move(*opened);
        const std::vector<tinykv::PeerId> members{tinykv::PeerId{1}};
        tinykv::raft::Config cfg{.id = tinykv::PeerId{1}, .voters = members,
                                 .election_timeout = 4, .heartbeat_interval = 2};
        auto p = Peer::open(cfg, *engine);
        EXPECT_TRUE(p.has_value());
        peer = std::move(*p);
        meta = std::make_unique<StoreMeta>();
        Region region{.id = tinykv::RegionId{1}, .start_key = std::move(start),
                      .end_key = std::move(end),
                      .peers = {tinykv::raftstore::PeerInfo{.id = tinykv::PeerId{1},
                                                            .store_id = 1,
                                                            .is_learner = false}},
                      .epoch = {1, 0}, .size_hint = 128};
        EXPECT_TRUE(meta->add_region(region).has_value());
        peer->set_region(region);
        peer->set_store_meta(meta.get());
        for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
            peer->tick();
        }
        EXPECT_TRUE(peer->is_leader());
        feed_scheduler();

        auto admin = [this](const std::string& method, const std::string& path,
                            const std::string& query,
                            const std::string& body) -> std::string {
            if (method == "GET") {
                return on_get(path, query);
            }
            return on_post(path, body);
        };

        auto started = StatusServer::start("127.0.0.1", 0, [] { return std::string("{}"); },
                                           {}, {}, admin);
        EXPECT_TRUE(started.has_value());
        server = std::move(*started);
    }

    // ⑥层调度器视图：注册 store1 与当前全部 region
    void feed_scheduler() {
        schedulerpb::StoreHeartbeatReq hb;
        hb.set_store_id(1);
        hb.set_region_count(static_cast<std::uint32_t>(meta->region_count()));
        hb.set_total_size(1024);
        hb.set_capacity(1ULL << 30);
        scheduler.on_store_heartbeat(hb);
        for (const auto& r : meta->all_regions()) {
            schedulerpb::RegionHeartbeatReq req;
            req.set_region_id(r.id.value);
            req.set_approximate_size(static_cast<std::uint64_t>(r.size_hint));
            req.mutable_epoch()->set_version(r.epoch.version);
            req.mutable_epoch()->set_conf_ver(r.epoch.conf_ver);
            scheduler.on_region_heartbeat(req);
        }
    }

    [[nodiscard]] std::string region_json(const Region& r) const {
        std::string peers;
        for (const auto& p : r.peers) {
            peers += (peers.empty() ? "" : ",") + std::to_string(p.store_id) +
                     (p.is_learner ? ":learner" : ":voter");
        }
        return "{\"id\":" + std::to_string(r.id.value) +
               ",\"start\":\"" + r.start_key + "\",\"end\":\"" + r.end_key +
               "\",\"version\":" + std::to_string(r.epoch.version) +
               ",\"conf_ver\":" + std::to_string(r.epoch.conf_ver) +
               ",\"size_hint\":" + std::to_string(r.size_hint) +
               ",\"peers\":[" + peers + "]" +
               ",\"trace\":" + (trace.enabled(r.id.value) ? "true" : "false") + "}";
    }

    [[nodiscard]] std::string on_get(const std::string& path, const std::string& query) {
        if (path == "/regions") {
            std::string out;
            for (const auto& r : meta->all_regions()) {
                out += (out.empty() ? "[" : ",") + region_json(r);
            }
            return out + "]";
        }
        if (path == "/region") {
            const auto r = meta->find_region_by_id(tinykv::RegionId{query_id(query)});
            if (!r.has_value()) {
                return {};   // 404 → CLI 退出码 3
            }
            return region_json(*r) + ",\"term\":" + std::to_string(peer->raft().term().value) +
                   ",\"commit\":" + std::to_string(peer->raft().committed().value);
        }
        if (path == "/region/peers") {
            const auto r = meta->find_region_by_id(tinykv::RegionId{query_id(query)});
            if (!r.has_value()) {
                return {};
            }
            std::string out;
            for (const auto& p : r->peers) {
                out += (out.empty() ? "[" : ",") + std::to_string(p.store_id) + ":" +
                       (p.is_learner ? "learner" : "voter") +
                       (p.id.value == peer->raft().leader_id().value ? ":leader" : "");
            }
            return out + "]";
        }
        if (path == "/scheduler/status") {
            return "{\"stores\":" + std::to_string(scheduler.store_count()) +
                   ",\"regions\":" + std::to_string(scheduler.region_count()) +
                   ",\"splits\":" + std::to_string(scheduler.split_count()) +
                   ",\"next_region_id\":" + std::to_string(scheduler.next_region_id()) + "}";
        }
        return {};
    }

    [[nodiscard]] std::string on_post(const std::string& path, const std::string& body) {
        const auto parts = split_frame(body);
        if (path == "/region/split" && parts.size() == 3) {
            const std::uint64_t id = std::stoull(parts[1]);
            const auto target = meta->find_region_by_id(tinykv::RegionId{id});
            if (!target.has_value()) {
                return "ERR region-not-found";
            }
            // AskSplit 预分配（⑥层契约），再走 admin 提案→共识→apply→路由更新（红线1/2）
            schedulerpb::AskSplitReq ask;
            ask.set_region_id(id);
            ask.set_split_key(parts[2]);
            const auto resp = scheduler.ask_split(ask, 1);
            tinykv::cmdpb::RaftCmdRequest req;
            req.mutable_header()->set_client_id(77);
            req.mutable_header()->set_sequence_id(++client_seq);
            auto* admin = req.mutable_admin();
            admin->mutable_split()->set_split_key(parts[2]);
            admin->mutable_split()->set_new_region_id(resp.new_region_id());
            if (!peer->propose(req).has_value()) {
                return "ERR not-leader";
            }
            auto applied = peer->on_ready();
            if (!applied.has_value()) {
                return "ERR " + std::string(tinykv::describe_kind(applied.error()));
            }
            feed_scheduler();
            return "OK new_region_id=" + std::to_string(resp.new_region_id());
        }
        if (path == "/trace" && parts.size() == 3) {
            const std::uint64_t id = std::stoull(parts[1]);
            if (meta->find_region_by_id(tinykv::RegionId{id}).has_value()) {
                trace.set(id, parts[2] == "on");
                return "OK trace " + parts[2];
            }
            return "ERR region-not-found";
        }
        return "ERR bad-request";
    }

    [[nodiscard]] std::string endpoint() const {
        return "127.0.0.1:" + std::to_string(server->port());
    }
};

TEST(CliRegion, ListsRegionsAfterSplit) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "regions"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("\"id\":1"), std::string::npos) << out.str();

    // split 后能同时看到左右两个 region
    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "split", "1", "m"}, out2, err2), 0)
        << err2.str();
    EXPECT_NE(out2.str().find("new_region_id=2"), std::string::npos) << out2.str();

    std::ostringstream out3;
    std::ostringstream err3;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "regions"}, out3, err3), 0) << err3.str();
    EXPECT_NE(out3.str().find("\"id\":2"), std::string::npos) << out3.str();
    EXPECT_NE(out3.str().find("\"start\":\"m\""), std::string::npos) << out3.str();
}

TEST(CliRegion, ShowsRegionDetailAndPeers) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "1"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("\"id\":1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"size_hint\":128"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"commit\":"), std::string::npos) << out.str();

    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "peers", "1"}, out2, err2), 0)
        << err2.str();
    EXPECT_NE(out2.str().find("voter"), std::string::npos) << out2.str();
}

TEST(CliRegion, SplitOutsideRangeRejected) {
    Fixture f("", "z");   // 构造 [,"z")
    // Fixture 默认 region 是 [,")，开区间外 split 会被 apply 拒（红线1）
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "split", "1", "zz"}, out, err), 2)
        << "开区间外 split 必须失败（退出码 2）";
    EXPECT_NE(err.str().find("ERR"), std::string::npos) << err.str();
}

TEST(CliRegion, SchedulerStatusReflectsView) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "scheduler", "status"}, out, err), 0) << err.str();
    EXPECT_NE(out.str().find("\"stores\":1"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("\"regions\":1"), std::string::npos) << out.str();
}

TEST(CliRegion, TraceToggleWritesRegionLog) {
    Fixture f;
    const auto log_path = f.tmp.path() / "log" / "trace-region-1.log";
    ASSERT_FALSE(std::filesystem::exists(log_path)) << "开启前不应有 trace 文件";

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "trace", "region", "1", "on"}, out, err), 0)
        << err.str();
    EXPECT_NE(out.str().find("OK trace on"), std::string::npos) << out.str();
    ASSERT_TRUE(std::filesystem::exists(log_path)) << "开启后 trace 文件必须存在";

    // 关闭后 region 详情应显示 trace:false
    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "trace", "region", "1", "off"}, out2, err2), 0)
        << err2.str();
    std::ostringstream out3;
    std::ostringstream err3;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "1"}, out3, err3), 0);
    EXPECT_NE(out3.str().find("\"trace\":false"), std::string::npos) << out3.str();
}

TEST(CliRegion, InvalidInputsUseContractExitCodes) {
    Fixture f;
    std::ostringstream out;
    std::ostringstream err;
    // 非法 id 格式 → 1（§10.2）
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "abc"}, out, err), 1);
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "0"}, out, err), 1);
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "trace", "region", "1", "bogus"}, out, err), 1);
    // 不存在的 region → 3（404 → NOT_FOUND）
    std::ostringstream out2;
    std::ostringstream err2;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "region", "999"}, out2, err2), 3) << err2.str();
    // 不存在 region 的 trace → 服务端 ERR → 2
    std::ostringstream out3;
    std::ostringstream err3;
    EXPECT_EQ(run({"--endpoint", f.endpoint(), "trace", "region", "999", "on"}, out3, err3), 2);
}

}  // namespace
