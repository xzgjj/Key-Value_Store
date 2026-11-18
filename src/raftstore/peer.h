// 文件用途：Peer——Raft 之上的状态机与提案入口：提案走共识、apply 落存储、按 client+seq 去重。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-04、UX-03）。
//
// 职责边界（与 SPEC-2 §2.2 对应）：
//   * 提案：把 RaftCmdRequest 序列化进日志条目（不是本地直接写库）；
//   * apply：只由 committed 条目驱动写存储——"写走共识"这条红线落在这一层；
//   * 去重：同 (client_id, seq) 的重试不得二次 apply，否则重试会变成重复写（删了又写回）。
//     M2 采用"每 client 记录已 apply 的最大 seq"，比缓存逐条回执省内存，且对顺序重试足够。
//   * 读：M2 为 leader 本地读（未做 ReadIndex，读可能略旧），M3 用 ReadIndex 收口——这是已知限制。

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <raft_cmdpb.pb.h>

#include "common/error.h"
#include "raft/raft.h"
#include <kv.pb.h>
#include "raftstore/region.h"
#include "txn/latch.h"
#include "txn/mvcc.h"
#include "storage/storage.h"

namespace tinykv::raftstore {

class Peer {
public:
    // cfg 的 voters 决定集群规模；storage 同时承载 raft log 与业务数据（M2 简化）
    [[nodiscard]] static result<std::unique_ptr<Peer>> open(const raft::Config& cfg, Storage& storage);

    // 提交一条 RaftCmdRequest；重复的 (client_id, seq) 直接返回成功而不产生新提案
    [[nodiscard]] result<void> propose(const cmdpb::RaftCmdRequest& req);

    // leader 本地读（M2 非线性一致，M3 由 ReadIndex 修正）
    [[nodiscard]] result<GetResult> get(Cf cf, std::string_view key) const;

    void tick();

    // 把本轮 Ready 里的已提交条目全部 apply；返回真正 apply 的条目数。
    // 出站消息不在此丢弃——收进 outbox 由 take_messages() 取走交给传输层。
    [[nodiscard]] result<std::size_t> on_ready();

    // 取走待发送消息（接网络前 on_ready 会把消息丢掉，那是 bug）
    [[nodiscard]] std::vector<raftpb::Message> take_messages();

    // 从传输层收到的消息交给状态机
    [[nodiscard]] result<void> step_message(const raftpb::Message& msg);

    // ---- M3 一致读（ReadIndex 必做 / LeaseRead 默认关） ----
    // 两阶段而非阻塞式：Peer 不持有传输层，不能自己泵消息；由调用方（Worker/测试驱动）
    // 在 begin 之后泵若干轮，再取 confirmed 位点，等 applied 追平后读状态机。
    [[nodiscard]] result<std::uint64_t> begin_read_index();

    // 取走已获 quorum 确认的读位点（可能为空）
    [[nodiscard]] std::vector<std::uint64_t> take_confirmed_reads();

    // 租约：默认关；开启且有效时可跳过 ReadIndex 直接读（零 RPC）
    void enable_lease(bool on);
    [[nodiscard]] bool lease_valid() const;

    [[nodiscard]] bool applied_at_least(std::uint64_t index) const { return applied_ >= index; }

    // 压缩日志（快照/测试用）：超过 committed 会被 RaftLog 拒绝
    [[nodiscard]] result<void> compact_log(LogIndex upto) { return log_->compact(upto); }

    // 直接读状态机（调用方保证 applied ≥ read_index，否则读到的可能不是线性一致点）
    [[nodiscard]] result<GetResult> read_directly(Cf cf, std::string_view key) const;

    // ---- 提案回执（集群写与 log 模式读的公共底座） ----
    // apply 时把结果按 (client_id, seq) 存进回执表，调用方泵若干轮后取走。
    // 不提供阻塞版本：Peer 不持有传输层，阻塞就得在状态机里起线程。
    [[nodiscard]] std::optional<cmdpb::RaftCmdResponse> take_reply(std::uint64_t client_id,
                                                                  std::uint64_t seq);

    // log 模式读：把 Get/Scan 作为提案走一遍共识，回执里带结果（PERF-03 的对比基线）
    [[nodiscard]] std::optional<cmdpb::RaftCmdResponse> take_read_reply(std::uint64_t client_id,
                                                                        std::uint64_t seq);

    [[nodiscard]] bool is_leader() const { return raft_->is_leader(); }
    [[nodiscard]] raft::Raft& raft() { return *raft_; }
    [[nodiscard]] std::uint64_t applied_seq(std::uint64_t client_id) const;

    // ---- M4 第①层：批量 propose（默认开启；关掉可作 batch=1 对比基线） ----
    // 攒批窗口内多条请求合成一次共识往返（Envelope），apply 解包逐条执行、回执不串。
    void enable_batching(bool on) { batching_ = on; }
    [[nodiscard]] bool batching_enabled() const { return batching_; }

    // ---- M4 第③层：Split 路由注入（可选；不设置则忽略 split admin，保持 M2 行为） ----
    // my_region：本 peer 所属 region 的当前元数据（split apply 时据此切分）
    void set_region(const Region& region) { my_region_ = region; }
    void set_store_meta(StoreMeta* meta) { store_meta_ = meta; }
    // split apply 成功后的宿主回调（左/右 region）：宿主据此动态建右半 peer + 迁移数据。
    // 在 apply 路径内调用，回调不得重入本 peer 的 pump/on_ready。
    using SplitObserver = std::function<void(const Region& left, const Region& right)>;
    void set_split_observer(SplitObserver cb) { split_observer_ = std::move(cb); }

    // ---- M4 第⑤层：快照分块大小（默认 4MB，SPEC-3 D3-3；测试调小以覆盖多块路径） ----
    void set_snapshot_chunk_bytes(std::size_t n) { snapshot_chunk_bytes_ = n == 0 ? 1 : n; }

    // ---- M5 第⑤层：事务命令进 raft 提案（SPEC-4 架构：Latch → MvccTxn → raft） ----
    // 提案载荷 = [0x2B][type:1B][kvpb 序列化]；apply 时经 execute_txn_payload 落三列族。
    // 返回 false = 非 leader（NotLeader 语义由调用方处理）。
    [[nodiscard]] bool propose_txn_prewrite(const kvpb::TxnPrewriteRequest& req);
    [[nodiscard]] bool propose_txn_commit(const kvpb::TxnCommitRequest& req);
    [[nodiscard]] bool propose_txn_rollback(const kvpb::TxnBatchRollbackRequest& req);
    [[nodiscard]] bool propose_txn_gc(const kvpb::TxnGCRequest& req);
    // apply 位点到达后取该条目的 txn 执行错误（nullopt = 成功；结构化，client 编排需要字段）
    [[nodiscard]] std::optional<KvError> take_txn_error(std::uint64_t index) {
        auto it = txn_errors_.find(index);
        if (it == txn_errors_.end()) {
            return std::nullopt;
        }
        return std::move(it->second);
    }
    // 只读事务操作直接走 leader 本地 MVCC（M2 leader 本地读同款偏差，记 notes-m5）
    [[nodiscard]] txn::MvccTxn mvcc() { return txn::MvccTxn(storage_); }
    [[nodiscard]] txn::LatchManager& txn_latches() { return txn_latches_; }

private:
    Peer(std::unique_ptr<raft::RaftLog> log, Storage& storage, const raft::Config& cfg);

    PeerId self_{0};

    [[nodiscard]] result<void> apply_entry(const raftpb::Entry& entry);
    [[nodiscard]] result<void> execute(const cmdpb::RaftCmdRequest& req);
    // M4 第③层：apply Split admin（校验开区间 + 更新 StoreMeta 路由；红线1/2）
    [[nodiscard]] result<void> apply_split_admin(const cmdpb::SplitRequest& split);
    // 把攒批队列打包成 Envelope 一次 propose；返回是否产生了提案
    [[nodiscard]] result<bool> flush_proposals();
    // M5 ⑤：txn 载荷执行（Latch → MvccTxn；失败记入 txn_errors_ 供 client 查询）
    [[nodiscard]] result<void> execute_txn_payload(const std::string& payload, std::uint64_t index);
    void fill_get_reply(const cmdpb::GetRequest& get, cmdpb::GetResponse* out);
    void fill_scan_reply(const cmdpb::ScanRequest& scan, cmdpb::ScanResponse* out);

    // ---- 快照（M2 单块全量；M4 第⑤层分块流） ----
    // 生成：遍历三个列族取全量存活键（墓碑已被 Scan 过滤，故快照天然是"当前状态"）
    [[nodiscard]] result<raftpb::SnapshotData> dump_snapshot() const;
    // 安装：先为"本地有、快照无"的键写墓碑，再写入快照内容——
    // 只覆盖不删除会让分叉日志留下的脏键永久残留
    [[nodiscard]] result<void> install_snapshot(const raftpb::SnapshotData& data);
    // 三个列族全量遍历（快照生成与安装共用同一扫描口径，避免两处口径漂移）
    [[nodiscard]] result<void> for_each_entry(
        const std::function<void(Cf, const std::string&, const std::string&)>& fn) const;

    // ---- M4 第⑤层：快照分块流（4MB + 流内序号 + 逐块 CRC + 窗口流控） ----
    // 发送状态：每 target 一份游标；blob 共享（dump 一次，多 target 不重复序列化）
    struct SnapshotSend {
        std::uint64_t index{0};
        std::uint64_t term{0};
        std::shared_ptr<const std::string> blob;
        std::uint32_t total{0};
        std::uint32_t next{0};   // 下一个待发 chunk_seq
        std::uint32_t ack{0};    // 已确认的最大 chunk_seq（含）
    };
    // 接收状态：按 (from) 缓存块，逐块 CRC；收齐后重组安装
    struct SnapshotRecv {
        std::uint64_t index{0};
        std::uint64_t term{0};
        std::uint32_t total{0};
        std::uint32_t received{0};
        std::vector<std::string> chunks;
        std::vector<bool> have;
    };
    // 为该 target 建立/刷新发送状态（dump_snapshot + 序列化 + 切块参数）
    void begin_snapshot_send(PeerId target);
    // 按窗口把在途额度内的块发出去（流控：next - ack < kSnapshotWindow）
    void pump_snapshot_sends();
    void send_snapshot_chunk(PeerId target, SnapshotSend& s);
    // 收一块：CRC 校验 → 入缓存 → 回 ACK → 收齐则重组安装
    [[nodiscard]] result<void> handle_snapshot_chunk(const raftpb::Message& msg);
    // 流控确认：推进 ack，下次 pump 继续发
    void handle_snapshot_ack(const raftpb::Message& msg);
    // 收齐后：按 seq 重组 blob → 解析 SnapshotData → 整表安装 → 重置日志指针
    [[nodiscard]] result<void> finish_snapshot_recv(PeerId from, SnapshotRecv& recv);

    std::map<std::uint64_t, SnapshotSend> snapshot_sends_;   // peer_id → 发送游标
    std::map<std::uint64_t, SnapshotRecv> snapshot_recvs_;   // from peer_id → 接收缓存
    std::size_t snapshot_chunk_bytes_{4U * 1024U * 1024U};   // 默认 4MB（SPEC-3 D3-3）

    std::unique_ptr<raft::RaftLog> log_;
    std::unique_ptr<raft::Raft> raft_;
    Storage& storage_;
    std::map<std::uint64_t, std::uint64_t> last_seq_;   // client_id → 已 apply 的最大 seq
    std::vector<raftpb::Message> outbox_;
    std::vector<std::uint64_t> confirmed_reads_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, cmdpb::RaftCmdResponse> replies_;
    std::uint64_t applied_{0};   // 已 apply 到的日志位点

    // 批量 propose：待攒批的已序列化请求 + 开关（默认开启）
    std::vector<std::string> pending_proposals_;
    bool batching_{true};

    // M4 第③层：Split 路由（可选注入；未注入时忽略 split admin）
    std::optional<Region> my_region_;
    StoreMeta* store_meta_{nullptr};
    SplitObserver split_observer_;
    txn::LatchManager txn_latches_;                                   // M5 ⑤：key 粒度闩（红线4）
    std::map<std::uint64_t, KvError> txn_errors_;                  // apply 位点 → 错误描述
};

}  // namespace tinykv::raftstore
