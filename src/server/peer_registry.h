// 文件用途：RegionPeerRegistry（M6 挂账收口）——region→peer 动态注册表（Storage×N / Peer×regions）。
// 归属：TinyKV-CPP（M6 运行时）。
//
// 为什么每 region 独立 Engine：raft log 以固定前缀 "\0re"/"\0lm" 存于 Default CF，
// 两个 Peer 共用一个 Engine 会串 log（都从 index=1 写同一把 key）。故每个 region 一份
// 独立 Engine（子目录 data_dir/regions/<id>），对应"Storage×N"。
//
// 路由：StoreMeta 按 key 找 region → 本表按 region_id 找 Slot（Engine + Peer）。
// split 动态建实例：Peer apply split 后通过 observer 回调 on_split(left,right)——
// 本表建右半新 Slot（新 PeerId = region id，单节点组），把旧 Engine 中落在右区间
// [split_key, old_end) 的 MVCC 条目迁到新 Engine，并从旧 Engine 删除（数据归属感）。
// 回调在 Peer 的 apply 路径内触发，此处只操作存储/元数据，绝不重入 peer 的 pump。
//
// 并发（⑨a 分片接线后）：slots_ 表被"分区闸下的路由读"与"apply 路径内的 on_split"
// 并发访问——本类自带 mu_ 保护表结构本身（⑨a 前 {slots_} 由 peer_mu_ 全局闸覆盖，
// 分片后 peer 之外的访问点只可能落在这些方法里，内部加锁让组合根无需第二把共享闸）。
// Peer/Engine 实例自身仍由调用方按 region 串行（锁序：region 桶 → 本表 mu_）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <type_traits>
#include "common/compat.h"
#include "common/encode.h"
#include "common/error.h"
#include "common/error_format.h"
#include "common/ids.h"
#include "raftstore/peer.h"
#include "raftstore/region.h"
#include "storage/engine.h"
#include "storage/storage.h"
namespace tinykv::server {

struct PeerSlot {
    RegionId region_id;
    raftstore::Region region;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<raftstore::Peer> peer;
};

class RegionPeerRegistry {
public:
    using ObserverFactory =
        std::function<void(raftstore::Peer&)>;  // 宿主给每个新 peer 挂 split 钩子

    // fsync_on_commit 默认 false：本表主要由测试装置直接构造（每次写都 fsync 会让用例慢一个量级）。
    // 生产装配点（ServerApp）显式传入配置值，默认 true——code-review P0-1 的收口就在这里：
    // 掉电丢已确认写是数据丢失，不能由"测试快一点"的需求决定。
    explicit RegionPeerRegistry(std::filesystem::path data_dir, raftstore::StoreMeta& meta,
                                std::uint64_t election_timeout = 8, std::uint64_t heartbeat = 3,
                                PeerId replica_id = PeerId{1}, std::vector<PeerId> voters = {},
                                bool fsync_on_commit = false)
        : data_dir_(std::move(data_dir)),
          meta_(meta),
          election_(election_timeout),
          heartbeat_(heartbeat),
          replica_id_(replica_id),
          voters_(voters.empty() ? std::vector<PeerId>{replica_id} : std::move(voters)),
          fsync_on_commit_(fsync_on_commit) {}

    void set_observer_factory(ObserverFactory fn) { observer_factory_ = std::move(fn); }

    // 建 slot：region 1 独占全空间时用 data_dir 根（兼容 M1 起的单 region 数据布局），
    // 其余 region 用 data_dir/regions/<id>。meta 恢复时逐个 region 走这里（⑧）。
    [[nodiscard]] result<void> bootstrap(const raftstore::Region& region) {
        auto eng =
            Engine::open({.dir = engine_dir_for(region.id), .fsync_on_commit = fsync_on_commit_});
        if (!eng.has_value()) {
            return tinykv::unexpected(eng.error());
        }
        auto peer = raftstore::Peer::open(raft::Config{.id = replica_id_,
                                                       .voters = voters_,
                                                       .election_timeout = election_,
                                                       .heartbeat_interval = heartbeat_},
                                          **eng);
        if (!peer.has_value()) {
            return tinykv::unexpected(peer.error());
        }
        (*peer)->set_region(region);
        (*peer)->set_store_meta(&meta_);
        install(region, std::move(*eng), std::move(*peer));
        return {};
    }

    // region 的数据目录：region 1 沿用根目录（M1 布局），子 region 各自 regions/<id>。
    // 双布局并存是既有事实（ARCH-3 §4.4 已记录），迁移属 format 变更，不在本波次做。
    [[nodiscard]] std::filesystem::path engine_dir_for(RegionId id) const {
        if (id.value == 1) {
            return data_dir_;
        }
        return data_dir_ / "regions" / std::to_string(id.value);
    }

    // 按 key 找覆盖它的 region 对应的 Peer（route = StoreMeta 区间索引 + 本表 id 索引）
    [[nodiscard]] raftstore::Peer* peer_for(std::string_view key) const {
        auto region = meta_.find_region(key);
        if (!region.has_value()) {
            return nullptr;
        }
        return peer_by_id(region->id);
    }

    [[nodiscard]] raftstore::Peer* peer_by_id(RegionId id) const {
        std::lock_guard lock(mu_);
        auto it = slots_.find(id);
        return it == slots_.end() ? nullptr : it->second.peer.get();
    }

    // 传输层按 Raft PeerId 寻址（多副本时 PeerId ≠ RegionId）
    [[nodiscard]] raftstore::Peer* peer_by_peer_id(PeerId id) const {
        std::lock_guard lock(mu_);
        for (const auto& [region, slot] : slots_) {
            (void)region;
            if (slot.peer->id() == id) {
                return slot.peer.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] const std::vector<PeerId>& voters() const { return voters_; }

    [[nodiscard]] raftstore::Peer& require(std::string_view key) const {
        auto* p = peer_for(key);
        if (p == nullptr) {
            // 理论不可达（REL-05：每个 key 恰属一个 region）；保底返回首个 peer 避免解引用空
            std::lock_guard lock(mu_);
            return *slots_.begin()->second.peer;
        }
        return *p;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mu_);
        return slots_.size();
    }

    // 遍历所有 peer（GC 广播、Worker 泵、stop 逆序收尾）。返回指针列表：
    // slot 指针稳定（map 节点），split 只增不删，Peer& 生命期覆盖到 close_all。
    [[nodiscard]] std::vector<raftstore::Peer*> all_peers() const {
        std::lock_guard lock(mu_);
        std::vector<raftstore::Peer*> out;
        out.reserve(slots_.size());
        for (const auto& [id, slot] : slots_) {
            (void)id;
            out.push_back(slot.peer.get());
        }
        return out;
    }

    // split 回调：动态建右半 peer + 迁移数据。left 是本 peer 保留的左区间。
    // 整个操作持 mu_：迁移期间路由读只会看到旧 slot（右半还不存在），install 后原子可见。
    [[nodiscard]] result<void> on_split(const raftstore::Region& left,
                                        const raftstore::Region& right) {
        std::lock_guard lock(mu_);
        if (slots_.find(right.id) != slots_.end()) {
            return {};  // 幂等：已建过
        }
        auto* old_slot = find_slot(left.id);
        if (old_slot == nullptr) {
            return tinykv::unexpected(InvalidArgument{"split 宿主 slot 不存在"});
        }
        auto eng =
            Engine::open({.dir = engine_dir_for(right.id), .fsync_on_commit = fsync_on_commit_});
        if (!eng.has_value()) {
            return tinykv::unexpected(eng.error());
        }
        if (auto r = migrate_range(*old_slot->engine, **eng, right); !r.has_value()) {
            return tinykv::unexpected(r.error());
        }
        auto peer = raftstore::Peer::open(raft::Config{.id = PeerId{right.id.value},
                                                       .voters = {PeerId{right.id.value}},
                                                       .election_timeout = election_,
                                                       .heartbeat_interval = heartbeat_},
                                          **eng);
        if (!peer.has_value()) {
            return tinykv::unexpected(peer.error());
        }
        (*peer)->set_region(right);
        (*peer)->set_store_meta(&meta_);
        // BUG-0022（code-review r2 N-4）：这里绝不能再泵（tick 推举/on_ready）。
        // observer 回调可能发生在分区闸泵路径（admin_split 超时后残留 split 在
        // pump_peer_only 里 apply），此时只持旧 region 的桶；若在此推举新 peer，
        // 另一 HTTP 线程可同时持新 region 桶触碰同一个新 Peer 对象——registry 的
        // mu_ 只保护 slots_ 表结构，不保护 Peer 本体。只注册：选举交给持全局闸的
        // Worker，或新 region 首次分区闸内泵（pump_peer_only），或 admin_split
        // 成功路径在全局闸下的补泵——三者都保证"触碰新 peer 时持其桶或全局闸"。
        install(right, std::move(*eng), std::move(*peer));
        return {};
    }

    // 关闭全部引擎（逆序：peer 先析构，engine 后 close）。stop 后无并发请求，mu_ 仍按惯例持有。
    [[nodiscard]] result<void> close_all() {
        std::lock_guard lock(mu_);
        std::string first_err;
        for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
            it->second.peer.reset();
            if (auto r = it->second.engine->close(); !r.has_value() && first_err.empty()) {
                first_err = std::string(tinykv::describe_kind(r.error()));
            }
            it->second.engine.reset();
        }
        slots_.clear();
        if (!first_err.empty()) {
            return tinykv::unexpected(IoFailed{.path = "peer-registry", .reason = first_err});
        }
        return {};
    }

    // 选举所有 peer（单节点组：若干 tick 即 leader）；启动/新 peer 建好后调用
    void campaign_all() {
        std::lock_guard lock(mu_);
        for (auto& [id, slot] : slots_) {
            (void)id;
            for (int i = 0; i < 20 && !slot.peer->is_leader(); ++i) {
                slot.peer->tick();
            }
            (void)slot.peer->on_ready();
        }
    }

    // 泵一轮：每个 peer tick + on_ready（Worker 与 executor 共用外部闸）
    void pump_all() {
        std::lock_guard lock(mu_);
        for (auto& [id, slot] : slots_) {
            (void)id;
            slot.peer->tick();
            (void)slot.peer->on_ready();
        }
    }

private:
    PeerSlot* find_slot(RegionId id) {
        auto it = slots_.find(id);
        return it == slots_.end() ? nullptr : &it->second;
    }

    void install(const raftstore::Region& region, std::unique_ptr<Engine> eng,
                 std::unique_ptr<raftstore::Peer> peer) {
        PeerSlot slot;
        slot.region_id = region.id;
        slot.region = region;
        slot.engine = std::move(eng);
        slot.peer = std::move(peer);
        if (observer_factory_) {
            observer_factory_(*slot.peer);
        }
        slots_.emplace(region.id, std::move(slot));
    }

    // 把 src 中落在 region 区间的 MVCC 条目搬到 dst：三列族各自扫描，
    // 跳过 raft log 内部键（\0 前缀），user key = 存储键去掉 8 字节 ts 后缀
    // （Lock CF 无后缀，直接是 user key）。
    [[nodiscard]] result<void> migrate_range(Storage& src, Storage& dst,
                                             const raftstore::Region& region) {
        std::vector<Modify> copies;
        std::vector<Modify> removes;
        for (const Cf cf : {Cf::Default, Cf::Write, Cf::Lock}) {
            if (auto r = scan_cf(src, cf, region, copies, removes); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
        }
        if (!copies.empty()) {
            if (auto r = dst.write(copies); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
        }
        if (!removes.empty()) {
            if (auto r = src.write(removes); !r.has_value()) {
                return tinykv::unexpected(r.error());
            }
        }
        return {};
    }

    [[nodiscard]] result<void> scan_cf(Storage& src, Cf cf, const raftstore::Region& region,
                                       std::vector<Modify>& copies,
                                       std::vector<Modify>& removes) const {
        auto reader = src.create_reader();
        if (!reader.has_value()) {
            return tinykv::unexpected(reader.error());
        }
        auto it = (*reader)->scan(cf, tinykv::span<const std::byte>{}, kScanLimit);
        if (!it.has_value()) {
            return tinykv::unexpected(it.error());
        }
        for (; (*it)->valid(); (*it)->next()) {
            const std::string stored((*it)->key());
            if (stored.empty()) {
                continue;
            }
            // BUG-0004: raw 键与 MVCC 键在 Default CF 共存，必须先按保留前缀分派，
            // 再判 user key 归属；否则剥掉 8B ts 会把 raw 长键误判到左 region 而静默丢失。
            std::string user;
            if (tinykv::starts_with(stored, kRawKeyPrefix)) {
                user = stored.substr(kRawKeyPrefix.size());
            } else if (stored[0] != '\0') {
                user = user_key_of(cf, stored);
            } else {
                continue;  // raft log 内部键（\0re / \0lm）不迁移
            }
            if (!region.contains(user)) {
                continue;
            }
            copies.push_back(
                Modify{Put{.cf = cf, .key = stored, .value = std::string((*it)->value())}});
            removes.push_back(Modify{Delete{.cf = cf, .key = stored}});
        }
        return {};
    }

    [[nodiscard]] static std::string user_key_of(Cf cf, const std::string& stored) {
        if (cf == Cf::Lock || stored.size() < 8) {
            return stored;
        }
        return stored.substr(0, stored.size() - 8);  // Default/Write：末尾 8B 大端 ts
    }

    static constexpr std::size_t kScanLimit = 1U << 20U;  // Admin 低频，全量有界

    mutable std::mutex mu_;  // 保护 slots_ 表结构（实例内部的 Peer/Engine 由 region 闸串行）
    std::filesystem::path data_dir_;
    raftstore::StoreMeta& meta_;
    std::uint64_t election_;
    std::uint64_t heartbeat_;
    PeerId replica_id_{1};
    std::vector<PeerId> voters_;
    bool fsync_on_commit_{false};
    ObserverFactory observer_factory_;
    std::map<RegionId, PeerSlot> slots_;
};

}  // namespace tinykv::server
