# V2-9E Experimental DOP2 Gate 任务书

## 目标

V2-9E 目标是在 V2-9A/B/C/D 已通过后，新增一个默认 OFF 的实验变量，用于无 debug flag 打开 DOP=2 threaded full scan。

本阶段只打开 DOP=2：

- DOP=1 继续由 `parallel_query_experimental_threaded_dop1` 控制；
- DOP=2 由新的 `parallel_query_experimental_threaded_dop` 控制；
- DOP>2 仍不支持；
- `pq_worker_scan_next()` 继续 disabled。

## 改动

- 新增 session sysvar `parallel_query_experimental_threaded_dop`，默认 OFF；
- `PQTableScanIterator::should_enter_threaded_read_shadow_path()` 分离 DOP1 / DOP2 gate：
  - DOP1: `parallel_query_experimental_threaded_dop1` 或 debug `pq_read_threaded_shadow_path`
  - DOP2: `parallel_query_experimental_threaded_dop` 或 debug `pq_read_threaded_dop2_shadow_path`
- `pq_vars` 增加新变量默认值、SET/RESET 和 global 可见性覆盖；
- 新增 `pq_read_threaded_dop2_experimental_var`，验证 no-debug DOP2 threaded path。
- 新增 `pq_read_threaded_dop2_aggregate`，验证 no-debug DOP2 row stream 可被上层 MySQL 聚合算子消费。

## 验证

新增 `pq_read_threaded_dop2_experimental_var`：

- `parallel_default_dop=2`
- `parallel_query_experimental_threaded_dop=ON`
- 不设置 debug flag
- `SELECT id, val FROM t1 WHERE pad >= 'a'`
- 预期：
  - `executed_delta=1`
  - `fallback_delta=0`
  - `rows_delta=5`
  - `workers_delta=2`

新增 `pq_read_threaded_dop2_aggregate`：

- `parallel_default_dop=2`
- `parallel_query_experimental_threaded_dop=ON`
- 不设置 debug flag
- 覆盖 `COUNT/SUM/AVG/MIN/MAX`、DECIMAL 聚合、带 WHERE 聚合和空结果聚合
- 预期：
  - `executed_delta=4`
  - `fallback_delta=0`
  - `rows_delta=40`
  - `workers_delta=8`

回归组：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars \
  pq_read_threaded_dop2_guard \
  pq_read_threaded_dop2_experimental_var \
  pq_read_threaded_dop2_multirange \
  --parallel=1 --vardir=/tmp/pqv_dop2_gate_group --tmpdir=/tmp/pqt_dop2_gate_group
```

完整 suite：

```bash
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full6 --tmpdir=/tmp/pqt_full6
```

结果：

- `mysqld` build 通过；
- DOP2 gate 相关回归组通过；
- `pq_read_threaded_dop2_aggregate` 单测通过；
- 完整 `parallel_query` suite 通过，共 35 项。

## 风险

- 该变量仍是 experimental gate，默认 OFF；
- 当前仅允许 DOP=2，不扩到 DOP=4；
- 真实覆盖范围仍限 clustered full scan、leader-side WHERE/projection、基础 row stream，不覆盖 ORDER BY/GROUP BY/secondary index/ICP/partition table。

当前状态：Completed，等待提交。
