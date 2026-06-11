# V2-9 DOP Range Correctness 任务书

## 目标

V2-9 目标是从 V2-8 的 DOP=1 threaded full scan，推进到 DOP>1 clustered full scan correctness。

本阶段的核心验收不是性能，而是正确性：

- 多 worker 不重复读；
- 多 worker 不漏读；
- leader read view 一致；
- worker 独立 TABLE/handler/prebuilt 生命周期正确；
- `KILL` / ERROR / EOF 仍可收敛资源；
- 默认路径仍受实验变量保护。

## 当前基线

已完成：

- `Parallel_reader::export_scan_ranges()` 可导出 deep-copy range boundary；
- `InnoDB_pq_scan_ctx::partition()` 已保存 exported ranges；
- `Parallel_ranges_built` 可观测；
- SQL 层 Exchange 多队列 synthetic ROW/FINISH 已通过 V2-8P；
- DOP=1 threaded row stream 可返回 `SELECT *`、projection、WHERE、empty EOF 和基础聚合；
- DOP=2 在 experimental threaded gate 下仍 serial fallback，见 V2-8Q。

未完成：

- `pq_worker_scan_init()` 仍拒绝 `open_ctx->actual_dop != 1`；
- `Gather_operator::run_worker_callback_threaded_producer()` 仍要求 `m_dop == 1`；
- `PQTableScanIterator::Init()` 的 EXECUTE path 仍硬编码 DOP=1；
- `InnoDB_pq_scan_ctx::produce_callback_rows()` 使用 `Parallel_reader::Scan_range{}`，会全表扫描，未绑定 worker range；
- `pq_worker_scan_next()` 继续 disabled。

## 必须拆分

### V2-9A Range Dispatch Contract

目标：只建立 worker_id 到 range 的只读映射，不读 row。

要求：

- 在 InnoDB worker ctx 中记录 assigned range id；
- DOP>N ranges 时定义分配策略；
- ranges<N workers 时空 worker 必须发送 FINISH；
- 新增 status：
  - `Parallel_ranges_dispatched`
  - `Parallel_empty_worker_ranges`
- MTR 只验证 dispatch observable，不打开真实 DOP>1 row stream。

### V2-9B Range-Aware Callback Producer

目标：让 callback producer 接收 worker assigned range，并只扫描该 range。

要求：

- 不使用 `pq_worker_scan_next()`；
- 继续走 push-style `PQ_row_sink`；
- 不共享 leader `row_prebuilt_t`；
- worker TABLE/handler/prebuilt 仍由 worker THD 打开；
- `Parallel_reader::Scan_range` 必须由 exported start/end tuple 构造；
- 若无法直接从 `InnoDB_pq_range` 构造 `Scan_range`，先新增 narrow adapter，不手写 B+tree cursor。

### V2-9C SQL Threaded DOP>1 Shadow Path

目标：debug-only DOP>2 threaded path，多个 worker 并发写 Exchange，leader 消费。

要求：

- 先 debug-only，不受 `parallel_query_experimental_threaded_dop1` 控制；
- `PQTableScanIterator::Init()` 可通过 DBUG flag 打开；
- workers launched delta 等于 DOP；
- ROW/FINISH/ERROR/KILL 资源回收路径必须覆盖。

### V2-9D Multi-range Callback Drain

目标：修复 `ranges > workers` 时每个 worker 只消费一个 range 导致潜在漏读的问题。

要求：

- worker 完成当前 assigned range 后继续从 leader ctx 原子领取下一个 range；
- 每个 range 最多被一个 worker 领取；
- 全部 range 被消费后 worker 才发送 FINISH；
- `Parallel_ranges_dispatched` 覆盖所有已领取 range；
- MTR 必须验证完整 row 集合、`rows_delta`、`workers_delta` 和 `ranges_dispatched >= 2`。

### V2-9E Experimental DOP>1 Gate

目标：在 V2-9A/B/C 通过后，新增独立默认 OFF 变量。

建议变量：

- `parallel_query_experimental_threaded_dop`

要求：

- 默认 OFF；
- 与 DOP=1 变量分离；
- 初始只允许 DOP=2；
- 后续再扩 DOP=4。

## 必须测试

最小 MTR：

- `pq_read_threaded_dop2_shadow_fullscan`
  - `COUNT(*)`
  - `SUM(id)`
  - `MIN/MAX(id)`
  - `BIT_XOR(id)` 如可用
  - 与串行结果一致
- `pq_read_threaded_dop2_shadow_projection_where`
  - projection
  - leader-side WHERE
  - empty WHERE
- `pq_read_threaded_dop2_shadow_empty_table`
- `pq_read_threaded_dop2_shadow_worker_error`
- `pq_read_threaded_dop2_shadow_external_kill`

状态计数：

- `executed_delta=1`
- `fallback_delta=0`
- `workers_delta=2`
- `rows_delta=表行数`
- `ranges_dispatched_delta >= 2`

## 禁止事项

- 禁止直接放开 `parallel_query_experimental_threaded_dop1` 支持 DOP>1；
- 禁止让多个 worker 各自执行 full scan；
- 禁止启用 `pq_worker_scan_next()`；
- 禁止手写低层 B+tree cursor traversal；
- 禁止接入 secondary index / ICP / partition table / BLOB；
- 禁止把 partial aggregation 和 DOP>1 row correctness 混在同一提交。

## 当前状态

只读 Explorer 结论已确认：

- `Parallel_reader::Exported_ranges` 足以作为 worker range dispatch 基础，边界是 deep-copy tuple，语义为 `[start, end)`；
- 当前 callback producer 会全表重复扫，因为 `produce_callback_rows()` 固定使用 `Parallel_reader::Scan_range{}`；
- 最小实现不需要新增 handler API，现有 `PQ_Worker_context` 可携带 assigned range；
- 必须先做 assigned/exhausted/produced observable，再放开 DOP>1；
- 禁止启用 latent `pq_worker_scan_next()` / `row_search_mvcc()` pull path。

当前状态：V2-9A/B/C 已完成；V2-9D multi-range callback drain 已完成验证；下一步再评估 V2-9E experimental DOP>1 gate。
