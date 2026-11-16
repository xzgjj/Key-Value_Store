// 文件用途：cli.h 的实现——参数解析、子命令执行、人类可读输出。
// 归属：TinyKV-CPP（M1 单机存储，UX-02/UX-05）。

#include "cli/cli.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error.h"
#include "cli/http_client.h"
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

// 已解析的全局参数
struct Globals {
    std::string config{kDefaultConfig};
    std::string endpoint;     // 非空 = 集群模式
    std::string read_mode{"read_index"};   // read_index / lease / log
    Cf cf{Cf::Default};
    std::size_t limit{10};
};

std::string_view usage() {
    return
        "用法: tinykv-cli [--config <path>] <命令> [参数]\n"
        "\n"
        "命令:\n"
        "  put <key> <value>        写入（同 key 覆盖）\n"
        "  get <key>                读取；key 不存在时退出码 3\n"
        "  del <key>                删除（删除不存在的 key 视为成功）\n"
        "  scan [start]             按升序扫描，默认 limit 10\n"
        "  status                   打印配置与引擎统计\n"
        "\n"
        "选项:\n"
        "  --config <path>          配置文件路径（默认 demo.yaml）\n"
        "  --endpoint <host:port>   集群模式：status/get/put/del（scan 暂未开放）\n"
        "  --read-mode <m>          read_index（默认）/ lease / log，仅集群 get 生效\n"
        "  --cf <default|write|lock> 列族（默认 default）\n"
        "  --limit <n>              scan 返回条数上限\n"
        "  --help                   显示本帮助\n";
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
            return std::unexpected(InvalidArgument{"未知列族 '" + value + "'"});
        }
        g.cf = *cf;
    } else if (name == "--limit") {
        g.limit = static_cast<std::size_t>(std::stoul(value));
    } else if (name == "--read-mode") {
        if (value != "read_index" && value != "lease" && value != "log") {
            return std::unexpected(InvalidArgument{"--read-mode 只能是 read_index/lease/log"});
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
                return std::unexpected(InvalidArgument{"选项 " + a + " 缺少取值"});
            }
            if (auto r = apply_global(g, a, args[++i]); !r) {
                return std::unexpected(r.error());
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
    opt.memtable_limit_bytes = cfg.engine.memtable_limit_mb << 20U;
    opt.wal_segment_bytes = cfg.engine.wal_segment_mb << 20U;
    return opt;
}

int cmd_put(RawServer& raw, const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
            std::ostream& err) {
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

int cmd_get(RawServer& raw, const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
            std::ostream& err) {
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

int cmd_del(RawServer& raw, const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
            std::ostream& err) {
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

int cmd_scan(RawServer& raw, const Globals& g, const std::vector<std::string>& pos, std::ostream& out,
             std::ostream& err) {
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
    const std::string body =
        cmd == "put" ? "put\t" + pos[1] + "\t" + pos[2] : "del\t" + pos[1];
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

int cmd_cluster(const Globals& g, const std::string& cmd, const std::vector<std::string>& pos,
                std::ostream& out, std::ostream& err) {
    if (cmd != "status" && cmd != "get" && cmd != "put" && cmd != "del") {
        err << "集群模式支持 status / get / put / del；scan 见 docs/cli.md\n";
        return kExitUsage;
    }
    const auto colon = g.endpoint.rfind(':');
    if (colon == std::string::npos) {
        err << "--endpoint 需为 host:port\n";
        return kExitUsage;
    }
    const std::string host = g.endpoint.substr(0, colon);
    const auto port = static_cast<std::uint16_t>(std::stoul(g.endpoint.substr(colon + 1)));
    if (cmd == "put" || cmd == "del") {
        return cmd_cluster_write(cmd, pos, host, port, out, err);
    }

    std::string path = "/status";
    if (cmd == "get") {
        if (pos.size() != 2) {
            err << "用法: get <key>\n";
            return kExitUsage;
        }
        path = "/kv?key=" + pos[1] + "&mode=" + g.read_mode;
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

int run_cli(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
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
    if (!g.endpoint.empty()) {
        return cmd_cluster(g, cmd, *positional, out, err);   // 集群模式不需要本地配置
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
