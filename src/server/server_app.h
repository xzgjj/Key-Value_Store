// 文件用途：ServerApp——组合根（M6 第③层）：唯一负责"创建并连接全部组件"的单元。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 装配顺序（= 注册顺序，LifecycleManager 保证逆序关闭）：
//   Storage(Engine) → Raft(Peer 单节点组) → Worker(泵循环) → StatusServer(HTTP)
// 并发模型：Peer 非线程安全 → 全部 Peer/Storage 访问经 peer_mu_ 串行（课程级粗粒度锁，
// per-peer 分片随多 region 演进）；Worker 后台泵与 HTTP handler 共用同一把锁。
// 服务端事务会话：TxnSessionManager（会话作用域），执行端 = PeerTxnExecutor（经 raft）。
//
// Service 自身可嵌套：ServerApp 实现 Service，可被更外层（多 store 拓扑）再组装。

#pragma once

#include "raftstore/peer.h"
#include "scheduler/tso.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include "server/peer_txn.h"
#include "server/service.h"
#include "server/status_server.h"
#include "server/txn_sessions.h"
#include "server/worker.h"
#include "storage/engine.h"
#include "txn/executor.h"

namespace tinykv::server {

struct ServerOptions {
    std::filesystem::path data_dir;      // 必填
    std::string host{"127.0.0.1"};
    std::uint16_t status_port{0};        // 0 = 随机
    std::uint64_t election_timeout{8};
    std::uint64_t heartbeat{3};
    std::chrono::milliseconds worker_interval{10};
};

class ServerApp final : public Service {
public:
    explicit ServerApp(ServerOptions opt) : opt_(std::move(opt)) {}

    [[nodiscard]] std::string_view name() const override { return "tinykv-server"; }

    // ---- Service：装配即 start，逆序即 stop ----
    [[nodiscard]] result<void> start() override {
        if (running_) {
            return {};
        }
        auto opened = Engine::open({.dir = opt_.data_dir, .fsync_on_commit = false});
        if (!opened.has_value()) {
            return std::unexpected(opened.error());
        }
        engine_ = std::move(*opened);

        const std::vector<PeerId> voters{PeerId{1}};   // 课程级拓扑：单 store 单节点组
        raft::Config cfg{.id = PeerId{1}, .voters = voters,
                         .election_timeout = opt_.election_timeout,
                         .heartbeat_interval = opt_.heartbeat};
        auto peer = raftstore::Peer::open(cfg, *engine_);
        if (!peer.has_value()) {
            return std::unexpected(peer.error());
        }
        peer_ = std::move(*peer);
        raftstore::Region region{.id = RegionId{1}, .start_key = "", .end_key = "",
                                 .peers = {}, .epoch = {1, 0}, .size_hint = 0};
        if (!meta_.add_region(region).has_value()) {
            return std::unexpected(InvalidArgument{"region 1 注册失败"});
        }
        peer_->set_region(region);
        peer_->set_store_meta(&meta_);
        for (int i = 0; i < 20 && !peer_->is_leader(); ++i) {
            peer_->tick();
        }
        (void)peer_->on_ready();

        // 泵回调在闸内执行（executor 持闸期间调用），故这里不再加锁
        exec_ = std::make_unique<PeerTxnExecutor>(
            [this]() -> raftstore::Peer& { return *peer_; },
            [this] {
                peer_->tick();
                (void)peer_->on_ready();
            },
            [this](std::uint64_t idx) { return peer_->applied_at_least(idx); },
            &peer_mu_);
        sessions_ = std::make_unique<TxnSessionManager>(*exec_, tso_);

        auto started = StatusServer::start(opt_.host, opt_.status_port,
                                           [this] { return status_json(); },
                                           [this](const std::string& q) { return kv_read(q); },
                                           [this](const std::string& body) { return kv_write(body); },
                                           [this](const std::string& m, const std::string& p,
                                                  const std::string& q, const std::string& b) {
                                               return admin(m, p, q, b);
                                           });
        if (!started.has_value()) {
            return std::unexpected(started.error());
        }
        status_ = std::move(*started);

        worker_ = std::make_unique<Worker>(
            [this] {
                std::lock_guard lock(peer_mu_);   // 后台泵与 HTTP 共闸（类注释）
                peer_->tick();
                (void)peer_->on_ready();
            },
            opt_.worker_interval);
        if (auto r = worker_->start(); !r.has_value()) {
            return std::unexpected(r.error());
        }
        running_ = true;
        return {};
    }

    [[nodiscard]] result<void> stop() override {
        if (!running_) {
            return {};
        }
        running_ = false;
        std::string first_err;
        if (auto r = worker_->stop(); !r.has_value() && first_err.empty()) {
            first_err = "worker: " + std::string(tinykv::describe_kind(r.error()));
        }
        worker_.reset();
        status_.reset();   // HTTP 先下线（拒绝新请求），再收引擎
        sessions_.reset();
        exec_.reset();
        peer_.reset();
        if (auto r = engine_->close(); !r.has_value() && first_err.empty()) {
            first_err = "engine: " + std::string(tinykv::describe_kind(r.error()));
        }
        engine_.reset();
        if (!first_err.empty()) {
            return std::unexpected(IoFailed{.path = "server-app:stop", .reason = first_err});
        }
        return {};
    }

    [[nodiscard]] std::uint16_t status_port() const { return status_ ? status_->port() : 0; }
    [[nodiscard]] std::uint16_t bound_port() const { return status_port(); }
    [[nodiscard]] std::size_t active_sessions() const { return sessions_ ? sessions_->active() : 0; }
    void advance_clock(std::uint64_t ms) { now_ms_ += ms; }   // 确定性时钟推进（测试/运维）

private:
    // Worker 后台泵与 HTTP handler 共用 peer_mu_（见类注释）
    void pump_locked() {
        std::lock_guard lock(peer_mu_);
        peer_->tick();
        (void)peer_->on_ready();
    }

    // ---- kv 面端点（经 raft 提案，"写走共识"红线不豁免） ----
    [[nodiscard]] std::string kv_read(const std::string& query) {
        const auto key = http_query_param(query, "key");
        if (key.empty()) {
            return {};
        }
        std::lock_guard lock(peer_mu_);
        kv_reads_.fetch_add(1, std::memory_order_relaxed);
        auto got = peer_->get(Cf::Default, key);
        if (!got.has_value()) {
            return "{\"error\":\"" + std::string(tinykv::describe_kind(got.error())) + "\"}";
        }
        if (!got->value.has_value()) {
            return "{\"found\":false}";
        }
        return "{\"found\":true,\"value\":\"" + escape(*got->value) + "\"}";
    }

    [[nodiscard]] std::string kv_write(const std::string& body) {
        const auto parts = split_frame(body);
        if (parts.size() < 2) {
            return "ERR bad-request";
        }
        std::lock_guard lock(peer_mu_);
        kv_writes_.fetch_add(1, std::memory_order_relaxed);
        const auto expect = peer_->raft().last_index().value + 1;
        cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(1);
        req.mutable_header()->set_sequence_id(expect);
        if (parts[0] == "put" && parts.size() == 3) {
            auto* one = req.add_requests();
            one->mutable_put()->set_key(parts[1]);
            one->mutable_put()->set_value(parts[2]);
        } else if (parts[0] == "del" && parts.size() == 2) {
            auto* one = req.add_requests();
            one->mutable_delete_()->set_key(parts[1]);
        } else {
            return "ERR bad-request";
        }
        if (!peer_->propose(req).has_value()) {
            return "ERR not-leader";
        }
        for (int round = 0; round < 400; ++round) {
            peer_->tick();
            (void)peer_->on_ready();
            if (peer_->applied_at_least(expect)) {
                return "OK";
            }
        }
        return "ERR apply-timeout";
    }

    // ---- 事务/观测端点（docs/cli.md §11；会话作用域） ----
    [[nodiscard]] std::string admin(const std::string& method, const std::string& path,
                                    const std::string& query, const std::string& body) {
        txn_ops_.fetch_add(1, std::memory_order_relaxed);
        return method == "POST" ? admin_post(path, body) : admin_get(path, query);
    }

    // POST 族：会话写操作 + GC（executor 自带闸，此处不加 peer_mu_）
    [[nodiscard]] std::string admin_post(const std::string& path, const std::string& body) {
        const auto parts = split_frame(body);
        if (path == "/txn/begin") {
            auto sid = sessions_->begin();
            return sid.has_value() ? "OK session=" + *sid
                                   : "ERR " + std::string(tinykv::describe_kind(sid.error()));
        }
        if (path == "/txn/set" || path == "/txn/del") {
            return admin_txn_buffered(path == "/txn/set", parts);
        }
        if (path == "/txn/commit" || path == "/txn/rollback") {
            return admin_txn_finish(path == "/txn/commit", parts);
        }
        if (path == "/txn/gc") {
            const Ts safe_point{parts.size() == 2 ? std::stoull(parts[1])
                                                  : sessions_safe_point()};
            auto r = exec_->gc(safe_point.value);
            return r.has_value() ? "OK gc"   // 计数在 apply 侧日志
                                 : "ERR " + std::string(tinykv::describe_kind(r.error()));
        }
        return {};
    }

    // 会话写缓冲（set/del）
    [[nodiscard]] std::string admin_txn_buffered(bool is_set,
                                                 const std::vector<std::string>& parts) {
        auto r = is_set
                     ? (parts.size() == 4 ? sessions_->set(parts[1], parts[2], parts[3])
                                          : std::unexpected(
                                                InvalidArgument{"set 需 session/key/value"}))
                     : (parts.size() == 3
                            ? sessions_->del(parts[1], parts[2])
                            : std::unexpected(InvalidArgument{"del 需 session/key"}));
        return r.has_value() ? "OK" : "ERR " + std::string(tinykv::describe_kind(r.error()));
    }

    // 会话终结（commit/rollback）
    [[nodiscard]] std::string admin_txn_finish(bool is_commit,
                                               const std::vector<std::string>& parts) {
        if (parts.size() != 2) {
            return "ERR bad-request";
        }
        if (!is_commit) {
            auto r = sessions_->rollback(parts[1]);
            return r.has_value() ? "OK" : "ERR " + std::string(tinykv::describe_kind(r.error()));
        }
        auto r = sessions_->commit(parts[1], now_ms_);
        return r.has_value() ? "OK commit_ts=" + std::to_string(r->value)
                             : "ERR " + std::string(tinykv::describe_kind(r.error()));
    }

    // GET 族：会话读 + 观测
    [[nodiscard]] std::string admin_get(const std::string& path, const std::string& query) {
        if (path == "/txn/get") {
            auto r = sessions_->get_verbose(http_query_param(query, "session"),
                                            http_query_param(query, "key"), now_ms_);
            if (!r.has_value()) {
                return {};
            }
            return "{\"value\":\"" + escape(*r) + "\"}";
        }
        if (path == "/txn/status") {
            const auto sid = http_query_param(query, "session");
            auto st = sessions_->session_start_ts(sid);
            if (!st.has_value()) {
                return {};
            }
            return "{\"session\":\"" + sid + "\",\"start_ts\":" + std::to_string(st->value) +
                   "}";
        }
        if (path == "/regions") {
            std::string out;
            for (const auto& r : meta_.all_regions()) {
                out += (out.empty() ? "[" : ",") + region_json(r);
            }
            return out + "]";
        }
        if (path == "/region") {
            return region_detail(http_query_param(query, "id"));
        }
        return {};   // 未知 admin 路径 → 404
    }

    [[nodiscard]] std::string region_detail(const std::string& id_text) {
        auto r = meta_.find_region_by_id(RegionId{id_text.empty() ? 0 : std::stoull(id_text)});
        if (!r.has_value()) {
            return {};
        }
        std::string base = region_json(*r);
        base.pop_back();   // 去掉尾 }，拼入 term/commit 后再闭合
        return base + ",\"term\":" + std::to_string(peer_->raft().term().value) +
               ",\"commit\":" + std::to_string(peer_->raft().committed().value) + "}";
    }

    [[nodiscard]] std::string status_json() {
        std::lock_guard lock(peer_mu_);
        return "{\"node\":\"tinykv-server\",\"worker_cycles\":" +
               std::to_string(worker_ ? worker_->cycles() : 0) +
               ",\"sessions\":" + std::to_string(active_sessions()) +
               ",\"regions\":" + std::to_string(meta_.region_count()) +
               ",\"kv_writes\":" + std::to_string(kv_writes_.load()) +
               ",\"kv_reads\":" + std::to_string(kv_reads_.load()) +
               ",\"txn_ops\":" + std::to_string(txn_ops_.load()) +
               ",\"term\":" + std::to_string(peer_ ? peer_->raft().term().value : 0) +
               ",\"commit\":" + std::to_string(peer_ ? peer_->raft().committed().value : 0) +
               "}";
    }

    [[nodiscard]] std::uint64_t sessions_safe_point() {
        // 无活跃会话时取当前 TSO（全部已提交数据只保留最新版本）；有会话则取最小活跃 start_ts
        return sessions_->active() > 0 ? sessions_->min_active_start_ts().value
                                       : tso_.current().value;
    }

    [[nodiscard]] static std::vector<std::string> split_frame(const std::string& body) {
        std::vector<std::string> parts;
        std::istringstream in(body);
        std::string item;
        while (std::getline(in, item, '\t')) {
            parts.push_back(item);
        }
        return parts;
    }

    [[nodiscard]] static std::string http_query_param(const std::string& query,
                                                      const std::string& name) {
        std::size_t begin = 0;
        while (begin <= query.size()) {
            const auto at = query.find('&', begin);
            const auto item = query.substr(begin, at == std::string::npos ? at : at - begin);
            const auto eq = item.find('=');
            if (eq != std::string::npos && item.substr(0, eq) == name) {
                return item.substr(eq + 1);
            }
            if (at == std::string::npos) {
                break;
            }
            begin = at + 1;
        }
        return {};
    }

    [[nodiscard]] static std::string escape(const std::string& s) {
        std::string out;
        for (const char ch : s) {
            if (ch == '"' || ch == '\\') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        return out;
    }

    [[nodiscard]] static std::string region_json(const raftstore::Region& r) {
        return "{\"id\":" + std::to_string(r.id.value) +
               ",\"start\":\"" + r.start_key + "\",\"end\":\"" + r.end_key +
               "\",\"version\":" + std::to_string(r.epoch.version) +
               ",\"conf_ver\":" + std::to_string(r.epoch.conf_ver) +
               ",\"size_hint\":" + std::to_string(r.size_hint) + "}";

    }

    ServerOptions opt_;
    std::mutex peer_mu_;                                   // 全部 Peer/Storage 访问的串行闸
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<raftstore::Peer> peer_;
    raftstore::StoreMeta meta_;
    tso::MonotonicTsProvider tso_;
    std::unique_ptr<PeerTxnExecutor> exec_;
    std::unique_ptr<TxnSessionManager> sessions_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<StatusServer> status_;
    std::atomic<std::uint64_t> kv_writes_{0};
    std::atomic<std::uint64_t> kv_reads_{0};
    std::atomic<std::uint64_t> txn_ops_{0};
    bool running_{false};

public:
    std::uint64_t now_ms_{0};                        // 注入时钟：TTL/GC 判定由调用方推进（确定性纪律）
};

}  // namespace tinykv::server
