# M8 ORDER BY Gather Merge Taskbook

## 状态

Planned。

## 目标

迁移 `Exchange_sort` 和 ORDER BY/Gather Merge 能力，先覆盖简单 ORDER BY 场景。

推荐拆分：

- M8-A: compile-only port `binary_heap.h` + `exchange_sort.*`，默认不可达；
- M8-B: leader-only/synthetic merge smoke，验证 heap ordering、ASC/DESC、stable rowid tie-break；
- M8-C: simple `ORDER BY field [ASC|DESC] LIMIT`，full scan worker output；
- M8-D: index sort / desc index / stable output；
- M8-E: GROUP BY same key ORDER BY same key，必须等 M7 路由稳定。

## 允许修改

- `sql/parallel_query/exchange_sort.h`
- `sql/parallel_query/exchange_sort.cc`
- `sql/parallel_query/binary_heap.h`
- `sql/parallel_query/exchange.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/pq_iterators.*`
- `mysql-test/suite/parallel_query/t/pq_commercial_order_by.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_order_by.result`
- `Docs/pq_tasks/commercial-port-m8-order-by-gather-merge.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`

## 设计要求

- 先支持 ORDER BY key 简单场景；
- 不支持 shape 必须 fallback；
- 输出顺序必须由 MTR 验证；
- `Exchange_sort` 不能孤立提前打开真实 worker path；
- expression/order aggregate/distinct/window/subquery fallback；
- 不与 M7 aggregation 变更混在同一提交。
- 验证中的 GROUP BY 用例只作为 aggregation 回归护栏，M8 不允许修改 aggregation route 或 typed-state result path。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by pq_explain_fallback pq_read_threaded_dop2_multirange pq_groupby_dop2_partial_count_min_max --parallel=1 --vardir=/tmp/pqv_m8 --tmpdir=/tmp/pqt_m8
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m8_full --tmpdir=/tmp/pqt_m8_full
```

若完整 suite 因耗时或环境问题未执行，Completion Report 必须明确说明原因，并至少保留 ORDER BY targeted suite、build、review Agent 检视结论。

## Completion Report

Pending.
