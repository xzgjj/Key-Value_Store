// 文件用途：peer.h 的实现——提案序列化、批量 propose、apply 执行、按 client 去重。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-04；M4 第①层批量 propose）。

#include "raftstore/peer.h"

#include <utility>

#include "common/error_format.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "common/error_format.h"
#include <vector>

#include "common/crc32c.h"
#include "common/error.h"
#include "raft/entry_batch.h"

namespace tinykv::raftstore {

constexpr char kTxnMarker = '\x2B';           // RaftCmdRequest 起始字节只会是 0x0A/0x12/0x1A
constexpr char kTxnTypePrewrite = '\x01';
constexpr char kTxnTypeCommit = '\x02';
constexpr char kTxnTypeRollback = '\x03';
constexpr char kTxnTypeGC = '\x04';

namespace {

// M4 第⑤层：快照分块在途窗口（流控）——未确认块数上限，防止把接收端内存打爆
constexpr std::uint32_t kSnapshotWindow = 4;

// proto 列族 → 存储层列族（唯一转换点；两侧定义见 proto/common.proto 与 common/encode.h）
Cf to_cf(commonpb::CfName name) {
    switch (name) {
        case commonpb::CF_WRITE:
            return Cf::Write;
        case commonpb::CF_LOCK:
            return Cf::Lock;
        case commonpb::CF_DEFAULT:
        default:
            return Cf::Default;
    }
}

std::string as_string(const std::string& bytes) { return bytes; }

}  // namespace

Peer::Peer(std::unique_ptr<raft::RaftLog> log, Storage& storage, const raft::Config& cfg)
    : self_(cfg.id), log_(std::move(log)), storage_(storage) {   // 顺序须与声明一致
    raft_ = std::make_unique<raft::Raft>(cfg, *log_);
}

result<std::unique_ptr<Peer>> Peer::open(const raft::Config& cfg, Storage& storage) {
    auto log = raft::RaftLog::open(storage);
    if (!log) {
        return std::unexpected(log.error());
    }
    // 构造函数私有：用 unique_ptr 立即接管（非数组 new，ENG-04 只禁裸数组）
    return std::unique_ptr<Peer>(new Peer(std::move(*log), storage, cfg));
}

std::uint64_t Peer::applied_seq(std::uint64_t client_id) const {
    const auto it = last_seq_.find(client_id);
    return it == last_seq_.end() ? 0 : it->second;
}

result<void> Peer::propose(const cmdpb::RaftCmdRequest& req) {
    const std::uint64_t client_id = req.header().client_id();
    const std::uint64_t seq = req.header().sequence_id();
    // 去重：重试不得二次 apply（否则"删了又写回"这类问题会出现）
    if (client_id != 0 && seq != 0 && seq <= applied_seq(client_id)) {
        return {};
    }
    std::string payload;
    if (!req.SerializeToString(&payload)) {
        return std::unexpected(Corrupted{.where = "peer:propose", .reason = "请求序列化失败"});
    }
    if (batching_) {
        // M4 第①层：攒批，等 on_ready 时合并成一次共识往返（Envelope）。
        // 必须先查 role：入队绕过了 raft_->propose 的 NotLeader 检查，非 leader 要在此挡住，
        // 否则非 leader 会"假成功"接收提案（raft_kv_test 的非 leader 拒绝语义会被破坏）。
        if (!raft_->is_leader()) {
            return std::unexpected(
                NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
        }
        pending_proposals_.push_back(std::move(payload));
        return {};
    }
    return raft_->propose(std::move(payload));
}

result<bool> Peer::flush_proposals() {
    if (pending_proposals_.empty()) {
        return false;
    }
    std::vector<std::string> batch;
    batch.swap(pending_proposals_);
    // 单条也走 Envelope（格式统一、解包路径单一）；仅一条时不额外增加往返
    if (auto r = raft_->propose(raft::pack_entries(batch)); !r) {
        return std::unexpected(r.error());
    }
    return true;
}

result<GetResult> Peer::get(Cf cf, std::string_view key) const {
    if (!raft_->is_leader()) {
        return std::unexpected(NotLeader{.region_id = RegionId{0}, .leader_hint = std::nullopt});
    }
    return storage_.get(cf, std::as_bytes(std::span{key.data(), key.size()}));
}

void Peer::tick() { raft_->tick(); }

std::vector<raftpb::Message> Peer::take_messages() {
    // 只取 outbox，**不要**再调用 take_ready()：那会顺手清掉待 apply 队列与已确认读位点，
    // 一旦调用方先 take_messages 再 on_ready（泵的常见顺序），数据就丢了。
    std::vector<raftpb::Message> out;
    out.swap(outbox_);
    return out;
}

result<void> Peer::step_message(const raftpb::Message& msg) {
    // M4 第⑤层：分块快照在 Peer 层收发（状态机不碰存储）
    if (msg.id() == raftpb::MSG_SNAPSHOT_CHUNK) {
        return handle_snapshot_chunk(msg);
    }
    if (msg.id() == raftpb::MSG_SNAPSHOT_ACK) {
        handle_snapshot_ack(msg);
        return {};
    }
    if (msg.id() != raftpb::MSG_SNAPSHOT) {
        return raft_->step(msg);
    }
    // 快照安装是应用层动作（要写存储），状态机只负责重置日志指针
    if (msg.term() < raft_->term().value) {
        return {};   // 过期快照直接丢弃
    }
    raftpb::SnapshotData data;
    if (!data.ParseFromString(msg.snapshot_data())) {
        return std::unexpected(Corrupted{.where = "peer:snapshot", .reason = "快照载荷解析失败"});
    }
    if (auto r = install_snapshot(data); !r) {
        return r;
    }
    raft_->restore_snapshot(LogIndex{msg.index()}, Term{msg.log_term()});

    // 回执让 leader 知道该 follower 已到快照位点，可以接着发增量
    raftpb::Message reply;
    reply.set_id(raftpb::MSG_APPEND_RESPONSE);
    reply.set_term(raft_->term().value);
    reply.set_from(self_.value);
    reply.set_to(msg.from());
    reply.set_index(msg.index());
    reply.set_commit(msg.index());
    outbox_.push_back(std::move(reply));
    return {};
}

result<void> Peer::for_each_entry(
    const std::function<void(Cf, const std::string&, const std::string&)>& fn) const {
    constexpr std::size_t kAll = 1U << 24U;   // 单 region 全量；超限属 M4 分块范畴
    for (const Cf cf : {Cf::Default, Cf::Write, Cf::Lock}) {
        auto reader = storage_.create_reader();
        if (!reader) {
            return std::unexpected(reader.error());
        }
        auto it = (*reader)->scan(cf, {}, kAll);
        if (!it) {
            return std::unexpected(it.error());
        }
        for (; (**it).valid(); (**it).next()) {
            fn(cf, std::string((**it).key()), std::string((**it).value()));
        }
    }
    return {};
}

result<raftpb::SnapshotData> Peer::dump_snapshot() const {
    raftpb::SnapshotData data;
    auto walk = for_each_entry([&](Cf cf, const std::string& key, const std::string& value) {
        auto* kv = data.add_kvs();
        kv->set_cf(static_cast<std::uint32_t>(cf));
        kv->set_key(key);
        kv->set_value(value);
    });
    if (!walk) {
        return std::unexpected(walk.error());
    }
    return data;
}

result<void> Peer::install_snapshot(const raftpb::SnapshotData& data) {
    ModifyVec batch;
    std::map<std::pair<std::uint32_t, std::string>, bool> incoming;
    for (const raftpb::SnapshotKv& kv : data.kvs()) {
        incoming[{kv.cf(), kv.key()}] = true;
        batch.push_back(Modify{Put{.cf = static_cast<Cf>(kv.cf()),
                                   .key = kv.key(),
                                   .value = kv.value()}});
    }
    // 删除本地有、快照无的键：只覆盖不删除，分叉日志留下的脏键会永久残留
    auto walk = for_each_entry([&](Cf cf, const std::string& key, const std::string&) {
        if (incoming.find({static_cast<std::uint32_t>(cf), key}) == incoming.end()) {
            batch.push_back(Modify{Delete{.cf = cf, .key = key}});
        }
    });
    if (!walk) {
        return std::unexpected(walk.error());
    }
    if (batch.empty()) {
        return {};
    }
    return storage_.write(batch);
}

std::vector<std::uint64_t> Peer::take_confirmed_reads() {
    std::vector<std::uint64_t> out;
    out.swap(confirmed_reads_);
    return out;
}

result<std::uint64_t> Peer::begin_read_index() { return raft_->read_index(); }

void Peer::enable_lease(bool on) { raft_->enable_lease(on); }

bool Peer::lease_valid() const { return raft_->lease_valid(); }

result<GetResult> Peer::read_directly(Cf cf, std::string_view key) const {
    return storage_.get(cf, std::as_bytes(std::span{key.data(), key.size()}));
}

result<std::size_t> Peer::on_ready() {
    // M4 第①层：先把窗口内攒批的请求合成一次 propose，再取 Ready 推进共识
    if (batching_) {
        if (auto r = flush_proposals(); !r) {
            return std::unexpected(r.error());
        }
    }
    auto ready = raft_->take_ready();
    for (auto& m : ready.messages) {
        outbox_.push_back(std::move(m));   // 交给传输层，不能丢
    }
    for (const auto idx : ready.confirmed_reads) {
        confirmed_reads_.push_back(idx);
    }
    // M4 第⑤层：快照按 4MB 分块 + 逐块 CRC + 窗口流控发送（替代 M2 单块全量）
    if (!ready.snapshot_targets.empty()) {
        for (const PeerId& target : ready.snapshot_targets) {
            begin_snapshot_send(target);
        }
    }
    pump_snapshot_sends();
    std::size_t applied = 0;
    for (const raftpb::Entry& entry : ready.to_apply) {
        if (auto r = apply_entry(entry); !r) {
            return std::unexpected(r.error());
        }
        ++applied;
        applied_ = entry.index();   // 已 apply 位点：ReadIndex 放行判据
    }
    return applied;
}

result<void> Peer::apply_entry(const raftpb::Entry& entry) {
    if (entry.data().empty()) {
        return {};   // 空条目（leader 占位）无需执行
    }
    // M5 ⑤：txn 载荷（[0x2B][type][kvpb]）走独立执行路径——cmdpb 反序列化对它必然失败
    if (entry.data()[0] == kTxnMarker) {
        return execute_txn_payload(entry.data(), entry.index());
    }
    // M4 第①层：Envelope 解包后逐条 execute——每条用各自 header 的 (client,seq) 登记回执，
    // 批量不串回执；解包失败说明是旧的单条 protobuf data，走原路径（向后兼容崩溃重放）。
    std::vector<std::string> items;
    if (raft::try_unpack_entries(entry.data(), items)) {
        for (const auto& data : items) {
            cmdpb::RaftCmdRequest req;
            if (!req.ParseFromString(data)) {
                return std::unexpected(
                    Corrupted{.where = "peer:apply-batch", .reason = "批内命令反序列化失败"});
            }
            if (auto r = execute(req); !r) {
                return r;
            }
        }
        return {};
    }
    cmdpb::RaftCmdRequest req;
    if (!req.ParseFromString(entry.data())) {
        return std::unexpected(Corrupted{.where = "peer:apply", .reason = "命令反序列化失败"});
    }
    return execute(req);
}

void Peer::fill_get_reply(const cmdpb::GetRequest& get, cmdpb::GetResponse* out) {
    // log 模式读：读也在日志里定序，apply 时才真正取数
    auto got = storage_.get(to_cf(get.cf()),
                            std::as_bytes(std::span{get.key().data(), get.key().size()}));
    if (got.has_value() && got->value.has_value()) {
        out->set_found(true);
        out->set_value(*got->value);
    }
}

void Peer::fill_scan_reply(const cmdpb::ScanRequest& scan, cmdpb::ScanResponse* out) {
    if (scan.limit() == 0) {
        return;
    }
    auto reader = storage_.create_reader();
    if (!reader.has_value()) {
        return;
    }
    auto it = (*reader)->scan(Cf::Default,
                              std::as_bytes(std::span{scan.start_key().data(),
                                                      scan.start_key().size()}),
                              scan.limit());
    if (!it.has_value()) {
        return;
    }
    for (; (**it).valid(); (**it).next()) {
        out->add_keys(std::string((**it).key()));
        out->add_values(std::string((**it).value()));
    }
}

// ---- M5 ⑤：事务命令（SPEC-4 架构：Latch → MvccTxn → raft 提案；apply 时落三列族） ----

bool Peer::propose_txn_prewrite(const kvpb::TxnPrewriteRequest& req) {
    std::string payload(1, kTxnMarker);
    payload.push_back(kTxnTypePrewrite);
    payload += req.SerializeAsString();
    return raft_->propose(std::move(payload)).has_value();
}

bool Peer::propose_txn_commit(const kvpb::TxnCommitRequest& req) {
    std::string payload(1, kTxnMarker);
    payload.push_back(kTxnTypeCommit);
    payload += req.SerializeAsString();
    return raft_->propose(std::move(payload)).has_value();
}

bool Peer::propose_txn_rollback(const kvpb::TxnBatchRollbackRequest& req) {
    std::string payload(1, kTxnMarker);
    payload.push_back(kTxnTypeRollback);
    payload += req.SerializeAsString();
    return raft_->propose(std::move(payload)).has_value();
}

bool Peer::propose_txn_gc(const kvpb::TxnGCRequest& req) {
    std::string payload(1, kTxnMarker);
    payload.push_back(kTxnTypeGC);
    payload += req.SerializeAsString();
    return raft_->propose(std::move(payload)).has_value();
}

namespace {

// txn 载荷各类型的执行体（execute_txn_payload 只做分派；错误以 optional 回传）
std::optional<KvError> run_txn_prewrite(Storage& storage, txn::LatchManager& latches,
                                        const std::string& body) {
    kvpb::TxnPrewriteRequest req;
    if (!req.ParseFromString(body)) {
        return InvalidArgument{"txn prewrite 反序列化失败"};
    }
    std::vector<std::string> keys;
    std::vector<txn::Mutation> muts;
    for (const auto& m : req.mutations()) {
        keys.emplace_back(m.key());
        muts.push_back(txn::Mutation{
            .op = m.op() == kvpb::Mutation::PUT ? txn::Mutation::Op::kPut
                                                : txn::Mutation::Op::kDel,
            .key = m.key(), .value = m.value()});
    }
    auto guard = latches.acquire(keys);   // 红线4：latch 持有至 apply 完成
    txn::MvccTxn mvcc(storage);
    if (auto r = mvcc.prewrite(muts, req.primary_lock(), Ts{req.start_ts()}, req.lock_ttl(), 0);
        !r.has_value()) {
        return std::move(r.error());
    }
    return std::nullopt;
}

std::optional<KvError> run_txn_commit(Storage& storage, txn::LatchManager& latches,
                                      const std::string& body) {
    kvpb::TxnCommitRequest req;
    if (!req.ParseFromString(body)) {
        return InvalidArgument{"txn commit 反序列化失败"};
    }
    std::vector<std::string> keys(req.keys().begin(), req.keys().end());
    auto guard = latches.acquire(keys);
    txn::MvccTxn mvcc(storage);
    if (auto r = mvcc.commit(keys, Ts{req.start_ts()}, Ts{req.commit_ts()}); !r.has_value()) {
        return std::move(r.error());
    }
    return std::nullopt;
}

std::optional<KvError> run_txn_rollback(Storage& storage, txn::LatchManager& latches,
                                        const std::string& body) {
    kvpb::TxnBatchRollbackRequest req;
    if (!req.ParseFromString(body)) {
        return InvalidArgument{"txn rollback 反序列化失败"};
    }
    std::vector<std::string> keys(req.keys().begin(), req.keys().end());
    auto guard = latches.acquire(keys);
    txn::MvccTxn mvcc(storage);
    if (auto r = mvcc.rollback(keys, Ts{req.start_ts()}); !r.has_value()) {
        return std::move(r.error());
    }
    return std::nullopt;
}

std::optional<KvError> run_txn_gc(Storage& storage, const std::string& body) {
    kvpb::TxnGCRequest req;
    if (!req.ParseFromString(body)) {
        return InvalidArgument{"txn gc 反序列化失败"};
    }
    txn::MvccTxn mvcc(storage);
    if (auto r = mvcc.gc(Ts{req.safe_point()}); !r.has_value()) {
        return std::move(r.error());
    }
    return std::nullopt;
}

}  // namespace

result<void> Peer::execute_txn_payload(const std::string& payload, std::uint64_t index) {
    const char type = payload[1];
    const std::string body = payload.substr(2);
    std::optional<KvError> err;
    switch (type) {
        case kTxnTypePrewrite:
            err = run_txn_prewrite(storage_, txn_latches_, body);
            break;
        case kTxnTypeCommit:
            err = run_txn_commit(storage_, txn_latches_, body);
            break;
        case kTxnTypeRollback:
            err = run_txn_rollback(storage_, txn_latches_, body);
            break;
        case kTxnTypeGC:
            err = run_txn_gc(storage_, body);
            break;
        default:
            err = InvalidArgument{"未知 txn 载荷类型"};
            break;
    }
    if (err.has_value()) {
        txn_errors_[index] = std::move(*err);   // nullopt = 成功；client 泵到位点后查询
    }
    return {};
}

result<void> Peer::apply_split_admin(const cmdpb::SplitRequest& split) {
    if (!my_region_.has_value() || store_meta_ == nullptr) {
        return std::unexpected(InvalidArgument{"split 需要注入 region 元数据与 StoreMeta"});
    }
    const auto ep = static_cast<std::uint64_t>(split.new_region_id());
    auto parts = split_region(*my_region_, split.split_key(), RegionId{ep});
    if (!parts) {
        return std::unexpected(parts.error());
    }
    if (auto r = store_meta_->replace_region(my_region_->id, parts->first, parts->second); !r) {
        return r;
    }
    // 红线2：apply 完成后立即更新本 peer 视图（左半仍由本 peer 负责）
    my_region_ = parts->first;
    if (split_observer_) {
        split_observer_(parts->first, parts->second);
    }
    return {};
}

result<void> Peer::execute(const cmdpb::RaftCmdRequest& req) {
    // M4 第③层：admin 与 requests 互斥（proto 约定）；有 admin 时先执行它
    if (req.has_admin()) {
        if (req.requests_size() != 0) {
            return std::unexpected(InvalidArgument{"admin 与 requests 互斥，不得共存"});
        }
        if (req.admin().cmd_case() == cmdpb::AdminRequest::kSplit) {
            if (auto r = apply_split_admin(req.admin().split()); !r) {
                return r;
            }
        } else {
            return std::unexpected(InvalidArgument{"该 admin 命令本阶段未实现"});
        }
    }
    ModifyVec batch;
    cmdpb::RaftCmdResponse resp;   // 与 requests 顺序一一对应的回执
    resp.mutable_header()->set_region_id(req.header().region_id());
    batch.reserve(static_cast<std::size_t>(req.requests_size()));

    for (const cmdpb::Request& one : req.requests()) {
        switch (one.cmd_case()) {
            case cmdpb::Request::kPut: {
                const auto& put = one.put();
                batch.push_back(Modify{Put{.cf = to_cf(put.cf()),
                                           .key = as_string(put.key()),
                                           .value = as_string(put.value())}});
                resp.add_cmd_responses()->mutable_put();
                break;
            }
            case cmdpb::Request::kDelete: {
                const auto& del = one.delete_();
                batch.push_back(Modify{Delete{.cf = to_cf(del.cf()), .key = as_string(del.key())}});
                resp.add_cmd_responses()->mutable_delete_();
                break;
            }
            case cmdpb::Request::kGet:
                fill_get_reply(one.get(), resp.add_cmd_responses()->mutable_get());
                break;
            case cmdpb::Request::kScan:
                fill_scan_reply(one.scan(), resp.add_cmd_responses()->mutable_scan());
                break;
            case cmdpb::Request::kReadIndex:
            case cmdpb::Request::CMD_NOT_SET:
                return std::unexpected(InvalidArgument{"ReadIndex 不进日志；空命令视为契约误用"});
            default:
                return std::unexpected(InvalidArgument{"未知命令类型"});
        }
    }
    if (!batch.empty()) {
        if (auto r = storage_.write(batch); !r) {
            return r;
        }
    }
    const std::uint64_t client_id = req.header().client_id();
    if (client_id != 0) {
        last_seq_[client_id] = req.header().sequence_id();
        replies_[{client_id, req.header().sequence_id()}] = std::move(resp);
    }
    return {};
}

std::optional<cmdpb::RaftCmdResponse> Peer::take_reply(std::uint64_t client_id,
                                                      std::uint64_t seq) {
    const auto it = replies_.find({client_id, seq});
    if (it == replies_.end()) {
        return std::nullopt;
    }
    auto out = std::move(it->second);
    replies_.erase(it);
    return out;
}

std::optional<cmdpb::RaftCmdResponse> Peer::take_read_reply(std::uint64_t client_id,
                                                            std::uint64_t seq) {
    return take_reply(client_id, seq);   // log 模式读与写共用回执表，语义一致
}

// ---- M4 第⑤层：快照分块流 ----

void Peer::begin_snapshot_send(PeerId target) {
    // 已有发送任务就沿用：snapshot_targets 可能每轮都出现（follower 未追平前），
    // 若每次重建会把窗口游标重置回 0，后面的块永远发不出去（分块流卡死）。
    if (snapshot_sends_.find(target.value) != snapshot_sends_.end()) {
        return;
    }
    auto data = dump_snapshot();
    if (!data.has_value()) {
        return;
    }
    auto blob = std::make_shared<std::string>();
    if (!data->SerializeToString(blob.get())) {
        return;
    }
    const std::size_t chunk = snapshot_chunk_bytes_ == 0 ? 1 : snapshot_chunk_bytes_;
    const auto total = static_cast<std::uint32_t>((blob->size() + chunk - 1) / chunk);
    SnapshotSend s;
    s.index = raft_->last_index().value;
    s.term = raft_->term().value;
    s.blob = std::shared_ptr<const std::string>(blob);
    s.total = total == 0 ? 1 : total;
    snapshot_sends_[target.value] = std::move(s);
}

void Peer::pump_snapshot_sends() {
    for (auto it = snapshot_sends_.begin(); it != snapshot_sends_.end();) {
        auto& s = it->second;
        // 流控：在途块数 next-ack 不得超过窗口
        while (s.next < s.total && (s.next - s.ack) < kSnapshotWindow) {
            send_snapshot_chunk(PeerId{it->first}, s);
        }
        if (s.ack >= s.total) {   // 全部确认：清理
            it = snapshot_sends_.erase(it);
        } else {
            ++it;
        }
    }
}

void Peer::send_snapshot_chunk(PeerId target, SnapshotSend& s) {
    const std::uint32_t seq = s.next;
    const std::size_t off = static_cast<std::size_t>(seq) * snapshot_chunk_bytes_;
    const std::size_t len = std::min(snapshot_chunk_bytes_, s.blob->size() - off);
    const std::string_view piece(s.blob->data() + off, len);

    raftpb::SnapshotChunk chunk;
    chunk.set_snapshot_index(s.index);
    chunk.set_snapshot_term(s.term);
    chunk.set_chunk_seq(seq);
    chunk.set_total_chunks(s.total);
    chunk.set_data(std::string(piece));
    chunk.set_crc32(crc32c(std::as_bytes(std::span{piece.data(), piece.size()})));

    std::string payload;
    if (!chunk.SerializeToString(&payload)) {
        return;
    }
    raftpb::Message msg;
    msg.set_id(raftpb::MSG_SNAPSHOT_CHUNK);
    msg.set_term(raft_->term().value);
    msg.set_from(self_.value);
    msg.set_to(target.value);
    msg.set_snapshot_data(payload);
    outbox_.push_back(std::move(msg));
    ++s.next;
}

result<void> Peer::handle_snapshot_chunk(const raftpb::Message& msg) {
    raftpb::SnapshotChunk chunk;
    if (!chunk.ParseFromString(msg.snapshot_data())) {
        return std::unexpected(
            Corrupted{.where = "peer:snapshot-chunk", .reason = "分块载荷解析失败"});
    }
    // 红线：逐块 CRC 校验——坏块直接拒绝，绝不把半截/脏数据安装进存储
    const std::string& d = chunk.data();
    const auto got = crc32c(std::as_bytes(std::span{d.data(), d.size()}));
    if (got != chunk.crc32()) {
        return std::unexpected(Corrupted{.where = "peer:snapshot-chunk", .reason = "块 CRC 不匹配"});
    }
    const PeerId from{msg.from()};
    auto& recv = snapshot_recvs_[from.value];
    if (recv.index != chunk.snapshot_index() || recv.total != chunk.total_chunks()) {
        recv.index = chunk.snapshot_index();   // 新快照：重置缓存
        recv.term = chunk.snapshot_term();
        recv.total = chunk.total_chunks();
        recv.received = 0;
        recv.chunks.assign(recv.total, std::string{});
        recv.have.assign(recv.total, false);
    }
    const std::uint32_t seq = chunk.chunk_seq();
    if (seq >= recv.total) {
        return std::unexpected(
            InvalidArgument{"快照块序号越界（seq >= total_chunks）"});
    }
    if (!recv.have[seq]) {
        recv.chunks[seq] = chunk.data();
        recv.have[seq] = true;
        ++recv.received;
    }
    // 回 ACK 推进 leader 流控窗口（index = 本块 seq）
    raftpb::Message ack;
    ack.set_id(raftpb::MSG_SNAPSHOT_ACK);
    ack.set_term(raft_->term().value);
    ack.set_from(self_.value);
    ack.set_to(from.value);
    ack.set_index(seq);
    ack.mutable_snapshot()->set_index(recv.index);
    ack.mutable_snapshot()->set_term(recv.term);
    outbox_.push_back(std::move(ack));

    if (recv.received >= recv.total) {
        auto r = finish_snapshot_recv(from, recv);
        snapshot_recvs_.erase(from.value);
        return r;
    }
    return {};
}

void Peer::handle_snapshot_ack(const raftpb::Message& msg) {
    const auto it = snapshot_sends_.find(msg.from());
    if (it == snapshot_sends_.end() || it->second.index != msg.snapshot().index()) {
        return;   // 无发送状态或快照代号不符（旧 ACK）
    }
    const auto ack = static_cast<std::uint32_t>(msg.index()) + 1;
    if (ack > it->second.ack) {
        it->second.ack = ack;
    }
}

result<void> Peer::finish_snapshot_recv(PeerId from, SnapshotRecv& recv) {
    std::string blob;
    blob.reserve(recv.total * snapshot_chunk_bytes_);
    for (const auto& c : recv.chunks) {
        blob += c;
    }
    raftpb::SnapshotData data;
    if (!data.ParseFromString(blob)) {
        return std::unexpected(
            Corrupted{.where = "peer:snapshot-chunk", .reason = "重组快照解析失败"});
    }
    if (auto r = install_snapshot(data); !r) {
        return r;
    }
    raft_->restore_snapshot(LogIndex{recv.index}, Term{recv.term});
    // 回执：告诉 leader 已到快照位点，可接着发增量日志
    raftpb::Message reply;
    reply.set_id(raftpb::MSG_APPEND_RESPONSE);
    reply.set_term(raft_->term().value);
    reply.set_from(self_.value);
    reply.set_to(from.value);
    reply.set_index(recv.index);
    reply.set_commit(recv.index);
    outbox_.push_back(std::move(reply));
    return {};
}

}  // namespace tinykv::raftstore
