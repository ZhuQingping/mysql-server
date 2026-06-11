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
| V2-11D concurrency hardening | MTR | Pending | large kill / MDL minimal tests |
| V2-11E experimental vars noop | MTR | Pending | vars OFF/ON counter isolation |

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

状态：V2-11A/B/C Completed。

已完成：

- V2-11A prepare state；
- V2-11B row image datatypes；
- V2-11C limit/counter boundary。

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
```

V2-11A/B 结果：完整 `parallel_query` suite 通过，共 46 项。

V2-11C 说明：

- `LIMIT 4 OFFSET 3` 下 threaded path 返回与串行一致；
- `rows_delta=7`，记录 leader early-stop accounting，即 OFFSET + LIMIT。
- V2-11C 后完整 `parallel_query` suite 通过，共 47 项。

下一步：

- 继续 V2-11D concurrency hardening；
- 启动 V2-12A GROUP BY partial aggregation design。
