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
#include <charconv>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "server/interceptor.h"
#include "server/peer_registry.h"
#include "server/peer_txn.h"
#include "server/service.h"
#include <cstdint>
#include "server/status_server.h"
#include "server/txn_sessions.h"
#include "server/worker.h"
#include "storage/engine.h"
#include <cstdint>
#include "txn/executor.h"
namespace tinykv::server {

struct ServerOptions {
    std::filesystem::path data_dir;      // 必填
    std::string host{"127.0.0.1"};
    std::uint16_t status_port{0};        // 0 = 随机
    std::uint64_t election_timeout{8};
    std::uint64_t heartbeat{3};
    std::chrono::milliseconds worker_interval{10};
    LogSink log_sink;                    // 可注入请求日志 sink（空 = 不输出）
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
        registry_ = std::make_unique<RegionPeerRegistry>(opt_.data_dir, meta_,
                                                         opt_.election_timeout, opt_.heartbeat);
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
        registry_->campaign_all();

        // 三个回调都按 key 路由到目标 region 的 peer（executor 持闸调用，这里不再加锁）
        exec_ = std::make_unique<PeerTxnExecutor>(
            [this](std::string_view key) -> raftstore::Peer& { return registry_->require(key); },
            [this](std::string_view key) { pump_one(key); },
            [this](std::string_view key, std::uint64_t idx) {
                return registry_->require(key).applied_at_least(idx);
            },
            &peer_mu_, 400,
            [this] { return registry_->all_peers(); });   // GC 广播到每个 region
        sessions_ = std::make_unique<TxnSessionManager>(*exec_, tso_);

        auto started = StatusServer::start(opt_.host, opt_.status_port,
                                           [this] { return status_json(); },
                                           [this](const std::string& q) {
                                               return intercepted(
                                                   "GET", "/kv",
                                                   http_query_param(q, "key"),
                                                   [&] { return kv_read(q); });
                                           },
                                           [this](const std::string& body) {
                                               return intercepted(
                                                   "POST", "/kv", "",
                                                   [&] { return kv_write(body); });
                                           },
                                           [this](const std::string& m, const std::string& p,
                                                  const std::string& q, const std::string& b) {
                                               return intercepted(m, p, "",
                                                                  [&] { return admin(m, p, q, b); });
                                           });
        if (!started.has_value()) {
            return std::unexpected(started.error());
        }
        status_ = std::move(*started);

        worker_ = std::make_unique<Worker>(
            [this] {
                std::lock_guard lock(peer_mu_);   // 后台泵与 HTTP 共闸（类注释）
                registry_->pump_all();
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
        if (auto r = registry_->close_all(); !r.has_value() && first_err.empty()) {
            first_err = "registry: " + std::string(tinykv::describe_kind(r.error()));
        }
        registry_.reset();
        if (!first_err.empty()) {
            return std::unexpected(IoFailed{.path = "server-app:stop", .reason = first_err});
        }
        return {};
    }

    [[nodiscard]] std::uint16_t status_port() const { return status_ ? status_->port() : 0; }
    [[nodiscard]] std::uint16_t bound_port() const { return status_port(); }
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
        const auto key = http_query_param(query, "key");
        if (key.empty()) {
            return {};
        }
        std::lock_guard lock(peer_mu_);
        kv_reads_.fetch_add(1, std::memory_order_relaxed);
        // BUG-0004: raw 键走保留前缀，与 MVCC 的 user_key+8B ts 分属两个命名空间
        auto got = registry_->require(key).get(Cf::Default, encode_raw_key(key));
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
        if (!peer.propose(req).has_value()) {
            return "ERR not-leader";
        }
        for (int round = 0; round < 400; ++round) {
            peer.tick();
            (void)peer.on_ready();
            if (peer.applied_at_least(expect)) {
                peer.drop_reply(1, expect);   // 组合根轮询位点，不消费回执 → 显式回收
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
            const auto safe = parts.size() == 2 ? parse_u64(parts[1]) : std::nullopt;
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
        const auto rid = parse_u64(parts[1]);
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
        for (int round = 0; round < 400; ++round) {
            registry_->pump_all();
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
        if (path == "/trace") {
            return intercept_events();
        }
        if (path == "/metrics") {
            return metrics_->snapshot();
        }
        return {};   // 未知 admin 路径 → 404
    }

    [[nodiscard]] std::string region_detail(const std::string& id_text) {
        const auto id = parse_u64(id_text);
        if (!id.has_value()) {
            return {};   // BUG-0002: 畸形查询参数 → 404，不得抛异常打崩进程
        }
        auto r = meta_.find_region_by_id(RegionId{*id});
        if (!r.has_value()) {
            return {};
        }
        std::string base = region_json(*r);
        base.pop_back();   // 去掉尾 }，拼入 term/commit 后再闭合
        auto* peer = registry_->peer_by_id(r->id);
        const auto term = peer == nullptr ? 0U : peer->raft().term().value;
        const auto commit = peer == nullptr ? 0U : peer->raft().committed().value;
        return base + ",\"term\":" + std::to_string(term) +
               ",\"commit\":" + std::to_string(commit) + "}";
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
               ",\"intercept_calls\":" + std::to_string(metrics_->total_calls()) +
               ",\"peers\":" + std::to_string(registry_ ? registry_->size() : 0) +
               ",\"splits\":" + std::to_string(split_events_.load()) +
               ",\"now_ms\":" + std::to_string(now_ms_.load()) +
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

    // BUG-0002: 请求参数统一用 from_chars 解析——不抛异常、不接受尾随字符/负号/空串。
    [[nodiscard]] static std::optional<std::uint64_t> parse_u64(std::string_view text) {
        std::uint64_t out{0};
        if (text.empty()) {
            return std::nullopt;
        }
        const char* first = text.data();
        const char* last = text.data() + text.size();
        const auto res = std::from_chars(first, last, out, 10);
        if (res.ec != std::errc{} || res.ptr != last) {
            return std::nullopt;
        }
        return out;
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

    static constexpr std::uint64_t kSplitClient = 0x5F5F5F5F;   // split admin 专用 client id
    static constexpr std::uint64_t kSessionIdleTtlMs = 30'000;  // 空闲会话回收线（锁 TTL 的 10 倍）

    ServerOptions opt_;
    std::mutex peer_mu_;                                   // 全部 Peer/Storage 访问的串行闸
    std::unique_ptr<RegionPeerRegistry> registry_;
    raftstore::StoreMeta meta_;
    tso::MonotonicTsProvider tso_;
    std::unique_ptr<PeerTxnExecutor> exec_;
    std::unique_ptr<TxnSessionManager> sessions_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<StatusServer> status_;
    std::atomic<std::uint64_t> kv_writes_{0};
    std::atomic<std::uint64_t> kv_reads_{0};
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
