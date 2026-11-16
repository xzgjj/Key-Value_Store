# 缺陷回归登记（bugs.md）

> **给执行代理的硬要求**（2026-11-16 用户指令，已落成 CI 门禁）
>
> 修 bug 的第一步不是改代码，是**把触发它的场景留下来**——场景不是回忆录，是能重跑的回归测试。
>
> 1. **复现**：现象 + 最小触发条件 + 可执行命令（种子/参数/脚本），一条都不能省；
> 2. **最小化** → 定位根因 → 修根因（禁止打补丁掩盖症状）；
> 3. **同一次提交**内把用例留成回归测试（`tests/` 对应层级），用例源码写 `// BUG-NNNN: 一句话`；
> 4. 该测试登记进 [docs/testing.md](testing.md) §4（未登记 = registry 阶段即红）；
> 5. 本表加一行，跑 `python tools/ci/run_ci.py --stage bugfix` 必须绿；
> 6. 禁止"先修后补"，禁止删除历史行（只增不删，更正另起一行并写明取代关系）。
>
> 门禁实现：[ci/rules.toml](../ci/rules.toml) `[bugfix]`、[tools/ci/check_bugfix.py](../tools/ci/check_bugfix.py)；
> 发现码与处置见 [CI.md](../CI.md) §4。

## 登记表（唯一事实来源）

<!-- bugs:begin v1 -->
| Bug | 触发场景 | 复现 | 回归测试 | 修复 |
|---|---|---|---|---|
| BUG-0001 | flush 写出 SST 后重开引擎必然读到 Corrupted：finish() 先 put_u32(条数) 再计算 CRC，此时 body 已被撑大 4 字节而校验长度仍按旧口径，读端按“总长-8”校验必然不匹配；正常读写全绿、只有重开校验才暴露 | `ctest --test-dir build/win-debug -R sst_test`（写 3 条→finish→重开，断言 CRC 自洽） | sst_test | 2026-11-16 |
<!-- bugs:end -->

> 空表 = 尚无缺陷登记。修第一个 bug 时按上面六步补第一行。

## 示例（形态参考，检查器不解析本节）

```markdown
| BUG-0001 | kill leader 后 5s 内客户端写入返回 Unavailable，重启节点丢失最后 2 条日志 | `--seed=42 --kill=leader`（夜跑脚本 run_chaos.sh） | `raft_chaos_n150` | 2026-11-20（1a2b3c4） |
```

## 列约定

| 列 | 规则 | 检查器判定 |
|---|---|---|
| Bug | `BUG-NNNN`，四位递增、永不复用 | 格式 + 全表唯一 |
| 触发场景 | 现象 + 触发条件，≥ 12 字；含崩溃/分区/重试/并发交错等真实条件 | 字数下限 |
| 复现 | 反引号包裹的可执行命令或参数（`--seed=`、`ctest -R`、脚本调用） | 必须含命令片段 |
| 回归测试 | 只填 docs/testing.md §4 的 ctest 测试名（首个反引号 token），不填文件名 | 必须已登记 + 源码含编号标记 |
| 修复 | `YYYY-MM-DD`，有 commit 带上短哈希 | 日期格式 |
