# M7 Aggregation Strategy Reconciliation Taskbook

## 状态

Completed on 2026-06-19。

## 目标

明确当前 V2 GROUP BY typed-state 子集与商用 aggregation path 的关系，避免两个路径同时声称同一 query shape 已执行。

推荐拆分：

- M7-A: 定义 commercial aggregation 与 legacy typed-state 的互斥路由；
- M7-B: 当前 `pq_group_aggregate_iterator.*` 降级为 legacy/experimental path；
- M7-C: 补 commercial aggregation selected/executed/fallback counter 口径；
- M7-D: 迁移商用 `pq_group_by` 子集，先单表、单 group key、COUNT/SUM/MIN/MAX。

本轮执行范围为 M7-A/B/C。M7-D 暂不迁移，原因是商用聚合依赖更完整的 worker plan clone、`Query_result_mq`、worker result path 和 gather/exchange 结果协议；在这些主路径未稳定前直接搬迁聚合会扩大风险。当前 typed-state GROUP BY 路径保留为 legacy/experimental 护栏，并通过 status counters 明确与 commercial aggregation 互斥。

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

- 当前商用 aggregation path 尚未接入执行，commercial counters 只允许记录 attempt/fallback；
- 当前 typed-state path 临时保留为 legacy/experimental path；
- status counters 必须区分 commercial path 与 legacy typed-state path；
- 同一 query shape 只能一个 path 计 executed：本阶段 commercial executed 必须保持 0，legacy typed executed 才能增长；
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

本轮完成 M7-A/B/C，M7-D 延后。

实现结果：

- 新增 `Parallel_groupby_commercial_attempts/selected/executed/fallback`；
- 新增 `Parallel_groupby_legacy_typed_selected/executed/fallback`；
- 当前 commercial aggregation 未接入执行，只记录 attempt/fallback，`selected/executed` 保持 0；
- 当前 `pq_group_aggregate_iterator.*` 明确作为 legacy/experimental typed-state GROUP BY 路径；
- legacy typed selected/executed 只在 typed COUNT/SUM/MIN/MAX 和 DOP partial 成功路径增长；
- unsupported factory shape 不增长 legacy typed counters；
- typed SUM overflow 与 DOP partial runtime unsupported 才增长 legacy typed fallback；
- 更新 `pq_groupby_dop_partial_counters` 和 `pq_stats` 结果文件。

验证结果：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_dop_partial_counters --parallel=1 --vardir=/tmp/pqv_m7_counter2 --tmpdir=/tmp/pqt_m7_counter2
TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m7_stats --tmpdir=/tmp/pqt_m7_stats
TMPDIR=/tmp ./mtr --suite=parallel_query pq_groupby_diagnostics pq_groupby_dop1_sum_min_max pq_groupby_dop2_partial_count_min_max pq_groupby_dop4_partial_count_sum_min_max pq_groupby_dop_partial_count_star pq_groupby_dop_partial_count_nullable pq_groupby_dop2_partial_worker_error pq_groupby_dop2_partial_external_kill --parallel=1 --vardir=/tmp/pqv_m7b --tmpdir=/tmp/pqt_m7b
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m7_full2 --tmpdir=/tmp/pqt_m7_full2
```

结果：

- build passed；
- counters/status targeted tests passed；
- targeted GROUP BY suite passed，9 tests successful；
- full `parallel_query` suite passed，72 tests successful。

Review Agent 结论：

- 初审发现 legacy typed counters 覆盖 legacy temp-table fallback、`pq_stats.result` stale；
- 已修复并复核；
- 复核结论：无阻断 findings。

M7-D 延后原因：

- 商用聚合依赖完整 worker plan clone、`Query_result_mq`、worker result path 和 gather/exchange 结果协议；
- 在这些主路径稳定前直接迁移 GROUP BY 商用执行链风险过高；
- 后续应在 commercial result path 稳定后再迁移单表、单 group key、COUNT/SUM/MIN/MAX 子集。
