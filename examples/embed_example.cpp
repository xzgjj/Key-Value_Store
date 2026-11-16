// 文件用途：UX-07 嵌入示例——把 TinyKV 当组件用，完整可编译（≤20 行核心逻辑）。
// 归属：TinyKV-CPP（M1 单机存储）。
//
// 这段代码同时是 README 的示例来源：改动接口时必须让本文件仍然编译通过，
// 否则"文档里的示例能跑"就只是口号。运行：./embed_example

#include <cassert>
#include <iostream>
#include <span>
#include <string>

#include "storage/engine.h"

int main() {
    using namespace tinykv;
    auto engine = Engine::open({.dir = "./embed_demo.db", .fsync_on_commit = true});
    assert(engine.has_value());

    const std::string key = "k1";
    const std::string value = "v1";
    ModifyVec batch{Modify{Put{.cf = Cf::Default, .key = key, .value = value}}};
    assert((*engine)->write(batch).has_value());

    const auto bytes = std::as_bytes(std::span{key.data(), key.size()});
    auto got = (*engine)->get(Cf::Default, bytes);
    assert(got.has_value() && got->value.has_value());
    std::cout << "read back: " << *got->value << "\n";
    return 0;
}
