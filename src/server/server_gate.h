// 文件用途：ServerApp 闸/泵辅助 成员定义（⑮ 从 server_app.h 纯移动，SPEC-7 §7.2.5）。
// 归属：TinyKV-CPP（M8 优化）。
// 警告：仅由 server_app.h 在 ServerApp 类定义之后包含（类外成员定义），不可单独包含。

#pragma once

    // 泵指定 key 所属 peer（executor 在闸内调用；新 peer 首次泵负责把 split observer 建的
    // 实例推成 leader 并 apply 初始条目）
inline     void ServerApp::pump_one(std::string_view key) {
        if (multi_voter()) {
            pump_locked();  // 多副本：选举/复制靠消息交换，高频 tick 会打乱节奏
            return;
        }
        auto* peer = registry_->peer_for(key);
        if (peer == nullptr) {
            return;
        }
        pump_peer_only(*peer);
    }

    // 单 peer 泵（⑨a 分区闸配套）：只推进目标 peer，绝不触碰别的 peer/transport——
    // 否则分区闸下的泵会与另一 region 的 handler 数据竞争。多副本一律走 pump_locked
    // （全局闸），多副本模式永不启用分片（类注释）。
inline     void ServerApp::pump_peer_only(raftstore::Peer& peer) {
        for (int i = 0; i < 20 && !peer.is_leader(); ++i) {
            peer.tick();
        }
        peer.tick();
        (void)peer.on_ready();
    }

    // 等待位点推进：单节点无网络，紧泵 400 轮即可（⑨a：只泵目标 peer，其他 peer 由
    // Worker 驱动——分区闸下泵全部 peer 会跨桶触碰他 region 的状态）；多副本必须让出
    // CPU 等 TCP 往返，否则 400 轮空转在 1ms 内跑完，对端心跳响应没机会回来就误判超时。
    [[nodiscard]] inline  bool ServerApp::wait_applied_locked(raftstore::Peer& peer, std::uint64_t idx) {
        if (!multi_voter()) {
            for (int i = 0; i < 400; ++i) {
                pump_peer_only(peer);
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

inline     void ServerApp::pump_locked() {
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

    // ---- M7 ⑤：统一泵——收件 → 步进 → apply → 发件。调用方必须持全局闸 ----
    // tick 由 Worker 单独驱动；HTTP 路径只 pump 不 tick，避免高频时钟打乱选举节奏。
    // 顺序很关键：先 step 收件（让 follower 及时跟上 leader），再 on_ready 推进共识与
    // apply，最后 take_messages 发件；take_messages 绝不单独调用而不 on_ready（会清掉队列）。
inline     void ServerApp::tick_and_pump_locked() {
        for (auto* peer : registry_->all_peers()) {
            peer->tick();
        }
        pump_locked();
    }

    // 回环判定：未鉴权时唯一允许的监听面（含 IPv6 回环与 localhost 别名）
    [[nodiscard]] inline  bool ServerApp::is_loopback(std::string_view host) {
        return host == "127.0.0.1" || host == "::1" || host == "localhost";
    }

    // key → region（StoreMeta 自带 RWMutex，免额外闸）
    [[nodiscard]] inline  std::optional<RegionId> ServerApp::region_of_key(std::string_view key) {
        auto r = meta_.find_region(key);
        return r.has_value() ? std::optional<RegionId>{r->id} : std::nullopt;
    }

    // 观测端点专用：try 全部闸，拿不到就回缓存（绝不因业务泵卡住观测读）
    [[nodiscard]] inline  std::optional<PeerGateHandle> ServerApp::try_gate_global() {
        if (!sharded_) {
            std::unique_lock lock(peer_mu_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return std::nullopt;
            }
            std::vector<std::unique_lock<std::mutex>> locks;
            locks.push_back(std::move(lock));
            return PeerGateHandle(std::move(locks));
        }
        auto all = locks_->try_lock_all();
        if (!all.has_value()) {
            return std::nullopt;
        }
        return PeerGateHandle(std::move(*all));
    }

    // ---- ⑨a 闸：分区闸（单 region）与全局闸（跨 region）的唯一入口 ----
    // 分片关/多副本：两种闸都退化为 peer_mu_（单把，句柄里只有一把锁）。
    // 分片开：分区闸=region 所属桶；全局闸=全部桶（lock_all 升序）。
    // key 无归属（理论不可达，REL-05）→ 保守取全局闸，绝不"猜一个桶"。
    [[nodiscard]] inline  PeerGateHandle ServerApp::gate_for(bool global, std::string_view key) {
        if (!sharded_) {
            std::vector<std::unique_lock<std::mutex>> locks;
            locks.emplace_back(peer_mu_);
            return PeerGateHandle(std::move(locks));
        }
        if (global) {
            return PeerGateHandle(locks_->lock_all());
        }
        const auto region = region_of_key(key);
        if (!region.has_value()) {
            return PeerGateHandle(locks_->lock_all());
        }
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.push_back(locks_->lock(*region));
        return PeerGateHandle(std::move(locks));
    }
