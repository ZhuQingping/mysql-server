# V2 Next Risk Closure 任务书

## 背景

V2-9E 后，DOP=1/DOP=2 clustered full scan threaded row stream 已在默认 OFF 实验变量保护下闭环。完整 `parallel_query` suite 当前 36 项通过。

本文件记录后续收口任务，避免把 DOP4、性能、复杂算子和回归补强混在一个大阶段。

## P0-A Locking Read EXPLAIN Fix

目标：修复 `FOR UPDATE` / `LOCK IN SHARE MODE` / `FOR SHARE` 在 EXPLAIN 阶段仍可能显示 PQ annotation 的问题。

要求：

- EXPLAIN 和执行阶段的 locking read fallback reason 一致；
- 恢复或新增 `pq_locking_read_fallback`；
- 验证不会污染 fallback/executed counters。

状态：Completed。

实现：

- `PT_locking_clause::contextualize()` 在 `parallel_query=ON` 的 EXPLAIN 中保留 locking clause 到 `Table_ref`，使 PQ eligibility 能稳定看到 `LOCKING_READ`；
- 新增 `pq_locking_read_fallback`，覆盖 `FOR UPDATE` / `LOCK IN SHARE MODE` / `FOR SHARE` 的 EXPLAIN 和真实执行 counters。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query pq_locking_read_fallback \
  --parallel=1 --vardir=/tmp/pqv_locking2 --tmpdir=/tmp/pqt_locking2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full8 --tmpdir=/tmp/pqt_full8
```

结果：完整 `parallel_query` suite 通过，共 37 项。

## P0-B DOP2 Read-view Concurrency

目标：验证 no-debug `parallel_query_experimental_threaded_dop=ON` 下 DOP=2 row stream 的 read-view 一致性。

要求：

- 覆盖 RR / RC；
- 并发 insert/update/delete 后，DOP2 结果与串行一致；
- 明确 leader-pinned read view 与 worker callback producer 的边界；
- 不引入 DOP4 或复杂算子。

## P0-C DOP4 Guard And Correctness

目标：在 DOP2 稳定后评估 DOP4。

建议拆分：

- Debug-only DOP4 shadow path；
- DOP4 多 range 完整集合；
- DOP4 worker ERROR / external KILL；
- 通过后再新增默认 OFF 的 no-debug DOP4 gate。

## P0-D Performance Baseline

目标：建立可复现性能基线，不作为功能正确性提交混入。

建议覆盖：

- 1M+ 行 clustered full scan；
- serial / DOP1 / DOP2 / DOP4；
- rows/sec、CPU、worker wait、MQ wait；
- 明确 macOS debug build 结果只作趋势参考，最终需要 release build 或目标环境验证。

## P1 Regression Expansion

候选项：

- prepared statement；
- LIMIT / examined_rows / found_rows；
- NULL-heavy row / wide row；
- MDL / DDL 并发；
- large table kill；
- OOM / MQ backpressure；
- production-like worker error propagation。

## P1 Feature Expansion Decision

候选方向：

- GROUP BY partial aggregation；
- ORDER BY Gather Merge；
- secondary index / ICP；
- partition table。

建议下一阶段优先考虑 GROUP BY partial aggregation，因为 implicit aggregate row stream 已经闭环，但显式 GROUP BY 仍 fallback。

当前状态：P0-A Completed；下一步进入 P0-B DOP2 Read-view Concurrency。
