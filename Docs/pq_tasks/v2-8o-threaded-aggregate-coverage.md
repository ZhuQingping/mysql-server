# V2-8O Threaded Aggregate Coverage 任务书

## 目标

V2-8O 目标是在 V2-8M/V2-8N 之后，验证 DOP=1 threaded row stream 可以被 MySQL 上层普通聚合算子自然消费。

本阶段不接入 `pq_aggregate` partial aggregation，也不启用 worker-side aggregate。只证明：

- worker thread 产出完整 base row image；
- leader `PQTableScanIterator::Read()` 返回完整 row stream；
- 上层 MySQL aggregation 对 `COUNT/SUM/AVG/MIN/MAX` 结果正确；
- 聚合查询没有 serial fallback。

## 范围

允许修改：

- `mysql-test/suite/parallel_query/t/pq_read_threaded_aggregate.test`
- `mysql-test/suite/parallel_query/r/pq_read_threaded_aggregate.result`
- `Docs/pq_tasks/README.md`
- 本任务文档

禁止修改：

- `pq_aggregate.*` partial aggregation 执行接入；
- worker-side Item/JOIN clone；
- DOP>1 真实执行；
- `pq_worker_scan_next()`；
- 默认打开 `parallel_query_experimental_threaded_dop1`。

## 验证用例

新增 `pq_read_threaded_aggregate`：

- `parallel_query=ON`
- `parallel_default_dop=1`
- `parallel_cost_threshold=0`
- `parallel_query_experimental_threaded_dop1=ON`

覆盖查询：

- `COUNT(*)`, `SUM(val)`, `AVG(val)`, `MIN(val)`, `MAX(val)`
- `DECIMAL` 列 `SUM/AVG/MIN/MAX`
- 带 WHERE 的聚合
- 空结果聚合返回 `NULL`

状态计数预期：

- `executed_delta=4`
- `fallback_delta=0`
- `rows_delta=40`
- `workers_delta=4`

## 验证

已执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_aggregate \
  --parallel=1 --vardir=/tmp/pqv_agg --tmpdir=/tmp/pqt_agg
```

提交前已执行：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 完成报告

实现内容：

- 新增 no-debug MTR `pq_read_threaded_aggregate`。
- 验证 experimental threaded DOP=1 row stream 可支撑基础隐式聚合结果正确性。

风险点：

- 这不是 partial aggregation，不提供 worker-side aggregate 性能收益。
- 仍仅覆盖 DOP=1；DOP>1 range correctness 和多 worker 分片消费后续单独推进。

当前状态：Completed，完整 suite 已通过，等待提交。
