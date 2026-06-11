# V2-11 Regression Expansion

## 目标

在不扩大功能语义的前提下，补强 threaded full scan row stream 的 P1 回归覆盖。

本阶段只写 MTR，不修改执行路径；所有新增测试必须自包含，并且完整 `parallel_query` suite 通过。

## 范围

优先覆盖：

- `PREPARE` / `EXECUTE` 语句生命周期；
- `LIMIT` / counter / session state 边界；
- nullable / NULL-heavy / wide non-BLOB row image materialization；
- large scan kill / MDL / DDL 简化并发；
- experimental vars ON/OFF 后 counter 不污染。

暂不覆盖：

- GROUP BY partial aggregation；
- ORDER BY Gather Merge；
- secondary index / ICP；
- partition table；
- BLOB/TEXT no-debug threaded path。当前 iterator gate 排除 blob table，这类测试应验证 fallback 或单独设计 gate，不应伪装成支持。

## 子任务

| Task | Type | Status | Files |
|------|------|--------|-------|
| V2-11A prepare state | MTR | Completed | `pq_read_threaded_prepare_state.test/result` |
| V2-11B row image datatypes | MTR | Completed | `pq_read_threaded_row_image_datatypes.test/result` |
| V2-11C limit/counter boundary | MTR | Completed | `pq_read_threaded_limit_counters.test/result` |
| V2-11D concurrency hardening | MTR | Large kill Completed; MDL pending | `pq_read_threaded_large_kill.test/result` |
| V2-11E experimental vars noop | MTR | Completed | `pq_read_threaded_experimental_vars_noop.test/result` |

## 验收标准

- 每个测试只依赖 `parallel_query` suite 自身；
- 每个测试显式设置并恢复 `parallel_query`、`parallel_default_dop`、`parallel_cost_threshold` 和相关 experimental gate；
- no-debug threaded 正向测试必须验证：
  - `Parallel_queries_executed` delta；
  - `Parallel_queries_fallback` delta 为 0；
  - `Parallel_rows_scanned` delta；
  - `Parallel_workers_launched` delta；
- fallback 测试必须说明 fallback reason 或 counter 预期；
- 不写 timing assertion；
- targeted MTR 和完整 `parallel_query` suite 均通过。

## 当前状态

状态：V2-11A/B/C Completed；V2-11D large kill Completed；V2-11E Completed。

已完成：

- V2-11A prepare state；
- V2-11B row image datatypes；
- V2-11C limit/counter boundary。
- V2-11D large scan external KILL hardening。
- V2-11E experimental vars noop counter isolation。

验证：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_read_threaded_prepare_state \
  pq_read_threaded_row_image_datatypes \
  --parallel=1 --vardir=/tmp/pqv_reg_a_b --tmpdir=/tmp/pqt_reg_a_b
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_limit_counters \
  --parallel=1 --vardir=/tmp/pqv_limit_counters2 \
  --tmpdir=/tmp/pqt_limit_counters2
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full14 --tmpdir=/tmp/pqt_full14
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full15 --tmpdir=/tmp/pqt_full15
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_large_kill \
  --parallel=1 --vardir=/tmp/pqv_large_kill \
  --tmpdir=/tmp/pqt_large_kill
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full21 --tmpdir=/tmp/pqt_full21
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_read_threaded_experimental_vars_noop \
  --parallel=1 --vardir=/tmp/pqv_vars_noop \
  --tmpdir=/tmp/pqt_vars_noop
```

V2-11A/B 结果：完整 `parallel_query` suite 通过，共 46 项。

V2-11C 说明：

- `LIMIT 4 OFFSET 3` 下 threaded path 返回与串行一致；
- `rows_delta=7`，记录 leader early-stop accounting，即 OFFSET + LIMIT。
- V2-11C 后完整 `parallel_query` suite 通过，共 47 项。

V2-11D large kill 说明：

- DOP=4 debug threaded shadow path 在 2048 行表上启动 worker 后接受外部 `KILL QUERY`；
- 预期查询返回 `ER_QUERY_INTERRUPTED`，不计入 `Parallel_queries_executed`，不触发 serial fallback；
- 验证 worker launch delta 为 4，随后串行 `COUNT(*)` 仍返回 2048。

V2-11E experimental vars noop 说明：

- `parallel_query=OFF` 时，即使 DOP1/DOP2/DOP4 experimental gate 全部 ON，也不应触发 PQ executed/fallback/rows/workers counters；
- `parallel_query=ON` 但三个 experimental gate 全部 OFF 时，DOP1/DOP2/DOP4 查询均应走 safe fallback，不启动 workers。
- 用普通 full scan 查询验证 gate noop，避免把 implicit aggregate read-view 生命周期混入本测试。

下一步：

- 继续 V2-11D MDL minimal concurrency；
- 继续评估 V2-12A-3 DOP1 Partial Group Execution。
