// 文件用途：TxnSessionManager——会话作用域（M6 第④层：解锁 CLI 交互式事务）。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 会话 = 服务端持有的一条活跃事务（TxnClient + 执行端）。session_id 单调递增（可预测、
// 可日志化）。活跃会话数与最小活跃 start_ts 是 GC 安全点的宿主（FUNC-17 前置）。
// 会话在 commit/rollback 后自动关闭；未关闭会话由 /txn/status 观测（TTL 清理随 daemon 演进）。

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/error.h"
#include "txn/client.h"
#include "txn/executor.h"

namespace tinykv::server {

class TxnSessionManager {
public:
    TxnSessionManager(txn::TxnExecutor& executor, tinykv::tso::TsProvider& tso)
        : executor_(executor), tso_(tso) {}

    [[nodiscard]] tinykv::result<std::string> begin(std::uint64_t now_ms = 0) {
        std::lock_guard lock(mu_);
        auto client = std::make_unique<tinykv::txn::TxnClient>(executor_, tso_);
        auto r = client->begin();
        if (!r.has_value()) {
            return std::unexpected(r.error());
        }
        const auto sid = "t" + std::to_string(++next_id_);
        clients_[sid] = std::move(client);
        created_ms_[sid] = now_ms;
        return sid;
    }

    // 空闲会话回收：超时会话走一次 rollback 清锁，成功后从表与 GC 安全点宿主中移除。
    // 回滚失败（非 leader 等）保留会话，下一轮再试——不允许把未清的锁直接丢掉。
    void sweep(std::uint64_t now_ms, std::uint64_t idle_ttl_ms) {
        std::vector<std::string> expired;
        {
            std::lock_guard lock(mu_);
            for (const auto& [sid, created] : created_ms_) {
                if (now_ms >= created && now_ms - created >= idle_ttl_ms) {
                    expired.push_back(sid);
                }
            }
        }
        for (const auto& sid : expired) {
            auto client = take(sid);
            if (!client.has_value()) {
                continue;
            }
            if (!(*client)->rollback().has_value()) {
                put_back(sid, std::move(*client));
                continue;
            }
            std::lock_guard lock(mu_);
            clients_.erase(sid);
            created_ms_.erase(sid);
        }
    }

    [[nodiscard]] tinykv::result<void> set(const std::string& sid, const std::string& key,
                                           const std::string& value) {
        auto client = take(sid);
        if (!client.has_value()) {
            return std::unexpected(session_missing(sid));
        }
        auto r = (*client)->set(key, value);
        if (r.has_value()) {
            put_back(sid, std::move(*client));
        } else {
            put_back(sid, std::move(*client));
        }
        return r;
    }

    [[nodiscard]] tinykv::result<void> del(const std::string& sid, const std::string& key) {
        auto client = take(sid);
        if (!client.has_value()) {
            return std::unexpected(session_missing(sid));
        }
        auto r = (*client)->del(key);
        put_back(sid, std::move(*client));
        return r;
    }

    [[nodiscard]] tinykv::result<std::optional<std::string>> get(const std::string& sid,
                                                                 const std::string& key,
                                                                 std::uint64_t now_ms) {
        auto r = get_full(sid, key, now_ms);
        if (!r.has_value()) {
            return std::unexpected(r.error());
        }
        return r->value;
    }

    // 带 blocked 语义的读（回执文本需要区分"未过期锁阻塞"与"无值"）
    [[nodiscard]] tinykv::result<std::string> get_verbose(const std::string& sid,
                                                          const std::string& key,
                                                          std::uint64_t now_ms) {
        auto r = get_full(sid, key, now_ms);
        if (!r.has_value()) {
            return std::unexpected(r.error());
        }
        if (r->blocked) {
            return std::string("BLOCKED");
        }
        return r->value.value_or(std::string{"(nil)"});
    }

    [[nodiscard]] tinykv::result<Ts> commit(const std::string& sid, std::uint64_t now_ms) {
        auto client = take(sid);
        if (!client.has_value()) {
            return std::unexpected(session_missing(sid));
        }
        auto r = (*client)->commit(now_ms);
        if (r.has_value()) {
            std::lock_guard lock(mu_);
            clients_.erase(sid);   // 提交即关闭会话
            created_ms_.erase(sid);
        } else {
            put_back(sid, std::move(*client));
        }
        return r;
    }

    [[nodiscard]] tinykv::result<void> rollback(const std::string& sid) {
        auto client = take(sid);
        if (!client.has_value()) {
            return std::unexpected(session_missing(sid));
        }
        auto r = (*client)->rollback();
        std::lock_guard lock(mu_);
        clients_.erase(sid);   // 回滚即关闭会话
        created_ms_.erase(sid);
        return r;
    }

    [[nodiscard]] tinykv::result<Ts> session_start_ts(const std::string& sid) {
        std::lock_guard lock(mu_);
        auto it = clients_.find(sid);
        if (it == clients_.end()) {
            return std::unexpected(session_missing(sid));
        }
        return it->second->start_ts();
    }

    // GC 安全点宿主：全部活跃会话 start_ts 的最小值（无会话时返回当前 TS 即无限制）
    [[nodiscard]] Ts min_active_start_ts() const {
        std::lock_guard lock(mu_);
        Ts best{std::numeric_limits<std::uint64_t>::max()};
        for (const auto& [sid, client] : clients_) {
            best = Ts{std::min(best.value, client->start_ts().value)};
        }
        return best;
    }

    [[nodiscard]] std::size_t active() const {
        std::lock_guard lock(mu_);
        return clients_.size();
    }

private:
    // 会话读核心：取客户端 → 读 → 放回（get 与 get_verbose 唯一实现点）
    [[nodiscard]] tinykv::result<tinykv::txn::TxnGetResult> get_full(const std::string& sid,
                                                                     const std::string& key,
                                                                     std::uint64_t now_ms) {
        auto client = take(sid);
        if (!client.has_value()) {
            return std::unexpected(session_missing(sid));
        }
        auto r = (*client)->get(key, now_ms);
        put_back(sid, std::move(*client));
        return r;
    }

    [[nodiscard]] static InvalidArgument session_missing(const std::string& sid) {
        return InvalidArgument{"会话不存在或已结束: " + sid};
    }

    [[nodiscard]] std::optional<std::unique_ptr<tinykv::txn::TxnClient>> take(
        const std::string& sid) {
        std::lock_guard lock(mu_);
        auto it = clients_.find(sid);
        if (it == clients_.end()) {
            return std::nullopt;
        }
        auto client = std::move(it->second);
        clients_.erase(it);
        return client;
    }

    void put_back(const std::string& sid, std::unique_ptr<tinykv::txn::TxnClient> client) {
        std::lock_guard lock(mu_);
        clients_[sid] = std::move(client);
    }

    txn::TxnExecutor& executor_;
    tinykv::tso::TsProvider& tso_;
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<tinykv::txn::TxnClient>> clients_;
    std::map<std::string, std::uint64_t> created_ms_;   // sid → 创建时刻（空闲回收用）
    std::uint64_t next_id_{0};
};

}  // namespace tinykv::server
