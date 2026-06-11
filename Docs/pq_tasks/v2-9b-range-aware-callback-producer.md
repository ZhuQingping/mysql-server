# V2-9B Range-Aware Callback Producer 任务书

## 目标

V2-9B 目标是把 InnoDB callback producer 从“总是 full scan”改为“按 worker assigned range 扫描”，为后续 DOP>1 correctness 做前置。

本阶段仍不打开 DOP>1：

- `pq_worker_scan_init()` 的 `actual_dop != 1` gate 保持；
- `Gather_operator::run_worker_callback_threaded_producer()` 的 `m_dop != 1` gate 保持；
- `PQTableScanIterator::Init()` 的 EXECUTE path 仍固定 DOP=1；
- 默认实验变量仍只允许 DOP=1。

## 改动

新增 latent helper：

- `InnoDB_pq_scan_ctx::produce_callback_rows_for_range(...)`

语义：

- `range != nullptr`：使用 `Parallel_reader::Scan_range(start,end)` 只扫描 assigned range；
- `range == nullptr`：表示 worker 没有 range，producer 成功但不发送 ROW，后续 worker 发送 FINISH；
- 旧 `produce_callback_rows()` 保持 full-range 兼容语义；
已确认并修复的 blocker：

- 直接把 `innodb_worker->assigned_range()` 传给 `Parallel_reader::Scan_range(start,end)` 会触发 InnoDB debug assertion：
  - `page0cur.cc:452:n <= searchable`
  - 调用栈位于 `Parallel_reader::Scan_ctx::create_ranges()` / `partition()` / `add_scan()`
- 根因是 PQ deep-copy 的 exported boundary tuple 保留了过多 compare fields；
- 修复：`InnoDB_pq_iter::assign()` 按 `dict_index_get_n_unique_in_tree(index)` clamp `n_fields_cmp`；
- handler 主路径已重新接入 `produce_callback_rows_for_range(... assigned_range)`。

## 验证

本阶段 DOP=1 下 assigned range 仍是 whole clustered range，因此结果应与 V2-8M/O 相同：

- projection/WHERE threaded path 正确；
- aggregate threaded path 正确；
- range dispatch observable 仍正确；
- DOP=2 experimental guard 仍 fallback。

验证命令：

```bash
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp ./mtr --suite=parallel_query \
  pq_read_threaded_projection_where \
  pq_read_threaded_aggregate \
  pq_range_dispatch_dop1 \
  pq_read_threaded_dop2_guard \
  --parallel=1 --vardir=/tmp/pqv_range_producer \
  --tmpdir=/tmp/pqt_range_producer

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## 风险

- 这还不是 DOP>1 correctness；
- `Parallel_reader::Scan_range(start,end)` 的 boundary correctness 需要 V2-9C/D 用 DOP=2/4 结果测试验证；
- 如果未来 ranges 少于 workers，empty worker 会只发 FINISH，SQL worker manager 必须允许 rows_sent=0 的 worker 正常完成。

验证结果：

- `cmake --build build-ninja --target mysqld -j 16` 通过；
- `pq_read_threaded_projection_where`、`pq_read_threaded_aggregate`、`pq_range_dispatch_dop1`、`pq_read_threaded_dop2_guard` 通过。

完整 suite：

- `TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full` 通过，29 个测试全部成功。

当前状态：Completed，等待提交。
