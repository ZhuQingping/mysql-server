# V2-12 Feature Expansion Decision

## 目标

在 P1 回归补强并行推进时，提前明确下一轮功能扩展的优先级、设计边界和不可并行文件范围。

本阶段是设计任务，不直接修改源码。

## 候选方向

| Direction | Priority | Why | Must Design First |
|-----------|----------|-----|-------------------|
| GROUP BY partial aggregation | P0 | implicit aggregate 已能消费 threaded row stream，显式 GROUP BY 仍 fallback，收益最高 | Yes |
| ORDER BY Gather Merge | P1 | 需要全局有序输出协议，不能依赖现有 round-robin gather | Yes |
| secondary index / ICP | P1 | 涉及 index tuple、row lookup、ICP predicate 和 covering/non-covering 分歧 | Yes |
| partition table | P1 | SQL partition pruning 与 InnoDB range dispatch 会叠加 | Yes |
| MQ backpressure / OOM injection | P1 | 需要可靠注入点，避免 flaky 大数据测试 | Small design first |

## 推荐顺序

1. 先完成 V2-11 P1 regression expansion，确保当前 clustered full scan threaded path 退化风险低；
2. 并行完成 V2-12A GROUP BY partial aggregation design；
3. GROUP BY 设计验收后，再进入实现；
4. ORDER BY、secondary index/ICP、partition table 继续保持设计先行，不与 GROUP BY 实现并行修改同一执行路径。

## V2-12A GROUP BY Partial Aggregation Design 要求

必须回答：

- worker 输出 raw row 还是 partial aggregate state；
- leader merge 协议和 Exchange message format 是否扩展；
- 支持哪些 aggregate function：
  - `COUNT(*)` / `COUNT(expr)`；
  - `SUM` / `AVG`；
  - `MIN` / `MAX`；
  - 暂不支持或 fallback 的 `COUNT(DISTINCT)`；
- NULL、DECIMAL、DOUBLE、collation 的语义；
- memory limit / tmp table spill / OOM 行为；
- worker error、leader kill、early EOF 的清理语义；
- EXPLAIN 和 fallback reason；
- MTR 矩阵。

建议文档：

- `Docs/pq_tasks/v2-12a-groupby-partial-aggregation-design.md`

## 不可并行文件范围

GROUP BY partial aggregation 实现阶段预计会触碰：

- `sql/parallel_query/`;
- `sql/sql_executor.cc`;
- `sql/iterators/*`;
- `sql/item_sum.*`;
- `sql/opt_explain.*`;
- `mysql-test/suite/parallel_query/t/`;
- `mysql-test/suite/parallel_query/r/`。

这些文件与 ORDER BY / ICP / partition 的执行路径高度耦合，实现阶段默认串行。

## 当前状态

状态：Design queue ready。

下一步：

- V2-11A/B 合入后，启动 V2-12A GROUP BY partial aggregation design；
- V2-12A 只做设计确认，不直接编码。
