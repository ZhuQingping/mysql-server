# M7 Aggregation Strategy Reconciliation Taskbook

## 状态

Planned。

## 目标

明确当前 V2 GROUP BY typed-state 子集与商用 aggregation path 的关系，避免两个路径同时声称同一 query shape 已执行。

推荐拆分：

- M7-A: 定义 commercial aggregation 与 legacy typed-state 的互斥路由；
- M7-B: 当前 `pq_group_aggregate_iterator.*` 降级为 legacy/experimental path；
- M7-C: 补 commercial aggregation selected/executed/fallback counter 口径；
- M7-D: 迁移商用 `pq_group_by` 子集，先单表、单 group key、COUNT/SUM/MIN/MAX。

## 允许修改

- `sql/parallel_query/pq_group_aggregate_iterator.*`
- `sql/parallel_query/pq_aggregate.*`
- `sql/parallel_query/pq_optimizer.*`
- `sql/parallel_query/sql_parallel.*`
- `mysql-test/suite/parallel_query/t/*group*`
- `mysql-test/suite/parallel_query/r/*group*`
- `Docs/pq_tasks/commercial-port-m7-aggregation-reconciliation.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 若商用 aggregation 覆盖当前 V2-12A/B，则当前 typed-state path 转为 fallback-only 或删除；
- 若商用 path 迁移成本高，则当前 typed-state path 临时保留；
- status counters 必须区分 commercial path 与 legacy typed-state path；
- 同一 query shape 只能一个 path 计 executed；
- AVG/HAVING/ORDER BY aggregate 后置；
- 默认行为必须可回归。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_diagnostics pq_groupby_dop1_sum_min_max pq_groupby_dop2_partial_count_min_max pq_groupby_dop4_partial_count_sum_min_max pq_groupby_dop_partial_count_star pq_groupby_dop_partial_count_nullable pq_groupby_dop2_partial_worker_error pq_groupby_dop2_partial_external_kill --parallel=1 --vardir=/tmp/pqv_m7 --tmpdir=/tmp/pqt_m7
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m7_full --tmpdir=/tmp/pqt_m7_full
```

若完整 suite 因耗时或环境问题未执行，Completion Report 必须明确说明原因，并至少保留 targeted group-by suite、build、review Agent 检视结论。

## Completion Report

Pending.
