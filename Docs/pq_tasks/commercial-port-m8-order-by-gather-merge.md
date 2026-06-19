# M8 ORDER BY Gather Merge Taskbook

## 状态

Completed on 2026-06-19。

## 目标

迁移 `Exchange_sort` 和 ORDER BY/Gather Merge 能力，先覆盖简单 ORDER BY 场景。

推荐拆分：

- M8-A: compile-only port `binary_heap.h` + `exchange_sort.*`，默认不可达；
- M8-B: leader-only/synthetic merge smoke，验证 heap ordering、ASC/DESC、stable rowid tie-break；
- M8-C: simple `ORDER BY field [ASC|DESC] LIMIT`，full scan worker output；
- M8-D: index sort / desc index / stable output；
- M8-E: GROUP BY same key ORDER BY same key，必须等 M7 路由稳定。

本轮完成 M8-A/B。M8-C/D/E 暂缓：当前 `Exchange_sort` 只作为 compile/synthetic smoke 骨架接入，不打开真实 ORDER BY worker path。真实 ORDER BY 仍在 optimizer eligibility 阶段以 `HAS_ORDER_BY` 拒绝，走串行执行。

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

本轮完成：

- 新增 `sql/parallel_query/binary_heap.h`；
- 新增 `sql/parallel_query/exchange_sort.h/.cc`；
- 将 `exchange_sort.cc` 加入 `sql/CMakeLists.txt`；
- 新增 `Exchange_sort::run_synthetic_order_merge_smoke()`，内部验证 ASC、DESC 和同 key rowid tie-break；
- 新增 `Gather_operator::run_exchange_sort_smoke()`，只在已有 PQ PROBE 成功后的 smoke 区域运行；
- 新增 `Parallel_exchange_sort_smoke_runs` / `Parallel_exchange_sort_smoke_rows`；
- 新增 `pq_commercial_order_by` MTR，验证 synthetic smoke 可观测、真实 ORDER BY 仍是 optimizer-reject serial boundary；
- 更新 `pq_stats.result`，Parallel status var 数量从 56 增至 58。

验证结果：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by --parallel=1 --vardir=/tmp/pqv_m8_order2 --tmpdir=/tmp/pqt_m8_order2
TMPDIR=/tmp ./mtr --suite=parallel_query pq_stats --parallel=1 --vardir=/tmp/pqv_m8_stats --tmpdir=/tmp/pqt_m8_stats
TMPDIR=/tmp ./mtr --suite=parallel_query pq_commercial_order_by pq_explain_fallback pq_read_threaded_dop2_multirange pq_groupby_dop2_partial_count_min_max --parallel=1 --vardir=/tmp/pqv_m8b --tmpdir=/tmp/pqt_m8b
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m8_full2 --tmpdir=/tmp/pqt_m8_full2
```

结果：

- build passed；
- `pq_commercial_order_by` passed；
- `pq_stats` passed；
- M8 targeted suite passed，5 tests successful；
- full `parallel_query` suite passed，73 tests successful。

Review Agent 结论：

- 初审无阻断；
- 建议澄清 `pq_commercial_order_by` 中 ORDER BY 是 optimizer-reject serial boundary，不是 PQ candidate 后 fallback；
- 已补充注释和 `EXPLAIN` 覆盖，结果显示 `Using filesort; Not parallel HAS_ORDER_BY`；
- 复核结论：无阻断。

后续：

- M8-C simple ORDER BY worker output 需要等待真实 worker result path 更稳定；
- M8-D index/desc index、M8-E GROUP BY + ORDER BY 后置。
