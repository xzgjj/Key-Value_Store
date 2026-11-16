// 文件用途：peer.h 的实现——提案序列化、apply 执行、按 client 去重。
// 归属：TinyKV-CPP（M2 RaftKV，FUNC-04）。

#include "raftstore/peer.h"

#include <utility>
#include <vector>

#include "common/error.h"

namespace tinykv::raftstore {
namespace {

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
    : log_(std::move(log)), storage_(storage) {
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
    return raft_->propose(std::move(payload));
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

result<void> Peer::step_message(const raftpb::Message& msg) { return raft_->step(msg); }

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
    auto ready = raft_->take_ready();
    for (auto& m : ready.messages) {
        outbox_.push_back(std::move(m));   // 交给传输层，不能丢
    }
    for (const auto idx : ready.confirmed_reads) {
        confirmed_reads_.push_back(idx);
    }
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

result<void> Peer::execute(const cmdpb::RaftCmdRequest& req) {
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

}  // namespace tinykv::raftstore
