# M4 排障演练记录（UX-04，SPEC-3 §3.7）

> 演练目的：验证"人为注入一个 BUG，仅凭 CLI status/region 命令 + 日志即可定位"的排障路径。
> 演练时间：2026-11-16；演练分支 `3`（HEAD `1a0529a`）。
> 注入的 BUG 演练结束即还原（`git diff` 清零），不是长期缺陷，不入 docs/bugs.md 登记。

## 1. 注入的 BUG

位置：`src/raftstore/peer.cpp` `Peer::execute` 的 admin 分发。

```cpp
// 原实现：
if (auto r = apply_split_admin(req.admin().split()); !r) {
    return r;
}
// 注入后（短路）：
(void)req.admin().split();   // split 回执成功但从未切分——模拟"处理分支漏调"
```

这类缺陷的特征：**回执成功、元数据未变**。客户端（CLI）拿到 `OK new_region_id=2`，
但路由/元数据没有任何变化——典型的"假成功"，比崩溃更危险。

## 2. 症状（仅凭 CLI 输出发现）

```
$ tinykv-cli --endpoint ... region split 1 m
OK new_region_id=2            ← 回执正常

$ tinykv-cli --endpoint ... regions
[{"id":1,"start":"","end":"","version":1,"conf_ver":0,...}]   ← 只有一个 region！
```

**矛盾点即定位起点**：split 声称成功且分配了新 region id=2，但 `regions` 里没有 id=2，
region 1 的 epoch 也停在 `version=1, conf_ver=0`（split apply 成功必然 version+1）。

## 3. 定位路径（CLI 输出 → 日志/代码）

1. CLI 回执 OK → 提案已被共识接受并 apply（否则 `region split` 会返回 `ERR ...`/退出码 2）；
2. `regions` 无新 region 且 epoch 未推进 → **apply 阶段没有执行切分**，而不是切分被拒
   （被拒会有 `ERR split_key 必须落在 region 开区间内`，见红线1 路径）；
3. 打开日志/复核代码：`Peer::execute` 的 admin 分发里，split 分支存在短路——
   `apply_split_admin` 从未被调用；回执由 execute 正常返回，于是"OK 但没切"。
4. 佐证：既有回归用例 `CliRegion.ListsRegionsAfterSplit` 在注入状态下立即转红
   （断言 `"id":2` 与 `"start":"m"` 找不到），与 CLI 观察一致。

## 4. 修复与验证

还原 `apply_split_admin` 调用后：
- `CliRegion.ListsRegionsAfterSplit` 恢复绿（`regions` 出现 id=2、start=m）；
- `mr_chaos_n150` 150 轮的 region 不变量断言（三副本指纹一致 + region_count=2）
  在无注入状态下持续绿——同类缺陷若再次出现，该用例会在 150 个种子里任何一个上爆发。

## 5. 演练结论

- **CLI 观测命令够用**：`region split`（回执）+ `regions`（元数据）+ epoch 字段
  三点交叉即可把"假成功"类缺陷锁死在 apply 阶段，不需要 attach 调试器。
- **回归用例是第二道网**：注入的 BUG 被既有用例 1:1 捕获，说明 §10 命令的端到端用例
  不是摆设；UX-04 与登记表用例互为印证。
- **已知限制**（同 notes-m4 §6）：trace 的 Peer 内事件埋点未接入，本次演练的"日志"环节
  用代码复核代替了 trace-region 日志；埋点接入后，此演练可完全在 CLI + trace 文件内闭环。
