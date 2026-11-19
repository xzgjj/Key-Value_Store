// 文件用途：cli.h 的实现——参数解析、子命令执行、人类可读输出。
// 归属：TinyKV-CPP（M1 单机存储，UX-02/UX-05）。

#include "cli/cli.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cstddef>
#include "cli/http_client.h"
#include "common/error.h"
#include "common/error_format.h"
#include "config/config.h"
#include "server/raw.h"
#include "storage/engine.h"
#include "storage/storage.h"
namespace tinykv::cli {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRuntime = 2;
constexpr int kExitNotFound = 3;
constexpr const char* kDefaultConfig = "demo.yaml";
constexpr std::size_t kMaxScanLimit = 1000000;  // scan 一次最多返回的条数上限

// 已解析的全局参数
struct Globals {
    std::string config{kDefaultConfig};
    std::string endpoint;                 // 非空 = 集群模式
    std::string read_mode{"read_index"};  // read_index / lease / log
    Cf cf{Cf::Default};
    std::size_t limit{10};
};

std::string_view usage() {
    return "用法: tinykv-cli [--config <path>] <命令> [参数]\n"
           "\n"
           "命令:\n"
           "  put <key> <value>        写入（同 key 覆盖）\n"
           "  get <key>                读取；key 不存在时退出码 3\n"
           "  del <key>                删除（删除不存在的 key 视为成功）\n"
           "  scan [start]             按升序扫描，默认 limit 10\n"
           "  status                   打印配置与引擎统计\n"
           "  txn <子命令>             事务（M5；详见 txn --help）\n"
           "\n"
           "集群观测命令（需 --endpoint，docs/cli.md §10）：\n"
           "  regions                        列出该 store 全部 region\n"
           "  region <id>                    单个 region 详情\n"
           "  region peers <id>              副本分布\n"
           "  region split <id> <key>        手动触发 split（调试）\n"
           "  scheduler status               调度器状态\n"
           "  trace region <id> on|off       region 级 trace 开关\n"
           "\n"
           "选项:\n"
           "  --config <path>          配置文件路径（默认 demo.yaml）\n"
           "  --endpoint <host:port>   集群模式：status/get/put/del（scan 暂未开放）\n"
           "  --read-mode <m>          read_index（默认）/ lease / log，仅集群 get 生效\n"
           "  --cf <default|write|lock> 列族（默认 default）\n"
           "  --limit <n>              scan 返回条数上限\n"
           "  --help                   显示本帮助\n";
}

// query 值编码：key/session 里的空格、'&'、'=' 直接拼进 URL 会被服务端截断
std::string url_encode(std::string_view in) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::optional<Cf> parse_cf(std::string_view name) {
    if (name == "default") {
        return Cf::Default;
    }
    if (name == "write") {
        return Cf::Write;
    }
    if (name == "lock") {
        return Cf::Lock;
    }
    return std::nullopt;
}

// 应用一个全局选项；未知选项由调用方过滤，这里只处理已知项
result<void> apply_global(Globals& g, const std::string& name, const std::string& value) {
    if (name == "--config") {
        g.config = value;
    } else if (name == "--endpoint") {
        g.endpoint = value;
    } else if (name == "--cf") {
        const auto cf = parse_cf(value);
        if (!cf) {
            return tinykv::unexpected(InvalidArgument{"未知列族 '" + value + "'"});
        }
        g.cf = *cf;
    } else if (name == "--limit") {
        // code-review：std::stoul 对 "abc" 抛异常（main 未捕获 → terminate）、对 "-1" 回绕。
        // 统一 from_chars + 范围校验。
        std::size_t parsed = 0;
        const auto res =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (res.ec != std::errc{} || res.ptr != value.data() + value.size() || parsed == 0 ||
            parsed > kMaxScanLimit) {
            return tinykv::unexpected(InvalidArgument{"--limit 必须是 1..1000000 的整数"});
        }
        g.limit = parsed;
    } else if (name == "--read-mode") {
        if (value != "read_index" && value != "lease" && value != "log") {
            return tinykv::unexpected(InvalidArgument{"--read-mode 只能是 read_index/lease/log"});
        }
        g.read_mode = value;
    }
    return {};
}

// 抽出 --cf/--limit：返回剩余的位置参数
result<std::vector<std::string>> split_globals(std::vector<std::string> args, Globals& g) {
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--cf" || a == "--limit" || a == "--config" || a == "--endpoint" ||
            a == "--read-mode") {
            if (i + 1 >= args.size()) {
                return tinykv::unexpected(InvalidArgument{"选项 " + a + " 缺少取值"});
            }
            if (auto r = apply_global(g, a, args[++i]); !r) {
                return tinykv::unexpected(r.error());
            }
            continue;
        }
        positional.push_back(a);
    }
    return positional;
}

EngineOptions to_options(const Config& cfg) {
    EngineOptions opt;
    opt.dir = cfg.engine.dir;
    opt.fsync_on_commit = cfg.engine.fsync_on_commit;
    // 左移前先夹到 size_t 可表示范围（config 已限 1..65536MB，这里的夹取是防未来改上限）
    constexpr std::size_t kMaxShiftMb = std::numeric_limits<std::size_t>::max() >> 20U;
    opt.memtable_limit_bytes = std::min(cfg.engine.memtable_limit_mb, kMaxShiftMb) << 20U;
    opt.wal_segment_bytes = std::min(cfg.engine.wal_segment_mb, kMaxShiftMb) << 20U;
    return opt;
}

int cmd_put(RawServer& raw, const Globals& g, const std::vector<std::string>& pos,
            std::ostream& out, std::ostream& err) {
    if (pos.size() != 3) {
        err << "用法: put <key> <value>\n";
        return kExitUsage;
    }
    auto r = raw.put(g.cf, pos[1], pos[2]);
    if (!r) {
        err << "写入失败：" << describe_kind(r.error()) << "\n";
        return kExitRuntime;
    }
    out << "OK\n";
    return kExitOk;
}

int cmd_get(RawServer& raw, const Globals& g, const std::vector<std::string>& pos,
            std::ostream& out, std::ostream& err) {
    if (pos.size() != 2) {
        err << "用法: get <key>\n";
        return kExitUsage;
    }
    auto r = raw.get(g.cf, pos[1]);
    if (!r) {
        err << "读取失败：" << describe_kind(r.error()) << "\n";
        return kExitRuntime;
    }
    if (!r->value.has_value()) {
        err << "NOT_FOUND\n";
        return kExitNotFound;
    }
    out << *r->value << "\n";
    return kExitOk;
}

int cmd_del(RawServer& raw, const Globals& g, const std::vector<std::string>& pos,
            std::ostream& out, std::ostream& err) {
    if (pos.size() != 2) {
        err << "用法: del <key>\n";
        return kExitUsage;
    }
    auto r = raw.remove(g.cf, pos[1]);
    if (!r) {
        err << "删除失败：" << describe_kind(r.error()) << "\n";
        return kExitRuntime;
    }
    out << "OK\n";
    return kExitOk;
}

int cmd_scan(RawServer& raw, const Globals& g, const std::vector<std::string>& pos,
             std::ostream& out, std::ostream& err) {
    if (pos.size() > 2) {
        err << "用法: scan [start]\n";
        return kExitUsage;
    }
    const std::string start = pos.size() == 2 ? pos[1] : std::string{};
    auto r = raw.scan(g.cf, start, g.limit);
    if (!r) {
        err << "扫描失败：" << describe_kind(r.error()) << "\n";
        return kExitRuntime;
    }
    for (const auto& [key, value] : *r) {
        out << key << "\t" << value << "\n";
    }
    out << "(" << r->size() << " 行)\n";
    return kExitOk;
}

int cmd_status(const Config& cfg, const Globals& g, std::ostream& out) {
    out << "config : " << g.config << "\n"
        << "dir    : " << cfg.engine.dir << "\n"
        << "fsync  : " << (cfg.engine.fsync_on_commit ? "on" : "off") << "\n"
        << "memtable_limit_mb : " << cfg.engine.memtable_limit_mb << "\n"
        << "wal_segment_mb    : " << cfg.engine.wal_segment_mb << "\n";
    return kExitOk;
}

}  // namespace

// 集群模式：目前只开放只读 status；写需提案回调（M3 与 ReadIndex 同链路），明确拒绝而非假成功
// 集群写：制表符文本协议 → POST /kv；写入必须经共识，回执由服务端 apply 后给出
int cmd_cluster_write(const std::string& cmd, const std::vector<std::string>& pos,
                      const std::string& host, std::uint16_t port, std::ostream& out,
                      std::ostream& err) {
    const std::size_t want = cmd == "put" ? 3U : 2U;
    if (pos.size() != want) {
        err << "用法: " << cmd << (cmd == "put" ? " <key> <value>\n" : " <key>\n");
        return kExitUsage;
    }
    const std::string body = cmd == "put" ? "put\t" + pos[1] + "\t" + pos[2] : "del\t" + pos[1];
    auto resp = http_post(host, port, "/kv", body);
    if (!resp) {
        err << "写入失败：" << describe(resp.error()) << "\n";
        return kExitRuntime;
    }
    if (resp->rfind("ERR", 0) == 0) {
        err << *resp << "\n";
        return kExitRuntime;
    }
    out << *resp << "\n";
    return kExitOk;
}

// ---- M4 ⑦b：region/scheduler/trace 观测命令（docs/cli.md §10，契约 2026-11-16 核对） ----

// <id>：十进制正整数；非法格式按用法错误退出（§10.2）
bool parse_region_id(const std::string& s, std::uint64_t& id) {
    if (s.empty() || s.size() > 18) {
        return false;
    }
    for (const char ch : s) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    id = std::stoull(s);
    return id > 0;
}

// 解析 --endpoint host:port；失败输出用法错误返回 false
bool parse_endpoint(const Globals& g, std::string& host, std::uint16_t& port, std::ostream& err) {
    const auto colon = g.endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= g.endpoint.size()) {
        err << "--endpoint 需为 host:port\n";
        return false;
    }
    const auto port_text = g.endpoint.substr(colon + 1);
    unsigned parsed = 0;
    const auto res =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
    if (res.ec != std::errc{} || res.ptr != port_text.data() + port_text.size() || parsed == 0 ||
        parsed > 65535) {
        err << "--endpoint 端口必须是 1..65535 的整数\n";
        return false;
    }
    host = g.endpoint.substr(0, colon);
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

// GET 一条观测路径：空正文 = 404（region 不存在/端点未装配）→ 退出码 3
int cmd_cluster_observe(const Globals& g, const std::string& path, std::ostream& out,
                        std::ostream& err) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(g, host, port, err)) {
        return kExitUsage;
    }
    auto body = http_get(host, port, path);
    if (!body) {
        err << "请求 " << path << " 失败：" << describe(body.error()) << "\n";
        return kExitRuntime;
    }
    if (body->empty()) {
        err << "NOT_FOUND\n";
        return kExitNotFound;
    }
    out << *body;
    if (body->back() != '\n') {
        out << "\n";
    }
    return kExitOk;
}

// POST 一条管理路径：ERR 前缀 → 运行错误（服务端拒绝原因原样透出）
int cmd_cluster_admin(const Globals& g, const std::string& path, const std::string& body,
                      std::ostream& out, std::ostream& err) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(g, host, port, err)) {
        return kExitUsage;
    }
    auto resp = http_post(host, port, path, body);
    if (!resp) {
        err << "请求 " << path << " 失败：" << describe(resp.error()) << "\n";
        return kExitRuntime;
    }
    if (resp->rfind("ERR", 0) == 0) {
        err << *resp << "\n";
        return kExitRuntime;
    }
    out << *resp;
    if (resp->back() != '\n') {
        out << "\n";
    }
    return kExitOk;
}

int cmd_regions(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
                std::ostream& err) {
    if (pos.size() != 1) {
        err << "用法: regions\n";
        return kExitUsage;
    }
    return cmd_cluster_observe(g, "/regions", out, err);
}

int cmd_region(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
               std::ostream& err) {
    // region <id> / region peers <id> / region split <id> <key>
    if (pos.size() >= 2 && pos[1] == "peers") {
        std::uint64_t id = 0;
        if (pos.size() != 3 || !parse_region_id(pos[2], id)) {
            err << "用法: region peers <id>\n";
            return kExitUsage;
        }
        return cmd_cluster_observe(g, "/region/peers?id=" + std::to_string(id), out, err);
    }
    if (pos.size() >= 2 && pos[1] == "split") {
        std::uint64_t id = 0;
        if (pos.size() != 4 || !parse_region_id(pos[2], id) || pos[3].empty()) {
            err << "用法: region split <id> <key>\n";
            return kExitUsage;
        }
        return cmd_cluster_admin(g, "/region/split", "split\t" + std::to_string(id) + "\t" + pos[3],
                                 out, err);
    }
    std::uint64_t id = 0;
    if (pos.size() != 2 || !parse_region_id(pos[1], id)) {
        err << "用法: region <id> | region peers <id> | region split <id> <key>\n";
        return kExitUsage;
    }
    return cmd_cluster_observe(g, "/region?id=" + std::to_string(id), out, err);
}

int cmd_scheduler_status(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
                         std::ostream& err) {
    if (pos.size() != 2 || pos[1] != "status") {
        err << "用法: scheduler status\n";
        return kExitUsage;
    }
    return cmd_cluster_observe(g, "/scheduler/status", out, err);
}

int cmd_trace_region(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
                     std::ostream& err) {
    // trace region <id> on|off（开关状态在 store 侧，故同样需要 --endpoint）
    if (pos.size() != 4 || pos[1] != "region" || (pos[3] != "on" && pos[3] != "off")) {
        err << "用法: trace region <id> on|off\n";
        return kExitUsage;
    }
    std::uint64_t id = 0;
    if (!parse_region_id(pos[2], id)) {
        err << "用法: trace region <id> on|off（<id> 须为十进制正整数）\n";
        return kExitUsage;
    }
    return cmd_cluster_admin(g, "/trace", "trace\t" + std::to_string(id) + "\t" + pos[3], out, err);
}

// ---- M5 ⑥：事务命令（docs/cli.md §11；UX-02(txn) 验收口径 = --help + 示例） ----

std::string_view txn_usage() {
    return "用法: tinykv-cli --endpoint <host:port> txn <子命令>\n"
           "\n"
           "子命令:\n"
           "  begin                       开启会话（服务端持事务状态），返回 session=<id>\n"
           "  set <session> <key> <value> 写缓冲\n"
           "  del <session> <key>         删除缓冲\n"
           "  get <session> <key>         读（读己之写；他者未过期锁 → BLOCKED）\n"
           "  commit <session>            prewrite → commit（冲突立即 abort，退出码 2）\n"
           "  rollback <session>          显式回滚\n"
           "  gc [safe_point]             旧版本清理（FUNC-17，走共识）\n"
           "  demo                        打印完整示例脚本\n"
           "\n"
           "说明: 会话状态在服务端（M6 会话作用域），故需要 --endpoint；语义见 docs/cli.md §11。\n";
}

// txn 子命令族的传输助手（拆函数以满足函数行数门禁）
int txn_call(const Globals& g, const char* path, const std::string& body, std::ostream& out,
             std::ostream& err) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(g, host, port, err)) {
        err << "txn 子命令需要 --endpoint <host:port>；用法见 txn --help\n";
        return kExitUsage;
    }
    auto resp = http_post(host, port, path, body);
    if (!resp) {
        err << path << " 失败：" << describe(resp.error()) << "\n";
        return kExitRuntime;
    }
    out << *resp << "\n";
    return resp->rfind("OK", 0) == 0 ? kExitOk : kExitRuntime;
}

// txn get 走 GET 查询串（读路径）
int txn_get(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
            std::ostream& err) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(g, host, port, err)) {
        err << "txn 子命令需要 --endpoint <host:port>；用法见 txn --help\n";
        return kExitUsage;
    }
    auto body = http_get(host, port, "/txn/get?session=" + url_encode(pos[2]) +
                                       "&key=" + url_encode(pos[3]));
    if (!body) {
        err << "get 失败：" << describe(body.error()) << "\n";
        return kExitRuntime;
    }
    out << *body << "\n";
    return *body == "{}" ? kExitNotFound : kExitOk;
}

// set/del 的参数校验 + 转发（拆出以降 cmd_txn 复杂度）
int txn_set_or_del(const Globals& g, const std::string& sub, const std::vector<std::string>& pos,
                   std::ostream& out, std::ostream& err) {
    const bool is_set = sub == "set";
    const std::size_t want = is_set ? 5U : 4U;
    if (pos.size() != want) {
        err << "用法: txn " << sub
            << (is_set ? " <session> <key> <value>\n" : " <session> <key>\n");
        return kExitUsage;
    }
    const std::string body = is_set ? "set\t" + pos[2] + "\t" + pos[3] + "\t" + pos[4]
                                    : "del\t" + pos[2] + "\t" + pos[3];
    return txn_call(g, is_set ? "/txn/set" : "/txn/del", body, out, err);
}

int cmd_txn(const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
            std::ostream& err) {
    if (pos.size() == 1 || pos[1] == "--help") {
        out << txn_usage();
        return kExitOk;
    }
    const std::string& sub = pos[1];
    if (sub == "demo") {
        out << "事务示例（需 tinykv-server 在线，docs/cli.md §11）：\n"
            << "  tinykv-cli --endpoint H:P txn begin                    # 返回 session=<id>\n"
            << "  tinykv-cli --endpoint H:P txn set <session> k v\n"
            << "  tinykv-cli --endpoint H:P txn get <session> k          # 读己之写\n"
            << "  tinykv-cli --endpoint H:P txn commit <session>         # prewrite→commit\n"
            << "  tinykv-cli --endpoint H:P txn rollback <session>\n"
            << "  tinykv-cli --endpoint H:P txn gc [safe_point]          # 旧版本清理（FUNC-17）\n";
        return kExitOk;
    }
    if (sub == "begin") {
        return txn_call(g, "/txn/begin", "", out, err);
    }
    if (sub == "commit" || sub == "rollback") {
        if (pos.size() != 3) {
            err << "用法: txn " << sub << " <session>\n";
            return kExitUsage;
        }
        return txn_call(g, sub == "commit" ? "/txn/commit" : "/txn/rollback", sub + "\t" + pos[2],
                        out, err);
    }
    if (sub == "gc") {
        return txn_call(g, "/txn/gc", pos.size() == 3 ? "gc\t" + pos[2] : "gc", out, err);
    }
    if (sub == "set" || sub == "del") {
        return txn_set_or_del(g, sub, pos, out, err);
    }
    if (sub == "get") {
        if (pos.size() != 4) {
            err << "用法: txn get <session> <key>\n";
            return kExitUsage;
        }
        return txn_get(g, pos, out, err);
    }
    err << "未知 txn 子命令 " << sub << "；用法见 txn --help\n";
    return kExitUsage;
}

// status/get 的读路径（get 走 /kv 查询串，status 走 /status）
int cmd_cluster_read(const Globals& g, const std::string& cmd, const std::vector<std::string>& pos,
                     std::ostream& out, std::ostream& err) {
    std::string host;
    std::uint16_t port = 0;
    if (!parse_endpoint(g, host, port, err)) {
        return kExitUsage;
    }
    std::string path = "/status";
    if (cmd == "get") {
        if (pos.size() != 2) {
            err << "用法: get <key>\n";
            return kExitUsage;
        }
        path = "/kv?key=" + url_encode(pos[1]) + "&mode=" + g.read_mode;
    }
    auto body = http_get(host, port, path);
    if (!body) {
        err << "请求 " << path << " 失败：" << describe(body.error()) << "\n";
        return kExitRuntime;
    }
    if (cmd == "get") {
        if (body->rfind("{\"found\":false", 0) == 0) {
            err << "NOT_FOUND\n";
            return kExitNotFound;
        }
        out << *body << "\n";
        return kExitOk;
    }
    out << *body;
    if (!body->empty() && body->back() != '\n') {
        out << "\n";
    }
    return kExitOk;
}

int cmd_cluster(const Globals& g, const std::string& cmd, const std::vector<std::string>& pos,
                std::ostream& out, std::ostream& err) {
    if (cmd == "regions") {
        return cmd_regions(g, pos, out, err);
    }
    if (cmd == "region") {
        return cmd_region(g, pos, out, err);
    }
    if (cmd == "scheduler") {
        return cmd_scheduler_status(g, pos, out, err);
    }
    if (cmd == "trace") {
        return cmd_trace_region(g, pos, out, err);
    }
    if (cmd != "status" && cmd != "get" && cmd != "put" && cmd != "del") {
        err << "集群模式支持 status/get/put/del 与 region 观测命令（见 docs/cli.md §10）\n";
        return kExitUsage;
    }
    if (cmd == "put" || cmd == "del") {
        std::string host;
        std::uint16_t port = 0;
        if (!parse_endpoint(g, host, port, err)) {
            return kExitUsage;
        }
        return cmd_cluster_write(cmd, pos, host, port, out, err);
    }
    return cmd_cluster_read(g, cmd, pos, out, err);
}

int run_cli(tinykv::span<const std::string> args, std::ostream& out, std::ostream& err) {
    std::vector<std::string> argv(args.begin(), args.end());
    Globals g;
    auto positional = split_globals(std::move(argv), g);
    if (!positional) {
        err << describe_kind(positional.error()) << "\n" << usage();
        return kExitUsage;
    }
    if (positional->empty() || (*positional)[0] == "--help" || (*positional)[0] == "help") {
        out << usage();
        return kExitOk;
    }

    const std::string& cmd = (*positional)[0];
    if (cmd == "txn") {
        // M5：事务命令在本地/集群模式之前分发（--help/demo 无需服务端；
        // 交互式子命令返回 1 指向用法，docs/cli.md §11）
        return cmd_txn(g, *positional, out, err);
    }
    if (!g.endpoint.empty()) {
        return cmd_cluster(g, cmd, *positional, out, err);  // 集群模式不需要本地配置
    }

    auto cfg = load_config(g.config);
    if (!cfg) {
        err << "配置加载失败：" << describe_kind(cfg.error()) << "\n";
        return kExitRuntime;
    }
    if (cmd == "status") {
        return cmd_status(*cfg, g, out);
    }

    auto engine = Engine::open(to_options(*cfg));
    if (!engine) {
        err << "打开引擎失败：" << describe_kind(engine.error()) << "\n";
        return kExitRuntime;
    }
    RawServer raw(**engine);

    if (cmd == "put") {
        return cmd_put(raw, g, *positional, out, err);
    }
    if (cmd == "get") {
        return cmd_get(raw, g, *positional, out, err);
    }
    if (cmd == "del") {
        return cmd_del(raw, g, *positional, out, err);
    }
    if (cmd == "scan") {
        return cmd_scan(raw, g, *positional, out, err);
    }
    err << "未知命令 '" << cmd << "'\n" << usage();
    return kExitUsage;
}

}  // namespace tinykv::cli
