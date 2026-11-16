// 文件用途：Raft 消息传输抽象——send 出去、poll 取回，单线程消费模型（SPEC-2 §2.3.1）。
// 归属：TinyKV-CPP（M2 RaftKV）。
//
// 为什么接口只有两个方法：Raft 状态机是纯线性的，它既不该等 IO 也不该被回调打断。
// 因此传输层只做两件事——把消息发出去、把到达的消息攒起来等人来取（poll）。
// 并发与线程全部关在实现里，状态机侧保持零锁。
//
// 快照通道（open_snapshot_stream）留到 M4：M2 的快照只有 compact，没有分块传输，
// 现在声明只会是个空实现，反而误导使用者以为可用。

#pragma once

#include <vector>

#include <raft.pb.h>

#include "common/error.h"
#include "common/ids.h"

namespace tinykv::raftstore {

class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // 发送失败是常态（对端未起、连接被拒）：调用方不应因此中止，Raft 会按心跳重传
    [[nodiscard]] virtual result<void> send(PeerId to, const raftpb::Message& msg) = 0;

    // 取出已到达的消息（非阻塞，取完即清空）
    [[nodiscard]] virtual std::vector<raftpb::Message> poll() = 0;
};

}  // namespace tinykv::raftstore
