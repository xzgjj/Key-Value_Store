// 文件用途：ServerApp——组合根（M6 第③层）：唯一负责"创建并连接全部组件"的单元。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 装配顺序（= 注册顺序，LifecycleManager 保证逆序关闭）：
//   Storage(Engine) → Raft(Peer 单节点组) → Worker(泵循环) → StatusServer(HTTP)
// 并发模型（⑨a 分片接线后）：Peer 非线程安全 → 访问经 PeerGate 串行——
//   * 单 region 操作（kv 读写、事务提案/读）只锁该 region 的桶（同区串行、跨区并行）；
//   * 跨 region 操作（Worker 泵、split、status、GC/锁决议广播）锁全部桶；
//   * 多副本或 lock_shards=1 时退化为一把大锁 peer_mu_（闸语义与改造前逐字节一致）。
//   * registry 的 slots_ 表自带 mu_（split 动态建 slot 与路由读并发）；StoreMeta 自带
//     RWMutex；锁序固定：region 桶 → registry mu_ / Engine mu_ / latch / session mu_。
// 服务端事务会话：TxnSessionManager（会话作用域），执行端 = PeerTxnExecutor（经 raft）。
//
// Service 自身可嵌套：ServerApp 实现 Service，可被更外层（多 store 拓扑）再组装。

#pragma once

#include "common/encode.h"
#include "raftstore/peer.h"
#include "scheduler/tso.h"

#include <algorithm>
#include <atomic>
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

#include <cstdint>
#include "raftstore/meta_store.h"
#include "server/http_helpers.h"
#include "server/interceptor.h"
#include "server/lock_registry.h"
#include "server/peer_registry.h"
#include "server/peer_txn.h"
#include "server/service.h"
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
    std::filesystem::path data_dir;  // 必填
    std::string host{"127.0.0.1"};
    std::uint16_t status_port{0};   // 0 = 随机
    std::uint16_t observe_port{0};  // 观测端点独立端口；0 = 随机（独立 io_context）
    std::uint64_t election_timeout{8};
    std::uint64_t heartbeat{3};
    std::chrono::milliseconds worker_interval{10};
    LogSink log_sink;  // 可注入请求日志 sink（空 = 不输出）
    // REL-02 开关：生产装配走配置（默认 true）。false 只允许出现在基准对比里——
    // 掉电会丢已向客户端确认的写，也会丢已提交的 Raft 日志（破坏 State Machine Safety）。
    bool fsync_on_commit{true};
    // ⑩ 安全边界（M7-SEC-01/02）：非空 = 所有 HTTP 请求必须带 Authorization: Bearer <token>；
    // 为空 = 不鉴权，此时强制绑回环（D6-3：默认关 + 启动告警 + 强制回环）。
    std::string auth_token;
    // ⑦ 跨区事务开关（SPEC-6 §6.6 `txn.cross_region_enabled`）：
    // 开（默认，M7 转正：150 轮随机 kill chaos 通过后由用户批准）→ 会话里的 TxnClient
    // 按 region 分组走两阶段提交；关 → 跨区 prewrite 被护栏拒绝。状态经 /status 暴露（红线 6）。
    bool cross_region_enabled{true};
    // ⑨a 锁分片（M7-PERF-01，SPEC-6 §6.6 `server.lock_shards`）：>1 时单 region 操作只锁
    // 所属桶（同区串行、跨区并行），跨区操作锁全部桶；1 = 一把大锁（改造前行为）。
    // 多副本模式强制退化为 1：泵路径触碰全部 peer + transport，分片收益为零。
    std::size_t lock_shards{16};
    // ⑪ HTTP 并发（M8-PERF-01，SPEC-7 §7.6 `server.http_threads`）：业务端口 io_context
    // 线程数。默认 2 是行为变更（并发度 1→2，用户 2026-11-18 已批）；显式 1 = 回 M7 行为；
    // 0 = hardware_concurrency（bench 档）。handler 并发安全以 ⑪-0 审计清单为前提
    // （docs/落实/notes-m8.md §1，红线 1：未审计的 handler 不许进线程池）。
    std::size_t http_threads{2};
    // ⑭ TLS 传输（M8-SEC-01，SPEC-7 §7.6 `server.tls_cert/tls_key`）：均空 = 明文
    // （M7 行为逐字节，红线 3）；非空 = 业务端口 TLS（两者必须同给，只给一个 = 启动报错，
    // 错误矩阵见 SPEC-7 §7.3.3）。观测端口维持明文内网面，不随此项开启。
    // 组合语义（D7-3）：配 TLS 后 token 可选（传输已加密，未配 token 不再强制回环）；
    // 未配 TLS 保持强制回环守卫不动。
    std::string tls_cert{};
    std::string tls_key{};

    // ---- Raft 传输层（M7 ⑤：单机 KV → 分布式 store 的分水岭） ----
    // voters 为空 = 单节点模式（不创建 Transport，保持既有行为）；非空 = 多副本组。
    // replica_id 是本节点在 voters 中的身份；多副本下所有节点须用同一 voters 列表、
    // 不同 replica_id。raft_port=0 由系统分配，start 后经 raft_port() 查询并 set_raft_peers()。
    PeerId replica_id{1};
    std::vector<PeerId> voters{};
    std::uint16_t raft_port{0};
    std::map<std::uint64_t, std::uint16_t> peer_ports{};  // 可选：启动即已知的对端端口
};

class ServerApp final : public Service {
public:
    explicit ServerApp(ServerOptions opt) : opt_(std::move(opt)) {
        interceptors_.add(std::make_unique<LogInterceptor>(opt_.log_sink));
        // 先建再取裸指针交给链持有：add() 抛异常时由 unique_ptr 回收，不留泄漏（CLAUDE.md §4-4）
        auto metrics = std::make_unique<MetricsInterceptor>();
        metrics_ = metrics.get();
        interceptors_.add(std::move(metrics));
        auto trace = std::make_unique<TraceInterceptor>();
        trace_ = trace.get();
        interceptors_.add(std::move(trace));
    }

    [[nodiscard]] std::string_view name() const override { return "tinykv-server"; }

    // ---- Service：装配即 start，逆序即 stop ----
    [[nodiscard]] result<void> start() override {
        if (running_) {
            return {};
        }
        // ⑨a：分片只在"单节点 + shards>1"生效。多副本的泵/读路径触碰全部 peer 与
        // transport，分片不带来并行度，保守退回一把大锁（闸语义等价改造前）。
        sharded_ = opt_.lock_shards > 1 && !multi_voter();
        if (sharded_) {
            locks_ = std::make_unique<LockRegistry>(opt_.lock_shards);
        }
        if (auto r = mount_storage_locked(); !r.has_value()) {
            return r;
        }
        if (auto r = mount_txn_locked(); !r.has_value()) {
            return r;
        }
        if (auto r = start_http_locked(); !r.has_value()) {
            return r;
        }
        worker_ = std::make_unique<Worker>(
            [this] {
                auto gate = gate_for(true, {});  // 全局闸：泵触碰全部 peer
                tick_and_pump_locked();
            },
            opt_.worker_interval);
        if (auto r = worker_->start(); !r.has_value()) {
            return tinykv::unexpected(r.error());
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
        status_.reset();   // 业务 HTTP 先下线（拒绝新请求），再收引擎
        observe_.reset();  // 观测端点独立 io_context：同样 join 线程，绝不 detach
        sessions_.reset();
        exec_.reset();
        if (auto r = registry_->close_all(); !r.has_value() && first_err.empty()) {
            first_err = "registry: " + std::string(tinykv::describe_kind(r.error()));
        }
        registry_.reset();
        // Transport 的 io 线程独立于 Worker：先停 Worker（不再投递），再关闭传输层。
        transport_.reset();
        if (!first_err.empty()) {
            return tinykv::unexpected(IoFailed{.path = "server-app:stop", .reason = first_err});
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
    [[nodiscard]] std::size_t active_sessions() const {
        return sessions_ ? sessions_->active() : 0;
    }
    void advance_clock(std::uint64_t ms) {
        now_ms_.fetch_add(ms, std::memory_order_relaxed);
    }  // 确定性时钟推进（测试/主循环），atomic 供 HTTP 线程安全读取
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

    // ---- ⑨a 闸：分区闸（单 region）与全局闸（跨 region）的唯一入口 ----
    // 分片关/多副本：两种闸都退化为 peer_mu_（单把，句柄里只有一把锁）。
    // 分片开：分区闸=region 所属桶；全局闸=全部桶（lock_all 升序）。
    // key 无归属（理论不可达，REL-05）→ 保守取全局闸，绝不"猜一个桶"。
    [[nodiscard]] PeerGateHandle gate_for(bool global, std::string_view key);

    // 观测端点专用：try 全部闸，拿不到就回缓存（绝不因业务泵卡住观测读）
    [[nodiscard]] std::optional<PeerGateHandle> try_gate_global();

    // key → region（StoreMeta 自带 RWMutex，免额外闸）
    [[nodiscard]] std::optional<RegionId> region_of_key(std::string_view key);

    // 回环判定：未鉴权时唯一允许的监听面（含 IPv6 回环与 localhost 别名）
    [[nodiscard]] static bool is_loopback(std::string_view host);

    // 存储面装配：region 元数据 → 传输层 → region/peer 注册表 → bootstrap → 选举。
    // 抽出来是因为 start() 单函数别超过 80 行（CI 静态门禁），且这三段的失败回滚语义不同。
    [[nodiscard]] result<void> mount_storage_locked() {
        // ⑧ 元数据持久化（SPEC-6 / M7-REL-02）：先读盘上的路由真相，再决定建哪些 peer。
        // 没有 meta = 首次启动 → 建 region 1 并落盘；有 meta → 按它恢复（split 过的拓扑不再回退）。
        meta_store_ = std::make_unique<raftstore::StoreMetaStore>(opt_.data_dir, meta_);
        auto has_meta = meta_store_->load();
        if (!has_meta.has_value()) {
            return tinykv::unexpected(has_meta.error());
        }
        if (!*has_meta) {
            raftstore::Region region{.id = RegionId{1},
                                     .start_key = "",
                                     .end_key = "",
                                     .peers = {},
                                     .epoch = {1, 0},
                                     .size_hint = 0};
            if (!meta_.add_region(region).has_value()) {
                return tinykv::unexpected(InvalidArgument{"region 1 注册失败"});
            }
            if (auto r = meta_store_->persist(); !r.has_value()) {
                return r;
            }
        }
        if (auto r = start_transport_locked(); !r.has_value()) {
            return r;
        }
        registry_ = std::make_unique<RegionPeerRegistry>(
            opt_.data_dir, meta_, opt_.election_timeout, opt_.heartbeat, opt_.replica_id,
            opt_.voters, opt_.fsync_on_commit);
        // split 钩子：新 region 动态建 peer + 迁移数据（回调在 apply 路径内，仅动存储）。
        // 经工厂挂钩——bootstrap 与后续每个 split 新 peer 都自动覆盖（多级 split 可用）。
        registry_->set_observer_factory([this](raftstore::Peer& peer) {
            peer.set_split_observer([this](const raftstore::Region& l, const raftstore::Region& r) {
                auto res = registry_->on_split(l, r);
                if (!res.has_value()) {
                    const std::lock_guard lock(split_error_mu_);  // ⑪ 审计 #15：apply 可能发生在分区闸泵路径
                    split_error_ = std::string(tinykv::describe_kind(res.error()));
                } else if (meta_store_ != nullptr) {
                    // ⑧：拓扑变了就落盘。失败不阻断 apply（数据已切分），但必须可见——
                    // 否则下次重启会少一个 region，表现为"右半数据没人在管"。
                    if (auto saved = meta_store_->persist(); !saved.has_value()) {
                        const std::lock_guard lock(split_error_mu_);
                        split_error_ = std::string(tinykv::describe_kind(saved.error()));
                    }
                }
                ++split_events_;
            });
        });
        std::uint64_t max_region_id = 0;
        for (const auto& region : meta_.all_regions()) {
            if (auto r = registry_->bootstrap(region); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
            max_region_id = std::max(max_region_id, region.id.value);
        }
        next_region_id_.store(max_region_id);  // 新 region id 接着盘上最大值，避免复用
        // 单节点组可立即 tick 成 leader（无网络依赖）；多副本必须靠消息交换选主，
        // 这里只把日志基线建好，选举交给后台 Worker 的 pump 驱动。
        if (!multi_voter()) {
            registry_->campaign_all();
        }
        return {};
    }

    // 事务面装配：执行器（按 key 路由）+ TSO 恢复基线 + 会话管理器。
    [[nodiscard]] result<void> mount_txn_locked() {
        // 三个回调都按 key 路由到目标 region 的 peer（executor 持闸调用，这里不再加锁）
        exec_ = std::make_unique<PeerTxnExecutor>(
            [this](std::string_view key) -> raftstore::Peer& { return registry_->require(key); },
            [this](std::string_view key) { pump_one(key); },
            [this](std::string_view key, std::uint64_t idx) {
                return registry_->require(key).applied_at_least(idx);
            },
            // ⑨a：闸提供者——执行器按操作类型取分区闸/全局闸（单区串行、跨区并行）
            [this](bool global, std::string_view key) { return gate_for(global, key); }, 400,
            [this] { return registry_->all_peers(); },  // GC 广播到每个 region
            // ARCH-1 P0-1：注入 key→region 路由，跨区 prewrite 在提案前就被结构化拒绝
            [this](std::string_view key) -> std::optional<RegionId> {
                return region_of_key(key);
            });
        // TSO 恢复（code-review P0-2）：先把下限抬到盘上已用的最大 ts，再开放分配。
        // 不做这一步，重启后 start_ts 从 1 重新计数 → 历史版本全读不到、写全冲突。
        if (auto baseline = recover_ts_floor(); !baseline.has_value()) {
            return tinykv::unexpected(baseline.error());
        }
        sessions_ = std::make_unique<TxnSessionManager>(*exec_, checked_tso_);
        if (opt_.cross_region_enabled) {
            // 协调在 client 侧（SPEC-6 §6.2.2）：TxnClient 拿到路由就能自己分组，
            // 不需要新增一个并行类——两套编排只会分叉。
            sessions_->set_region_router([this](std::string_view key) {
                return region_of_key(key);
            });
        }
        return {};
    }

    // 启动期抬高 TSO 下限：扫每个 region 的 Write/Lock 列族取最大 ts。
    // 这是"数据真相优先于计时器"的一次性对账，O(region × 数据量)，只在装配期做一次。
    [[nodiscard]] result<void> recover_ts_floor() {
        Ts floor{0};
        for (auto* peer : registry_->all_peers()) {
            auto baseline = peer->mvcc().max_ts_in_use();
            if (!baseline.has_value()) {
                return tinykv::unexpected(baseline.error());
            }
            if (baseline->value > floor.value) {
                floor = *baseline;
            }
        }
        tso_.raise_floor(floor);
        return {};
    }

    // 多副本模式起 TcpTransport（独立 io_context 线程）；单节点不创建，保持零 socket 开销。
    [[nodiscard]] result<void> start_transport_locked() {
        if (opt_.voters.empty()) {
            return {};
        }
        auto started = raftstore::TcpTransport::start(raftstore::TcpOptions{
            .self = opt_.replica_id, .listen_host = opt_.host, .listen_port = opt_.raft_port});
        if (!started.has_value()) {
            return tinykv::unexpected(started.error());
        }
        transport_ = std::move(*started);
        if (!opt_.peer_ports.empty()) {
            transport_->set_peers(opt_.peer_ports);
        }
        return {};
    }

    // 业务 HTTP 端点 + 独立观测端点。观测端点 try_lock 快照，业务泵持闸时返回上一份。
    [[nodiscard]] result<void> start_http_locked() {
        // ⑭ 错误矩阵（SPEC-7 §7.3.3）：只给 cert/key 之一 = 半配置，启动即拒绝，不许半配置运行
        const bool tls_partial =
            (opt_.tls_cert.empty() != opt_.tls_key.empty());
        if (tls_partial) {
            return tinykv::unexpected(InvalidArgument{"tls_cert 与 tls_key 必须同给"});
        }
        // ⑩ D6-3 + ⑭ D7-3：未配置 token 时不许对外监听——除非传输已加密（TLS）。
        // 未配 TLS 的回环守卫逐字节不动（红线 3：配了证书也不许放松对"明文"的判定）。
        // 这里直接改写 host 为回环并把事实记进 /status，而不是"只告警"——告警会被忽略，
        // 绑定面不会。
        if (opt_.auth_token.empty() && !is_loopback(opt_.host)) {
            auth_forced_loopback_ = true;
            opt_.host = "127.0.0.1";
        }
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
            },
            opt_.auth_token,
            opt_.http_threads,                          // ⑪：业务端口进线程池（审计前置）
            TlsOptions{opt_.tls_cert, opt_.tls_key});   // ⑭：业务端口 TLS（观测端口维持明文）
        if (!started.has_value()) {
            return tinykv::unexpected(started.error());
        }
        status_ = std::move(*started);

        {
            std::lock_guard cache_lock(observe_mu_);
            observe_cache_ = status_json();
        }
        auto observed = StatusServer::start(
            opt_.host, opt_.observe_port, [this] { return status_snapshot(); }, {}, {},
            [this](const std::string& m, const std::string& p, const std::string& q,
                   const std::string& b) { return observe_admin(m, p, q, b); },
            opt_.auth_token);
        if (!observed.has_value()) {
            status_.reset();
            return tinykv::unexpected(observed.error());
        }
        observe_ = std::move(*observed);
        return {};
    }

    // ---- M7 ⑤：统一泵——收件 → 步进 → apply → 发件。调用方必须持全局闸 ----
    // tick 由 Worker 单独驱动；HTTP 路径只 pump 不 tick，避免高频时钟打乱选举节奏。
    // 顺序很关键：先 step 收件（让 follower 及时跟上 leader），再 on_ready 推进共识与
    // apply，最后 take_messages 发件；take_messages 绝不单独调用而不 on_ready（会清掉队列）。
    void tick_and_pump_locked();

    void pump_locked();

    // 等待位点推进：单节点无网络，紧泵 400 轮即可（⑨a：只泵目标 peer，其他 peer 由
    // Worker 驱动——分区闸下泵全部 peer 会跨桶触碰他 region 的状态）；多副本必须让出
    // CPU 等 TCP 往返，否则 400 轮空转在 1ms 内跑完，对端心跳响应没机会回来就误判超时。
    [[nodiscard]] bool wait_applied_locked(raftstore::Peer& peer, std::uint64_t idx);

    // 请求切面：统一 begin → 业务 → finish（成功判据：非空且非 ERR / 空 404 视为未命中）
    template <typename Fn>
    [[nodiscard]] std::string intercepted(const std::string& method, const std::string& path,
                                          const std::string& key, Fn&& fn);

    // 单 peer 泵（⑨a 分区闸配套）：只推进目标 peer，绝不触碰别的 peer/transport——
    // 否则分区闸下的泵会与另一 region 的 handler 数据竞争。多副本一律走 pump_locked
    // （全局闸），多副本模式永不启用分片（类注释）。
    void pump_peer_only(raftstore::Peer& peer);

    // 泵指定 key 所属 peer（executor 在闸内调用；新 peer 首次泵负责把 split observer 建的
    // 实例推成 leader 并 apply 初始条目）
    void pump_one(std::string_view key);

    // ---- kv 面端点（经 raft 提案，"写走共识"红线不豁免） ----
    [[nodiscard]] std::string kv_read(const std::string& query);

    [[nodiscard]] std::string kv_write(const std::string& body);

    // ---- 事务/观测端点（docs/cli.md §11；会话作用域） ----
    [[nodiscard]] std::string admin(const std::string& method, const std::string& path,
                                    const std::string& query, const std::string& body);

    // 观测端口的处理器白名单：只暴露读型观测路径，事务写/GC/split 一律 404。
    [[nodiscard]] std::string observe_admin(const std::string& method, const std::string& path,
                                            const std::string& query, const std::string& body);

    // POST 族：会话写操作 + GC（executor 自带闸，此处不加全局闸）
    [[nodiscard]] std::string admin_post(const std::string& path, const std::string& body);

    [[nodiscard]] std::string admin_txn_begin();

    // GC：body 形如 `gc` 或 `gc\t<safe_point>`；安全点缺省取当前 TSO / 最小活跃 start_ts
    [[nodiscard]] std::string admin_txn_gc(const std::vector<std::string>& parts);

    // 采样开关：body 形如 `trace\t<region_id>\ton|off`（region 必须存在，否则 ERR no-region）
    [[nodiscard]] std::string admin_trace(const std::vector<std::string>& parts);

    // region split（M4 功能在 M6 多 region 下重新接通）：propose → apply → observer 动态建 peer
    // 全局闸：split 触碰 meta 拓扑 + 新建 peer（跨 region 操作）
    [[nodiscard]] std::string admin_split(const std::vector<std::string>& parts);

    // 会话写缓冲（set/del）：显式分支而非嵌套三元——三元链在这里既难读也会把
    // 函数的圈复杂度推过门禁（每个 ? 都算一个分支）。
    [[nodiscard]] std::string admin_txn_buffered(bool is_set,
                                                 const std::vector<std::string>& parts);

    // 写路径失败回执：只回错误种类（细节在日志；读路径才需要字段级 JSON）
    [[nodiscard]] static std::string write_error(const KvError& error);

    // 会话终结（commit/rollback）
    [[nodiscard]] std::string admin_txn_finish(bool is_commit,
                                               const std::vector<std::string>& parts);

    // GET 族：会话读 + 观测
    [[nodiscard]] std::string admin_get(const std::string& path, const std::string& query);

    [[nodiscard]] std::string region_detail(const std::string& id_text);

    // 副本分布（CLI `region peers <id>` 的落点）。单 store 单 peer/region 是当前事实，
    // 如实报告而不是编造多副本表：store_id 留 0 = 本机未配置多 store。
    [[nodiscard]] std::string region_peers(const std::string& id_text);

    // 调度器视图（CLI `scheduler status` 的落点）。单 store 没有真正需要调度的对象，
    // 因此报告"事实 + 为什么没有调度器"，避免让调用方以为有 PD 在背后工作。
    [[nodiscard]] std::string scheduler_status();

    // 同步快照：业务线程可等闸；观测端点用 status_snapshot() 永不等待业务闸
    [[nodiscard]] std::string status_json();

    // 观测端点专用：try 全部闸成功则刷新缓存，失败则回上一份快照——绝不因业务泵卡住观测
    [[nodiscard]] std::string status_snapshot();

    [[nodiscard]] std::string status_json_locked() const;

    [[nodiscard]] std::string intercept_events() const;

    [[nodiscard]] std::uint64_t first_term() const;

    [[nodiscard]] std::uint64_t first_commit() const;

    [[nodiscard]] std::uint64_t sessions_safe_point();

    static constexpr std::uint64_t kSplitClient = 0x5F5F5F5F;   // split admin 专用 client id
    static constexpr std::uint64_t kSessionIdleTtlMs = 30'000;  // 空闲会话回收线（锁 TTL 的 10 倍）

    ServerOptions opt_;
    std::mutex peer_mu_;  // 全局闸的退化形态（分片关闭/多副本）；分片开启时仅此路径仍在用
    std::unique_ptr<LockRegistry> locks_;  // ⑨a 分桶注册表（start 按配置建，shards 恒定）
    bool sharded_{false};                  // 分片是否生效（单节点 && lock_shards>1）
    std::unique_ptr<RegionPeerRegistry> registry_;
    std::unique_ptr<raftstore::TcpTransport> transport_;  // 多副本传输层（单节点为 nullptr）
    raftstore::StoreMeta meta_;
    std::unique_ptr<raftstore::StoreMetaStore> meta_store_;  // ⑧ 路由真相仓储（start 时装配）
    // 物理基线 TSO（生产语义）+ Checked 装饰器（红线 6：非严格递增即拒绝服务）。
    // 声明顺序即构造顺序：装饰器引用前者，故前者先声明。
    tso::MonotonicTsProvider tso_{true};
    tso::CheckedTsProvider checked_tso_{tso_};
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
    std::atomic<std::uint64_t> next_region_id_{1};  // 下一个 split 新 region id（CAS 前置）
    mutable std::mutex split_error_mu_;  // ⑪ 审计 #15：写入在 apply 路径（可能仅持分区闸），读在 admin_split
    std::string split_error_;            // observer 里迁移失败的回执（split 线程写）
    InterceptorChain interceptors_;
    MetricsInterceptor* metrics_{nullptr};  // 拥有权在 interceptors_ 链中
    TraceInterceptor* trace_{nullptr};      // 拥有权在 interceptors_ 链中
    bool auth_forced_loopback_{false};      // 未鉴权被强制绑回环（启动告警的事实来源）
    bool running_{false};

public:
    std::atomic<std::uint64_t> now_ms_{0};  // 注入时钟：主循环每秒推进，HTTP 线程只读
};


// ---- ⑮ 结构还债（SPEC-7 §7.2.5）：成员定义分段，纯移动零语义变化 ----
// 以下两份头文件承载类外成员定义（inline），须在 ServerApp 类定义之后、仅由本头包含。
#include "server/server_gate.h"    // 闸/泵辅助（gate_for/try_gate_global/pump 族）
#include "server/server_http.h"    // handler 面（kv/admin/status/observe）
}  // namespace tinykv::server
