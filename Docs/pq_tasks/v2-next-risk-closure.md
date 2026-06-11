# V2 Next Risk Closure 任务书

## 背景

V2-9E 后，DOP=1/DOP=2 clustered full scan threaded row stream 已在默认 OFF 实验变量保护下闭环。后续 P0-C 已把 DOP=4 no-debug experimental gate 纳入同样的默认 OFF 保护。

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

状态：Completed。

实现：

- 新增 `pq_read_threaded_dop2_read_view`；
- RR 下事务内第二次 DOP2 聚合读取仍看到旧 snapshot；
- commit 后新语句看到已提交 insert/update/delete；
- RC 下同一事务后续语句看到 writer 新提交；
- status 验证 `executed_delta=5`、`fallback_delta=0`、`rows_delta=21`、`workers_delta=10`。

验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_dop2_read_view \
  --parallel=1 --vardir=/tmp/pqv_dop2_rv2 --tmpdir=/tmp/pqt_dop2_rv2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full9 --tmpdir=/tmp/pqt_full9
```

结果：完整 `parallel_query` suite 通过，共 38 项。

## P0-C DOP4 Guard And Correctness

目标：在 DOP2 稳定后评估 DOP4。

建议拆分：

- Debug-only DOP4 shadow path；
- DOP4 多 range 完整集合；
- DOP4 worker ERROR / external KILL；
- 通过后再新增默认 OFF 的 no-debug DOP4 gate。

状态：Completed。

实现：

- 新增 debug flag `pq_read_threaded_dop4_shadow_path`；
- 新增 `pq_read_threaded_dop4_shadow_multirange`，验证 1024 行多 range 聚合完整性；
- 新增 `pq_read_threaded_dop4_worker_error`，验证 worker ERROR token propagation；
- 新增 `pq_read_threaded_dop4_external_kill`，验证 4 worker 启动后 external `KILL QUERY` cleanup。
- 新增默认 OFF 的 `parallel_query_experimental_threaded_dop4`；
- 新增 `pq_read_threaded_dop4_experimental_var`，验证 no-debug DOP=4 threaded full scan 可显式启用；
- 新增 `pq_read_threaded_dop4_gate_negative`，验证 DOP4 gate 不会误启 DOP=1/DOP=2。

验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_read_threaded_dop4_worker_error \
  pq_read_threaded_dop4_external_kill \
  pq_read_threaded_dop4_shadow_multirange \
  --parallel=1 --vardir=/tmp/pqv_dop4_group --tmpdir=/tmp/pqt_dop4_group
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full11 --tmpdir=/tmp/pqt_full11
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_vars pq_read_threaded_dop4_experimental_var \
  pq_read_threaded_dop4_gate_negative \
  --parallel=1 --vardir=/tmp/pqv_dop4_gate2 --tmpdir=/tmp/pqt_dop4_gate2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full12 --tmpdir=/tmp/pqt_full12
```

结果：完整 `parallel_query` suite 通过，共 43 项。

## P0-D Performance Baseline

目标：建立可复现性能基线，不作为功能正确性提交混入。

建议覆盖：

- 1M+ 行 clustered full scan；
- serial / DOP1 / DOP2 / DOP4；
- rows/sec、CPU、worker wait、MQ wait；
- 明确 macOS debug build 结果只作趋势参考，最终需要 release build 或目标环境验证。

状态：Baseline materials completed；release/目标环境采样待执行。

实现：

- 新增 [v2-p0d-performance-baseline.md](v2-p0d-performance-baseline.md)，定义 serial/DOP1/DOP2/DOP4 对比矩阵、采样要求和验收标准；
- 新增 [pq_perf_baseline.sql](pq_perf_baseline.sql)，可在已启动 mysqld 上创建 1M+ clustered rows 并输出 per-run 与 aggregate 结果；
- 新增 `pq_perf_baseline_contract` MTR，只验证 serial/DOP1/DOP2/DOP4 的 status counter contract，不做 timing assertion。

验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_perf_baseline_contract \
  --parallel=1 --vardir=/tmp/pqv_perf_contract --tmpdir=/tmp/pqt_perf_contract
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full13 --tmpdir=/tmp/pqt_full13
```

结果：完整 `parallel_query` suite 通过，共 44 项。

## P1 Regression Expansion

候选项：

- prepared statement；
- LIMIT / examined_rows / found_rows；
- NULL-heavy row / wide row；
- MDL / DDL 并发；
- large table kill；
- OOM / MQ backpressure；
- production-like worker error propagation。

### P1-E Aggregate Fallback Read-view Cleanup

状态：Open。

触发记录：

- 在新增 V2-11E noop 测试的初版中，`parallel_query=ON`、DOP1/DOP2/DOP4 experimental gate 全部 OFF 时连续执行 `COUNT(*)` safe fallback；
- 后续 `pq_read_threaded_limit_counters` 的 DOP2 threaded path 进入 worker `Parallel_reader::check_visibility()` 时触发 debug assertion：
  `!trx || trx->read_view == nullptr || MVCC::is_view_active(trx->read_view)`；
- 将 V2-11E 改为普通 full scan 查询后，`pq_read_threaded_experimental_vars_noop + pq_read_threaded_limit_counters` 短序列和完整 suite 均通过。

后续要求：

- 单独设计 aggregate fallback read-view cleanup 回归；
- 不要混入 experimental vars noop；
- 重点检查 implicit aggregate safe fallback 后 leader/worker open context、trx read view、handler end 的释放顺序；
- 若复现稳定，应在 started 前 fallback window 清理 read view，或禁止该路径留下 inactive read view 给后续 threaded scan。

## P1 Feature Expansion Decision

候选方向：

- GROUP BY partial aggregation；
- ORDER BY Gather Merge；
- secondary index / ICP；
- partition table。

建议下一阶段优先考虑 GROUP BY partial aggregation，因为 implicit aggregate row stream 已经闭环，但显式 GROUP BY 仍 fallback。

当前状态：P0-A/P0-B/P0-C Completed；下一步进入 P0-D performance baseline。
