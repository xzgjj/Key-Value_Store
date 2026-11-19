// 文件用途：ServerApp HTTP handler 面 成员定义（⑮ 从 server_app.h 纯移动，SPEC-7 §7.2.5）。
// 归属：TinyKV-CPP（M8 优化）。
// 警告：仅由 server_app.h 在 ServerApp 类定义之后包含（类外成员定义），不可单独包含。

#pragma once

    [[nodiscard]] inline  std::uint64_t ServerApp::sessions_safe_point() {
        // 无活跃会话时取当前 TSO（全部已提交数据只保留最新版本）；有会话则取最小活跃 start_ts
        return sessions_->active() > 0 ? sessions_->min_active_start_ts().value
                                       : tso_.current().value;
    }

    [[nodiscard]] inline  std::uint64_t ServerApp::first_commit() const {
        if (registry_ == nullptr) {
            return 0;
        }
        const auto peers = registry_->all_peers();
        return peers.empty() ? 0 : peers.front()->raft().committed().value;
    }

    [[nodiscard]] inline  std::uint64_t ServerApp::first_term() const {
        if (registry_ == nullptr) {
            return 0;
        }
        const auto peers = registry_->all_peers();
        return peers.empty() ? 0 : peers.front()->raft().term().value;
    }

    [[nodiscard]] inline  std::string ServerApp::intercept_events() const { return trace_->snapshot(); }

    [[nodiscard]] inline  std::string ServerApp::status_json_locked() const {
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
               ",\"txn_errors\":" + std::to_string(txn_errors) + ",\"last_apply_error\":\"" +
               http::escape(last_error) + "\"" +                ",\"now_ms\":" + std::to_string(now_ms_.load()) +
               ",\"auth\":" + (opt_.auth_token.empty() ? "false" : "true") +
               // ⑭ 红线 3：未配 TLS 时 /status 逐字节不变——"tls" 字段只在配置证书时出现
               (opt_.tls_cert.empty() ? "" : ",\"tls\":true") +
               ",\"txn_cross_region\":" + (opt_.cross_region_enabled ? "true" : "false") +
               ",\"bind\":\"" +
               http::escape(opt_.host) + "\"" +
               // ⑭ D7-3：TLS 下 token 可选（传输已加密），不再标"未鉴权"
               (!opt_.auth_token.empty() || !opt_.tls_cert.empty() || is_loopback(opt_.host)
                    ? ""
                    : ",\"warn\":\"未鉴权\"") +
               ",\"replica_id\":" + std::to_string(opt_.replica_id.value) +
               ",\"voters\":" + std::to_string(opt_.voters.size()) +
               ",\"leader\":" + (is_leader() ? "true" : "false") +
               ",\"applied\":" + std::to_string(applied) +
               ",\"term\":" + std::to_string(first_term()) +
               ",\"commit\":" + std::to_string(first_commit()) + "}";
    }

    // 观测端点专用：try 全部闸成功则刷新缓存，失败则回上一份快照——绝不因业务泵卡住观测
    [[nodiscard]] inline  std::string ServerApp::status_snapshot() {
        if (auto gate = try_gate_global(); gate.has_value()) {
            auto json = status_json_locked();
            std::lock_guard cache_lock(observe_mu_);
            observe_cache_ = std::move(json);
        }
        std::lock_guard cache_lock(observe_mu_);
        return observe_cache_.empty() ? std::string{"{\"node\":\"tinykv-server\",\"ready\":false}"}
                                      : observe_cache_;
    }

    // 同步快照：业务线程可等闸；观测端点用 status_snapshot() 永不等待业务闸
    [[nodiscard]] inline  std::string ServerApp::status_json() {
        auto gate = gate_for(true, {});  // 全局闸：快照读全部 peer
        return status_json_locked();
    }

    // 调度器视图（CLI `scheduler status` 的落点）。单 store 没有真正需要调度的对象，
    // 因此报告"事实 + 为什么没有调度器"，避免让调用方以为有 PD 在背后工作。
    [[nodiscard]] inline  std::string ServerApp::scheduler_status() {
        std::size_t leaders = 0;
        if (registry_ != nullptr) {
            for (auto* peer : registry_->all_peers()) {
                if (peer->is_leader()) {
                    ++leaders;
                }
            }
        }
        return "{\"mode\":\"pd-lite-embedded\",\"regions\":" +
               std::to_string(meta_.region_count()) + ",\"leaders\":" + std::to_string(leaders) +
               ",\"voters\":" + std::to_string(opt_.voters.size()) +
               ",\"note\":\"单 store 无跨节点调度对象，调度器以库形态存在\"}";
    }

    // 副本分布（CLI `region peers <id>` 的落点）。单 store 单 peer/region 是当前事实，
    // 如实报告而不是编造多副本表：store_id 留 0 = 本机未配置多 store。
    [[nodiscard]] inline  std::string ServerApp::region_peers(const std::string& id_text) {
        const auto id = http::parse_u64(id_text);
        if (!id.has_value()) {
            return {};
        }
        auto region = meta_.find_region_by_id(RegionId{*id});
        if (!region.has_value()) {
            return {};
        }
        std::string out = "[";
        if (registry_ != nullptr) {
            auto* peer = registry_->peer_by_id(region->id);
            if (peer != nullptr) {
                out += "{\"peer_id\":" + std::to_string(peer->id().value) +
                       ",\"store_id\":0,\"leader\":" + (peer->is_leader() ? "true" : "false") + "}";
            }
        }
        return out + "]";
    }

    [[nodiscard]] inline  std::string ServerApp::region_detail(const std::string& id_text) {
        const auto id = http::parse_u64(id_text);
        if (!id.has_value()) {
            return {};  // BUG-0002: 畸形查询参数 → 404，不得抛异常打崩进程
        }
        auto r = meta_.find_region_by_id(RegionId{*id});
        if (!r.has_value()) {
            return {};
        }
        std::string base = http::region_json(*r);
        base.pop_back();  // 去掉尾 }，拼入 term/commit 后再闭合
        auto* peer = registry_->peer_by_id(r->id);
        const auto term = peer == nullptr ? 0U : peer->raft().term().value;
        const auto commit = peer == nullptr ? 0U : peer->raft().committed().value;
        return base + ",\"term\":" + std::to_string(term) +
               ",\"commit\":" + std::to_string(commit) + "}";
    }

    // GET 族：会话读 + 观测
    [[nodiscard]] inline  std::string ServerApp::admin_get(const std::string& path, const std::string& query) {
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
            return "{\"session\":\"" + sid + "\",\"start_ts\":" + std::to_string(st->value) + "}";
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
        if (path == "/region/peers") {
            return region_peers(http::http_query_param(query, "id"));
        }
        if (path == "/scheduler/status") {
            return scheduler_status();
        }
        if (path == "/trace") {
            return intercept_events();
        }
        if (path == "/metrics") {
            return metrics_->snapshot();
        }
        return {};  // 未知 admin 路径 → 404
    }

    // 会话终结（commit/rollback）
    [[nodiscard]] inline  std::string ServerApp::admin_txn_finish(bool is_commit,
                                               const std::vector<std::string>& parts) {
        if (parts.size() != 2) {
            return "ERR bad-request";
        }
        if (!is_commit) {
            const auto rb = sessions_->rollback(parts[1]);
            if (!rb.has_value()) {
                return write_error(rb.error());
            }
            return "OK";
        }
        auto r = sessions_->commit(parts[1], now_ms_);
        // 错误带上结构化细节：跨区拒绝、锁冲突、IO 失败的原因都在字段里，
        // 只回 kind 的话调用方（和人）都无法定位（M7 跨区上线时踩到这点）。
        return r.has_value() ? "OK commit_ts=" + std::to_string(r->value)
                             : "ERR " + tinykv::describe(r.error());
    }

    // 写路径失败回执：只回错误种类（细节在日志；读路径才需要字段级 JSON）
    [[nodiscard]] inline  std::string ServerApp::write_error(const KvError& error) {
        return "ERR " + std::string(tinykv::describe_kind(error));
    }

    // 会话写缓冲（set/del）：显式分支而非嵌套三元——三元链在这里既难读也会把
    // 函数的圈复杂度推过门禁（每个 ? 都算一个分支）。
    [[nodiscard]] inline  std::string ServerApp::admin_txn_buffered(bool is_set,
                                                 const std::vector<std::string>& parts) {
        if (is_set) {
            if (parts.size() != 4) {
                return "ERR bad-request";
            }
            const auto r = sessions_->set(parts[1], parts[2], parts[3]);
            if (!r.has_value()) {
                return write_error(r.error());
            }
            return "OK";
        }
        if (parts.size() != 3) {
            return "ERR bad-request";
        }
        const auto r = sessions_->del(parts[1], parts[2]);
        if (!r.has_value()) {
            return write_error(r.error());
        }
        return "OK";
    }

    // region split（M4 功能在 M6 多 region 下重新接通）：propose → apply → observer 动态建 peer
    // 全局闸：split 触碰 meta 拓扑 + 新建 peer（跨 region 操作）
    [[nodiscard]] inline  std::string ServerApp::admin_split(const std::vector<std::string>& parts) {
        if (parts.size() != 3) {
            return "ERR bad-request";
        }
        auto gate = gate_for(true, {});
        const auto rid = http::parse_u64(parts[1]);
        if (!rid.has_value()) {
            return "ERR bad-request";  // BUG-0002: 非数字 region id 不得击穿解析路径
        }
        const auto region_id = RegionId{*rid};
        auto* peer = registry_->peer_by_id(region_id);
        if (peer == nullptr) {
            return "ERR no-region";
        }
        // code-review：多 voter 下 RegionPeerRegistry 无法保证新 region 的 PeerId 全局唯一
        // （raftpb::Message 只带 to=PeerId，无 region_id），split 会造出不可路由的 peer。
        // 这个判定必须在 propose **之前**——旧实现在 apply 之后才返回 unsupported，
        // 此时 split 已经生效，留下了不可路由的 peer。
        if (multi_voter()) {
            return "ERR split-unsupported-in-multi-voter";
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
        if (!wait_applied_locked(*peer, expect)) {
            peer->drop_reply(kSplitClient, expect);  // 超时必须回收，避免回执表累积
            return "ERR apply-timeout";
        }
        peer->drop_reply(kSplitClient, expect);  // admin 回执不消费，必须显式回收
        next_region_id_.fetch_add(1);
        // BUG-0022 配套：on_split 改为"只注册不泵"后，成功路径在这里（仍持全局闸）补泵
        // 新 peer——保住"split 返回后右半立即可写"的既有契约。apply 超时路径不补泵：
        // 残留 apply 由后续分区闸内泵/Worker 完成，新 peer 在其桶内被推举，同样无竞态。
        if (auto* fresh = registry_->peer_by_id(RegionId{next_region_id_.load()});
            fresh != nullptr && !fresh->is_leader()) {
            pump_peer_only(*fresh);
        }
        std::string err;
        {
            const std::lock_guard lock(split_error_mu_);  // ⑪ 审计 #15：与 apply 路径的写入互斥
            err = split_error_;
        }
        return err.empty() ? "OK split" : "ERR " + err;
    }

    // 采样开关：body 形如 `trace\t<region_id>\ton|off`（region 必须存在，否则 ERR no-region）
    [[nodiscard]] inline  std::string ServerApp::admin_trace(const std::vector<std::string>& parts) {
        if (parts.size() != 3) {
            return "ERR bad-request";
        }
        const bool on = parts[2] == "on";
        if (!on && parts[2] != "off") {
            return "ERR bad-request";
        }
        const auto id = http::parse_u64(parts[1]);
        if (!id.has_value()) {
            return "ERR bad-request";
        }
        if (!meta_.find_region_by_id(RegionId{*id}).has_value()) {
            return "ERR no-region";
        }
        if (trace_ != nullptr) {
            trace_->set_enabled(on);
        }
        return on ? "OK trace on" : "OK trace off";
    }

    // GC：body 形如 `gc` 或 `gc\t<safe_point>`；安全点缺省取当前 TSO / 最小活跃 start_ts
    [[nodiscard]] inline  std::string ServerApp::admin_txn_gc(const std::vector<std::string>& parts) {
        const auto safe = parts.size() == 2 ? http::parse_u64(parts[1]) : std::nullopt;
        if (parts.size() > 1 && !safe.has_value()) {
            return "ERR bad-request";
        }
        const Ts point{safe.value_or(sessions_safe_point())};
        auto r = exec_->gc(point.value);
        if (!r.has_value()) {
            return write_error(r.error());
        }
        return "OK gc";  // 计数在 apply 侧日志
    }

    [[nodiscard]] inline  std::string ServerApp::admin_txn_begin() {
        auto sid = sessions_->begin(now_ms_.load());
        return sid.has_value() ? "OK session=" + *sid
                               : "ERR " + std::string(tinykv::describe_kind(sid.error()));
    }

    // POST 族：会话写操作 + GC（executor 自带闸，此处不加全局闸）
    [[nodiscard]] inline  std::string ServerApp::admin_post(const std::string& path, const std::string& body) {
        const auto parts = http::split_frame(body);
        if (path == "/txn/begin") {
            return admin_txn_begin();
        }
        if (path == "/txn/set" || path == "/txn/del") {
            return admin_txn_buffered(path == "/txn/set", parts);
        }
        if (path == "/txn/commit" || path == "/txn/rollback") {
            return admin_txn_finish(path == "/txn/commit", parts);
        }
        if (path == "/txn/gc") {
            return admin_txn_gc(parts);
        }
        if (path == "/trace") {
            return admin_trace(parts);
        }
        if (path == "/region/split") {
            return admin_split(parts);
        }
        return {};
    }

    // 观测端口的处理器白名单：只暴露读型观测路径，事务写/GC/split 一律 404。
    [[nodiscard]] inline  std::string ServerApp::observe_admin(const std::string& method, const std::string& path,
                                            const std::string& query, const std::string& body) {
        if (method != "GET") {
            return {};
        }
        if (path != "/regions" && path != "/region" && path != "/metrics" && path != "/trace") {
            return {};
        }
        return admin(method, path, query, body);
    }

    // ---- 事务/观测端点（docs/cli.md §11；会话作用域） ----
    [[nodiscard]] inline  std::string ServerApp::admin(const std::string& method, const std::string& path,
                                    const std::string& query, const std::string& body) {
        txn_ops_.fetch_add(1, std::memory_order_relaxed);
        return method == "POST" ? admin_post(path, body) : admin_get(path, query);
    }

    [[nodiscard]] inline  std::string ServerApp::kv_write(const std::string& body) {
        const auto parts = http::split_frame(body);
        if (parts.size() < 2 || parts[1].empty()) {
            return "ERR bad-request";
        }
        auto gate = gate_for(false, parts[1]);  // ⑨a 分区闸：写只触碰 key 所属 region
        kv_writes_.fetch_add(1, std::memory_order_relaxed);
        auto* peer = registry_->peer_for(parts[1]);
        if (peer == nullptr) {
            return "ERR no-region";  // P0-4：绝不拿别的 region 的 peer 兜底写
        }
        const auto expect = peer->raft().last_index().value + 1;
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
        auto accepted = peer->propose(req);
        if (!accepted.has_value()) {
            return "ERR not-leader";
        }
        if (!*accepted) {
            // ARCH-1 P0-3：去重命中 = 该 (client,seq) 早已 apply 过，语义上已成功。
            // 这里不能再等 applied 追平 last_index+1——那个位点永远到不了。
            peer->drop_reply(1, expect);  // 回收 propose 登记的合成回执
            return "OK";
        }
        if (!wait_applied_locked(*peer, expect)) {
            // code-review：超时必须回收回执，否则反复超时会累积（replies_ 无界增长）
            peer->drop_reply(1, expect);
            return "ERR apply-timeout";
        }
        peer->drop_reply(1, expect);  // 组合根轮询位点，不消费回执 → 显式回收
        return "OK";
    }

    // ---- kv 面端点（经 raft 提案，"写走共识"红线不豁免） ----
    [[nodiscard]] inline  std::string ServerApp::kv_read(const std::string& query) {
        const auto key = http::http_query_param(query, "key");
        if (key.empty()) {
            return {};
        }
        auto gate = gate_for(false, key);  // ⑨a 分区闸：读只触碰 key 所属 region
        kv_reads_.fetch_add(1, std::memory_order_relaxed);
        auto* peer = registry_->peer_for(key);
        if (peer == nullptr) {
            return "{\"error\":\"no-region\"}";  // P0-4：不拿别的 region 兜底读
        }
        // 线性一致读：ReadIndex 确认 + applied 追平后才读状态机（不再直接 leader 本地读）
        // 多副本下 pump 必须让出 CPU，否则心跳确认的 TCP 回包没机会到达 io_context 线程。
        const auto ri = peer->wait_read_index(
            [&] {
                if (multi_voter()) {
                    pump_locked();
                    std::this_thread::yield();
                } else {
                    pump_peer_only(*peer);  // ⑨a：分区闸下只泵本 peer
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
        auto got = peer->read_directly(Cf::Default, encode_raw_key(key));
        if (!got.has_value()) {
            return "{\"error\":\"" + std::string(tinykv::describe_kind(got.error())) + "\"}";
        }
        if (!got->value.has_value()) {
            return "{\"found\":false}";
        }
        return "{\"found\":true,\"value\":\"" + http::escape(*got->value) + "\"}";
    }

    template <typename Fn>
    [[nodiscard]] inline  std::string ServerApp::intercepted(const std::string& method, const std::string& path,
                                          const std::string& key, Fn&& fn) {
        auto ev = interceptors_.begin(method, path, key);
        // code-review：fn() 抛异常时 finish 也必须执行，否则 metrics/trace 丢事件。
        try {
            auto out = fn();
            const bool ok = !out.empty() && out.rfind("ERR", 0) != 0;
            interceptors_.finish(ev, ok, ok ? std::string{"OK"} : out.substr(0, 48));
            return out;
        } catch (...) {
            interceptors_.finish(ev, false, std::string{"exception"});
            throw;
        }
    }
