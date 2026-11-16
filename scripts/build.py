#!/usr/bin/env python3
# 文件用途：TinyKV 统一构建入口——配置 → 构建 → 测试一条龙（CMake presets 封装，决策 D0-3）。
# 归属：TinyKV-CPP（M0 基建，SPEC-0 §0.1/§0.2；协议 §8 精简集四件套之一）。
#
# 用法：
#   python scripts/build.py                     # 全流程（preset 按平台自动选：win-debug / linux-debug）
#   python scripts/build.py --preset win-debug  # 指定 preset（全表见 CMakePresets.json）
#   python scripts/build.py --label fast        # ctest 按 LABEL 过滤（unit/simulation/integration/bench/fast...）
#   python scripts/build.py --skip-build        # 跳过配置与构建，只跑测试
#   python scripts/build.py --list              # 列出可用 preset
#
# 环境要求：
#   Windows：MSYS2 ucrt64 工具链在 PATH（gcc/g++/ninja；建议一并 export CMAKE_GENERATOR=Ninja，
#            供不带 preset 的裸 cmake 调用（如 TinyKV-CI check_code.py）复用同一工具链）。
#   Linux：g++/clang 与 ninja。
#   依赖：本机用 MSYS2 pacman（spdlog/protobuf/asio/unordered_dense；gtest 走 CMake FetchContent 锁版），
#         CI/MSVC 环境走 vcpkg 清单（vcpkg.json + *-vcpkg 预设）——构建脚本对两种来源无感知。

import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ctest 证据物固定输出位（SPEC-0 §0.7：build/evidence/ 为 CI 证据链根）
EVIDENCE_DIR = os.path.join(REPO_ROOT, "build", "evidence")


def run(cmd):
    # 执行外部命令：失败即终止并原样透传输出（判定只认实测，不虚绿）
    print("+ " + " ".join(cmd), flush=True)
    try:
        subprocess.run(cmd, cwd=REPO_ROOT, check=True)
    except FileNotFoundError as exc:
        sys.exit(f"找不到可执行程序 {exc.filename}——请按脚本头部说明准备环境")
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)


def default_preset():
    # 按平台选默认 preset：Windows 用 MSYS2 工具链，Linux 用系统工具链
    return "win-debug" if os.name == "nt" else "linux-debug"


def parse_args():
    parser = argparse.ArgumentParser(description="TinyKV 统一构建入口（配置→构建→测试）")
    parser.add_argument("--preset", default=default_preset(), help="CMake configure preset 名")
    parser.add_argument("--label", default=None, help="ctest LABEL 过滤（如 fast/unit）")
    parser.add_argument("--skip-build", action="store_true", help="跳过配置与构建，只跑测试")
    parser.add_argument("--list", action="store_true", help="列出可用 preset")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.list:
        run(["cmake", "--list-presets"])
        return

    if not args.skip_build:
        run(["cmake", "--preset", args.preset])
        run(["cmake", "--build", "--preset", args.preset])

    os.makedirs(EVIDENCE_DIR, exist_ok=True)
    build_dir = os.path.join(REPO_ROOT, "build", args.preset)
    junit = os.path.join(EVIDENCE_DIR, "junit.xml")
    test_cmd = ["ctest", "--test-dir", build_dir, "--output-on-failure",
                "--output-junit", junit]
    if args.label:
        test_cmd += ["-L", args.label]
    run(test_cmd)
    print(f"完成：测试证据已写入 {junit}")


if __name__ == "__main__":
    main()
