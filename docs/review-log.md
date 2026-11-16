# 审查台账（review-log）

> 每阶段收尾的"三轮检查"记录在此，随提交入库、跨会话可查。
> 写入走 `python tools/review_rounds.py record`，门禁走 `gate`；**不要手工改标记区**，
> 表格列宽由工具维护，手改会破坏解析。
> 每轮设不同重点：可判定性 → 契约与复用 → 文档与红线（见 AGENT_EXECUTION_PROTOCOL.md §8）。

## 轮次记录

<!-- review:begin v1 -->
| 阶段 | 轮次 | 重点 | 结论 | 备注 |
|---|---|---|---|---|
<!-- review:end -->

## 约定

- 「结论」只允许 PASS / FAIL；FAIL 轮次保留在台账里，作为修复依据，不删除。
- 三轮全 PASS 后跑对齐检查（`python tools/ci/check_align.py`），两者都过才可进入推送环节。
- 台账本身不替代证据：每轮「备注」应写清跑了什么命令、产物在哪。
