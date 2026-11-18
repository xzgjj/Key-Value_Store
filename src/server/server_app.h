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

#include "common/encode.h"
#include "raftstore/peer.h"
#include "scheduler/tso.h"

#include <atomic>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "server/interceptor.h"
#include "server/http_helpers.h"
#include "server/peer_registry.h"
#include "server/peer_txn.h"
#include "server/service.h"
#include <cstdint>
#include "server/status_server.h"
#include "server/txn_sessions.h"
#include "server/worker.h"
#include "storage/engine.h"
#include "txn/executor.h"
// asio（Windows SDK）会定义 DELETE 宏，必须放在 peer_txn.h（引用 kvpb::Mutation::DELETE）
// 之后，否则宏展开破坏枚举。status_server.h 同样引入 asio，故本头统一置于最末。
#include "raftstore/tcp_transport.h"
namespace tinykv::server {

struct ServerOptions {
    std::filesystem::path data_dir;      // 必填
    std::string host{"127.0.0.1"};
    std::uint16_t status_port{0};        // 0 = 随机
    std::uint16_t observe_port{0};       // 观测端点独立端口；0 = 随机（独立 io_context）
    std::uint64_t election_timeout{8};
    std::uint64_t heartbeat{3};
    std::chrono::milliseconds worker_interval{10};
    LogSink log_sink;                    // 可注入请求日志 sink（空 = 不输出）

    // ---- Raft 传输层（M7 ⑤：单机 KV → 分布式 store 的分水岭） ----
    // voters 为空 = 单节点模式（不创建 Transport，保持既有行为）；非空 = 多副本组。
    // replica_id 是本节点在 voters 中的身份；多副本下所有节点须用同一 voters 列表、
    // 不同 replica_id。raft_port=0 由系统分配，start 后经 raft_port() 查询并 set_raft_peers()。
    PeerId replica_id{1};
    std::vector<PeerId> voters{};
    std::uint16_t raft_port{0};
    std::map<std::uint64_t, std::uint16_t> peer_ports{};   // 可选：启动即已知的对端端口
};

class ServerApp final : public Service {
public:
    explicit ServerApp(ServerOptions opt) : opt_(std::move(opt)) {
        interceptors_.add(std::make_unique<LogInterceptor>(opt_.log_sink));
        metrics_ = new MetricsInterceptor();
        interceptors_.add(std::unique_ptr<Interceptor>(metrics_));
        trace_ = new TraceInterceptor();
        interceptors_.add(std::unique_ptr<Interceptor>(trace_));
    }

    [[nodiscard]] std::string_view name() const override { return "tinykv-server"; }

    // ---- Service：装配即 start，逆序即 stop ----
    [[nodiscard]] result<void> start() override {
        if (running_) {
            return {};
        }
        raftstore::Region region{.id = RegionId{1}, .start_key = "", .end_key = "",
                                 .peers = {}, .epoch = {1, 0}, .size_hint = 0};
        if (!meta_.add_region(region).has_value()) {
            return std::unexpected(InvalidArgument{"region 1 注册失败"});
        }
        if (auto r = start_transport_locked(); !r.has_value()) {
            return r;
        }
        registry_ = std::make_unique<RegionPeerRegistry>(
            opt_.data_dir, meta_, opt_.election_timeout, opt_.heartbeat,
            opt_.replica_id, opt_.voters);
        // split 钩子：新 region 动态建 peer + 迁移数据（回调在 apply 路径内，仅动存储）。
        // 经工厂挂钩——bootstrap 与后续每个 split 新 peer 都自动覆盖（多级 split 可用）。
        registry_->set_observer_factory([this](raftstore::Peer& peer) {
            peer.set_split_observer([this](const raftstore::Region& l,
                                           const raftstore::Region& r) {
                auto res = registry_->on_split(l, r);
                if (!res.has_value()) {
                    split_error_ = std::string(tinykv::describe_kind(res.error()));
                }
                ++split_events_;
            });
        });
        if (auto r = registry_->bootstrap(region); !r.has_value()) {
            return std::unexpected(r.error());
        }
        // 单节点组可立即 tick 成 leader（无网络依赖）；多副本必须靠消息交换选主，
        // 这里只把日志基线建好，选举交给后台 Worker 的 pump 驱动。
        if (!multi_voter()) {
            registry_->campaign_all();
        }

        // 三个回调都按 key 路由到目标 region 的 peer（executor 持闸调用，这里不再加锁）
        exec_ = std::make_unique<PeerTxnExecutor>(
            [this](std::string_view key) -> raftstore::Peer& { return registry_->require(key); },
            [this](std::string_view key) { pump_one(key); },
            [this](std::string_view key, std::uint64_t idx) {
                return registry_->require(key).applied_at_least(idx);
            },
            &peer_mu_, 400,
            [this] { return registry_->all_peers(); },   // GC 广播到每个 region
            // ARCH-1 P0-1：注入 key→region 路由，跨区 prewrite 在提案前就被结构化拒绝
            [this](std::string_view key) -> std::optional<RegionId> {
                auto r = meta_.find_region(key);
                return r.has_value() ? std::optional<RegionId>{r->id} : std::nullopt;
            });
        sessions_ = std::make_unique<TxnSessionManager>(*exec_, tso_);

        if (auto r = start_http_locked(); !r.has_value()) {
            return r;
        }

        worker_ = std::make_unique<Worker>(
            [this] {
                std::lock_guard lock(peer_mu_);   // 后台泵与 HTTP 共闸（类注释）
                tick_and_pump_locked();
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
        status_.reset();     // 业务 HTTP 先下线（拒绝新请求），再收引擎
        observe_.reset();    // 观测端点独立 io_context：同样 join 线程，绝不 detach
        sessions_.reset();
        exec_.reset();
        if (auto r = registry_->close_all(); !r.has_value() && first_err.empty()) {
            first_err = "registry: " + std::string(tinykv::describe_kind(r.error()));
        }
        registry_.reset();
        // Transport 的 io 线程独立于 Worker：先停 Worker（不再投递），再关闭传输层。
        transport_.reset();
        if (!first_err.empty()) {
            return std::unexpected(IoFailed{.path = "server-app:stop", .reason = first_err});
        }
        return {};
    }

    [[nodiscard]] std::uint16_t status_port() const { return status_ ? status_->port() : 0; }
    [[nodiscard]] std::uint16_t bound_port() const { return status_port(); }
    [[nodiscard]] std::uint16_t observe_port() const { return observe_ ? observe_->port() : 0; }
    // Raft 传输层（多副本）：系统分配端口时在 start 后查询，再经 set_raft_peers 互告。
    [[nodiscard]] std::uint16_t raft_port() const { return transport_ ? transport_->port() : 0; }
    void set_raft_peers(const std::map<std::uint64_t, std::uint16_t>& peer_ports) {
        if (transport_ != nullptr) {
            transport_->set_peers(peer_ports);
        }
    }
    // 单 region 多 voter：首个 peer 是否为 leader（供测试/就绪探测；生产路由仍走 ReadIndex）
    [[nodiscard]] bool is_leader() const {
        if (registry_ == nullptr) {
            return false;
        }
        auto peers = registry_->all_peers();
        return !peers.empty() && peers.front()->is_leader();
    }
    [[nodiscard]] std::size_t active_sessions() const { return sessions_ ? sessions_->active() : 0; }
    void advance_clock(std::uint64_t ms) {
        now_ms_.fetch_add(ms, std::memory_order_relaxed);
    }   // 确定性时钟推进（测试/主循环），atomic 供 HTTP 线程安全读取
    // 主循环每秒调用：回收空闲会话，避免 GC 安全点被僵尸会话钉死
    void sweep_sessions() {
        if (sessions_ != nullptr) {
            (void)sessions_->sweep(now_ms_.load(), kSessionIdleTtlMs);
        }
    }
    [[nodiscard]] std::size_t pending_replies() const {
        std::size_t total = 0;
        if (registry_ != nullptr) {
            for (auto* peer : registry_->all_peers()) {
                total += peer->reply_count();
            }
        }
        return total;
    }

private:
    [[nodiscard]] bool multi_voter() const { return opt_.voters.size() > 1; }

    // 多副本模式起 TcpTransport（独立 io_context 线程）；单节点不创建，保持零 socket 开销。
    [[nodiscard]] result<void> start_transport_locked() {
        if (opt_.voters.empty()) {
            return {};
        }
        auto started = raftstore::TcpTransport::start(
            raftstore::TcpOptions{.self = opt_.replica_id, .listen_host = opt_.host,
                                  .listen_port = opt_.raft_port});
        if (!started.has_value()) {
            return std::unexpected(started.error());
        }
        transport_ = std::move(*started);
        if (!opt_.peer_ports.empty()) {
            transport_->set_peers(opt_.peer_ports);
        }
        return {};
    }

    // 业务 HTTP 端点 + 独立观测端点。观测端点 try_lock 快照，业务泵持闸时返回上一份。
    [[nodiscard]] result<void> start_http_locked() {
        auto started = StatusServer::start(
            opt_.host, opt_.status_port, [this] { return status_json(); },
            [this](const std::string& q) {
                return intercepted("GET", "/kv", http::http_query_param(q, "key"),
                                   [&] { return kv_read(q); });
            },
            [this](const std::string& body) {
                return intercepted("POST", "/kv", "", [&] { return kv_write(body); });
            },
            [this](const std::string& m, const std::string& p, const std::string& q,
                   const std::string& b) {
                return intercepted(m, p, "", [&] { return admin(m, p, q, b); });
            });
        if (!started.has_value()) {
            return std::unexpected(started.error());
        }
        status_ = std::move(*started);

        {
            std::lock_guard cache_lock(observe_mu_);
            observe_cache_ = status_json();
        }
        auto observed = StatusServer::start(
            opt_.host, opt_.observe_port, [this] { return status_snapshot(); }, {}, {},
            [this](const std::string& m, const std::string& p, const std::string& q,
                   const std::string& b) { return observe_admin(m, p, q, b); });
        if (!observed.has_value()) {
            status_.reset();
            return std::unexpected(observed.error());
        }
        observe_ = std::move(*observed);
        return {};
    }

    // ---- M7 ⑤：统一泵——收件 → 步进 → apply → 发件。调用方必须持 peer_mu_ ----
    // tick 由 Worker 单独驱动；HTTP 路径只 pump 不 tick，避免高频时钟打乱选举节奏。
    // 顺序很关键：先 step 收件（让 follower 及时跟上 leader），再 on_ready 推进共识与
    // apply，最后 take_messages 发件；take_messages 绝不单独调用而不 on_ready（会清掉队列）。
    void tick_and_pump_locked() {
        for (auto* peer : registry_->all_peers()) {
            peer->tick();
        }
        pump_locked();
    }

    void pump_locked() {
        if (transport_ != nullptr) {
            for (auto& msg : transport_->poll()) {
                auto* target = registry_->peer_by_peer_id(PeerId{msg.to()});
                if (target != nullptr) {
                    (void)target->step_message(msg);
                }
            }
        }
        for (auto* peer : registry_->all_peers()) {
            (void)peer->on_ready();
        }
        if (transport_ != nullptr) {
            for (auto* peer : registry_->all_peers()) {
                for (auto& out : peer->take_messages()) {
                    // 发送失败是常态（对端未起/连接被拒），Raft 心跳会重传——不记错误
                    (void)transport_->send(PeerId{out.to()}, out);
                }
            }
        }
    }

    // 等待位点推进：单节点无网络，紧泵 400 轮即可；多副本必须让出 CPU 等 TCP 往返，
    // 否则 400 轮空转在 1ms 内跑完，对端的心跳响应还没回来就误判超时。
    [[nodiscard]] bool wait_applied_locked(raftstore::Peer& peer, std::uint64_t idx) {
        if (!multi_voter()) {
            for (int i = 0; i < 400; ++i) {
                pump_locked();
                if (peer.applied_at_least(idx)) {
                    return true;
                }
            }
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pump_locked();
            if (peer.applied_at_least(idx)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    // 请求切面：统一 begin → 业务 → finish（成功判据：非空且非 ERR / 空 404 视为未命中）
    template <typename Fn>
    [[nodiscard]] std::string intercepted(const std::string& method, const std::string& path,
                                          const std::string& key, Fn&& fn) {
        auto ev = interceptors_.begin(method, path, key);
        auto out = fn();
        const bool ok = !out.empty() && out.rfind("ERR", 0) != 0;
        interceptors_.finish(ev, ok, ok ? std::string{"OK"} : out.substr(0, 48));
        return out;
    }

    // 泵指定 key 所属 peer（executor 在闸内调用；新 peer 首次泵负责把 split observer 建的
    // 实例推成 leader 并 apply 初始条目）
    void pump_one(std::string_view key) {
        if (multi_voter()) {
            pump_locked();   // 多副本：选举/复制靠消息交换，高频 tick 会打乱节奏
            return;
        }
        auto* peer = registry_->peer_for(key);
        if (peer == nullptr) {
            return;
        }
        for (int i = 0; i < 20 && !peer->is_leader(); ++i) {
            peer->tick();
        }
        peer->tick();
        (void)peer->on_ready();
    }

    // ---- kv 面端点（经 raft 提案，"写走共识"红线不豁免） ----
    [[nodiscard]] std::string kv_read(const std::string& query) {
        const auto key = http::http_query_param(query, "key");
        if (key.empty()) {
            return {};
        }
        std::lock_guard lock(peer_mu_);
        kv_reads_.fetch_add(1, std::memory_order_relaxed);
        auto& peer = registry_->require(key);
        // 线性一致读：ReadIndex 确认 + applied 追平后才读状态机（不再直接 leader 本地读）
        // 多副本下 pump 必须让出 CPU，否则心跳确认的 TCP 回包没机会到达 io_context 线程。
        const auto ri = peer.wait_read_index(
            [this] {
                pump_locked();
                if (multi_voter()) {
                    std::this_thread::yield();
                }
            },
            multi_voter() ? 200000 : 400,
            multi_voter() ? std::optional<std::chrono::milliseconds>{5000} : std::nullopt);
        if (!ri.has_value()) {
            std::string why{tinykv::describe_kind(ri.error())};
            if (const auto* io = std::get_if<IoFailed>(&ri.error())) {
                why += ":" + io->reason;
            }
            return "{\"error\":\"" + why + "\"}";
        }
        read_index_reads_.fetch_add(1, std::memory_order_relaxed);
        // BUG-0004: raw 键走保留前缀，与 MVCC 的 user_key+8B ts 分属两个命名空间
        auto got = peer.read_directly(Cf::Default, encode_raw_key(key));
        if (!got.has_value()) {
            return "{\"error\":\"" + std::string(tinykv::describe_kind(got.error())) + "\"}";
        }
        if (!got->value.has_value()) {
            return "{\"found\":false}";
        }
        return "{\"found\":true,\"value\":\"" + http::escape(*got->value) + "\"}";
    }

    [[nodiscard]] std::string kv_write(const std::string& body) {
        const auto parts = http::split_frame(body);
        if (parts.size() < 2 || parts[1].empty()) {
            return "ERR bad-request";
        }
        std::lock_guard lock(peer_mu_);
        kv_writes_.fetch_add(1, std::memory_order_relaxed);
        auto& peer = registry_->require(parts[1]);
        const auto expect = peer.raft().last_index().value + 1;
        cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(1);
        req.mutable_header()->set_sequence_id(expect);
        if (parts[0] == "put" && parts.size() == 3) {
            auto* one = req.add_requests();
            one->mutable_put()->set_key(encode_raw_key(parts[1]));
            one->mutable_put()->set_value(parts[2]);
        } else if (parts[0] == "del" && parts.size() == 2) {
            auto* one = req.add_requests();
            one->mutable_delete_()->set_key(encode_raw_key(parts[1]));
        } else {
            return "ERR bad-request";
        }
        auto accepted = peer.propose(req);
        if (!accepted.has_value()) {
            return "ERR not-leader";
        }
        if (!*accepted) {
            // ARCH-1 P0-3：去重命中 = 该 (client,seq) 早已 apply 过，语义上已成功。
            // 这里不能再等 applied 追平 last_index+1——那个位点永远到不了。
            peer.drop_reply(1, expect);   // 回收 propose 登记的合成回执
            return "OK";
        }
        if (!wait_applied_locked(peer, expect)) {
            return "ERR apply-timeout";
        }
        peer.drop_reply(1, expect);   // 组合根轮询位点，不消费回执 → 显式回收
        return "OK";
    }

    // ---- 事务/观测端点（docs/cli.md §11；会话作用域） ----
    [[nodiscard]] std::string admin(const std::string& method, const std::string& path,
                                    const std::string& query, const std::string& body) {
        txn_ops_.fetch_add(1, std::memory_order_relaxed);
        return method == "POST" ? admin_post(path, body) : admin_get(path, query);
    }

    // 观测端口的处理器白名单：只暴露读型观测路径，事务写/GC/split 一律 404。
    [[nodiscard]] std::string observe_admin(const std::string& method,
                                            const std::string& path,
                                            const std::string& query,
                                            const std::string& body) {
        if (method != "GET") {
            return {};
        }
        if (path != "/regions" && path != "/region" && path != "/metrics" && path != "/trace") {
            return {};
        }
        return admin(method, path, query, body);
    }

    // POST 族：会话写操作 + GC（executor 自带闸，此处不加 peer_mu_）
    [[nodiscard]] std::string admin_post(const std::string& path, const std::string& body) {
        const auto parts = http::split_frame(body);
        if (path == "/txn/begin") {
            auto sid = sessions_->begin(now_ms_.load());
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
            const auto safe = parts.size() == 2 ? http::parse_u64(parts[1]) : std::nullopt;
            if (parts.size() > 1 && !safe.has_value()) {
                return "ERR bad-request";
            }
            const Ts point{safe.value_or(sessions_safe_point())};
            auto r = exec_->gc(point.value);
            return r.has_value() ? "OK gc"   // 计数在 apply 侧日志
                                 : "ERR " + std::string(tinykv::describe_kind(r.error()));
        }
        if (path == "/region/split") {
            return admin_split(parts);
        }
        return {};
    }

    // region split（M4 功能在 M6 多 region 下重新接通）：propose → apply → observer 动态建 peer
    [[nodiscard]] std::string admin_split(const std::vector<std::string>& parts) {
        if (parts.size() != 3) {
            return "ERR bad-request";
        }
        std::lock_guard lock(peer_mu_);
        const auto rid = http::parse_u64(parts[1]);
        if (!rid.has_value()) {
            return "ERR bad-request";   // BUG-0002: 非数字 region id 不得击穿解析路径
        }
        const auto region_id = RegionId{*rid};
        auto* peer = registry_->peer_by_id(region_id);
        if (peer == nullptr) {
            return "ERR no-region";
        }
        const auto expect = peer->raft().last_index().value + 1;
        cmdpb::RaftCmdRequest req;
        req.mutable_header()->set_client_id(kSplitClient);
        req.mutable_header()->set_sequence_id(expect);
        auto* admin = req.mutable_admin();
        admin->mutable_split()->set_split_key(parts[2]);
        admin->mutable_split()->set_new_region_id(next_region_id_.load() + 1);
        if (!peer->propose(req).has_value()) {
            return "ERR not-leader";
        }
        // 多 voter 下 RegionPeerRegistry 无法保证新 region 的 PeerId 全局唯一
        // （raftpb::Message 只带 to=PeerId，无 region_id），此时 split 会造出不可路由的
        // peer——明确拒绝，单节点模式不受影响。
        if (multi_voter()) {
            return "ERR split-unsupported-in-multi-voter";
        }
        for (int round = 0; round < 400; ++round) {
            pump_locked();
            if (peer->applied_at_least(expect)) {
                peer->drop_reply(kSplitClient, expect);   // 同上，admin 回执也不消费
                next_region_id_.fetch_add(1);
                return split_error_.empty() ? "OK split" : "ERR " + split_error_;
            }
        }
        return "ERR apply-timeout";
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
            auto r = sessions_->get_verbose(http::http_query_param(query, "session"),
                                            http::http_query_param(query, "key"), now_ms_);
            if (!r.has_value()) {
                return {};
            }
            return "{\"value\":\"" + http::escape(*r) + "\"}";
        }
        if (path == "/txn/status") {
            const auto sid = http::http_query_param(query, "session");
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
                out += (out.empty() ? "[" : ",") + http::region_json(r);
            }
            return out + "]";
        }
        if (path == "/region") {
            return region_detail(http::http_query_param(query, "id"));
        }
        if (path == "/trace") {
            return intercept_events();
        }
        if (path == "/metrics") {
            return metrics_->snapshot();
        }
        return {};   // 未知 admin 路径 → 404
    }

    [[nodiscard]] std::string region_detail(const std::string& id_text) {
        const auto id = http::parse_u64(id_text);
        if (!id.has_value()) {
            return {};   // BUG-0002: 畸形查询参数 → 404，不得抛异常打崩进程
        }
        auto r = meta_.find_region_by_id(RegionId{*id});
        if (!r.has_value()) {
            return {};
        }
        std::string base = http::region_json(*r);
        base.pop_back();   // 去掉尾 }，拼入 term/commit 后再闭合
        auto* peer = registry_->peer_by_id(r->id);
        const auto term = peer == nullptr ? 0U : peer->raft().term().value;
        const auto commit = peer == nullptr ? 0U : peer->raft().committed().value;
        return base + ",\"term\":" + std::to_string(term) +
               ",\"commit\":" + std::to_string(commit) + "}";
    }

    // 同步快照：业务线程可等闸；观测端点用 status_snapshot() 永不等待业务闸
    [[nodiscard]] std::string status_json() {
        std::lock_guard lock(peer_mu_);
        return status_json_locked();
    }

    // 观测端点专用：try_lock 成功则刷新缓存，失败则回上一份快照——绝不因业务泵卡住观测
    [[nodiscard]] std::string status_snapshot() {
        if (std::unique_lock lock(peer_mu_, std::try_to_lock); lock.owns_lock()) {
            auto json = status_json_locked();
            std::lock_guard cache_lock(observe_mu_);
            observe_cache_ = std::move(json);
        }
        std::lock_guard cache_lock(observe_mu_);
        return observe_cache_.empty() ? std::string{"{\"node\":\"tinykv-server\",\"ready\":false}"}
                                      : observe_cache_;
    }

    [[nodiscard]] std::string status_json_locked() const {
        std::uint64_t apply_errors = 0;
        std::size_t txn_errors = 0;
        std::string last_error;
        std::uint64_t applied = 0;
        if (registry_ != nullptr) {
            for (auto* peer : registry_->all_peers()) {
                apply_errors += peer->apply_error_count();
                txn_errors += peer->pending_txn_errors();
                applied = std::max(applied, peer->applied_index());
                if (!peer->last_apply_error().empty()) {
                    last_error = peer->last_apply_error();
                }
            }
        }
        return "{\"node\":\"tinykv-server\",\"worker_cycles\":" +
               std::to_string(worker_ ? worker_->cycles() : 0) +
               ",\"sessions\":" + std::to_string(active_sessions()) +
               ",\"regions\":" + std::to_string(meta_.region_count()) +
               ",\"kv_writes\":" + std::to_string(kv_writes_.load()) +
               ",\"kv_reads\":" + std::to_string(kv_reads_.load()) +
               ",\"read_index_reads\":" + std::to_string(read_index_reads_.load()) +
               ",\"txn_ops\":" + std::to_string(txn_ops_.load()) +
               ",\"intercept_calls\":" + std::to_string(metrics_->total_calls()) +
               ",\"peers\":" + std::to_string(registry_ ? registry_->size() : 0) +
               ",\"splits\":" + std::to_string(split_events_.load()) +
               ",\"apply_errors\":" + std::to_string(apply_errors) +
               ",\"txn_errors\":" + std::to_string(txn_errors) +
               ",\"last_apply_error\":\"" + http::escape(last_error) + "\"" +
               ",\"now_ms\":" + std::to_string(now_ms_.load()) +
               ",\"replica_id\":" + std::to_string(opt_.replica_id.value) +
               ",\"voters\":" + std::to_string(opt_.voters.size()) +
               ",\"leader\":" + (is_leader() ? "true" : "false") +
               ",\"applied\":" + std::to_string(applied) +
               ",\"term\":" + std::to_string(first_term()) +
               ",\"commit\":" + std::to_string(first_commit()) +
               "}";
    }

    [[nodiscard]] std::string intercept_events() const { return trace_->snapshot(); }

    [[nodiscard]] std::uint64_t first_term() const {
        if (registry_ == nullptr) {
            return 0;
        }
        const auto peers = registry_->all_peers();
        return peers.empty() ? 0 : peers.front()->raft().term().value;
    }

    [[nodiscard]] std::uint64_t first_commit() const {
        if (registry_ == nullptr) {
            return 0;
        }
        const auto peers = registry_->all_peers();
        return peers.empty() ? 0 : peers.front()->raft().committed().value;
    }

    [[nodiscard]] std::uint64_t sessions_safe_point() {
        // 无活跃会话时取当前 TSO（全部已提交数据只保留最新版本）；有会话则取最小活跃 start_ts
        return sessions_->active() > 0 ? sessions_->min_active_start_ts().value
                                       : tso_.current().value;
    }

    static constexpr std::uint64_t kSplitClient = 0x5F5F5F5F;   // split admin 专用 client id
    static constexpr std::uint64_t kSessionIdleTtlMs = 30'000;  // 空闲会话回收线（锁 TTL 的 10 倍）

    ServerOptions opt_;
    std::mutex peer_mu_;                                   // 全部 Peer/Storage 访问的串行闸
    std::unique_ptr<RegionPeerRegistry> registry_;
    std::unique_ptr<raftstore::TcpTransport> transport_;   // 多副本传输层（单节点为 nullptr）
    raftstore::StoreMeta meta_;
    tso::MonotonicTsProvider tso_;
    std::unique_ptr<PeerTxnExecutor> exec_;
    std::unique_ptr<TxnSessionManager> sessions_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<StatusServer> status_;
    std::unique_ptr<StatusServer> observe_;
    mutable std::mutex observe_mu_;
    std::string observe_cache_;
    std::atomic<std::uint64_t> kv_writes_{0};
    std::atomic<std::uint64_t> kv_reads_{0};
    std::atomic<std::uint64_t> read_index_reads_{0};
    std::atomic<std::uint64_t> txn_ops_{0};
    std::atomic<std::uint64_t> split_events_{0};
    std::atomic<std::uint64_t> next_region_id_{1};   // 下一个 split 新 region id（CAS 前置）
    std::string split_error_;                        // observer 里迁移失败的回执（split 线程写）
    InterceptorChain interceptors_;
    MetricsInterceptor* metrics_{nullptr};   // 拥有权在 interceptors_ 链中
    TraceInterceptor* trace_{nullptr};        // 拥有权在 interceptors_ 链中
    bool running_{false};

public:
    std::atomic<std::uint64_t> now_ms_{0};            // 注入时钟：主循环每秒推进，HTTP 线程只读
};

}  // namespace tinykv::server
