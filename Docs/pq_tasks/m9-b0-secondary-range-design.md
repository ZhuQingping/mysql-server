# M9-B0 Secondary Range Design And Guard

## 状态

Completed by Codex Orchestrator.

## 目标

M9-B0 只确认 secondary index range 正例的拆分边界和前置护栏，不打开真实 secondary range PQ 执行。

当前分支仍以 clustered full scan PQ 为主：

- SQL eligibility 只允许 `JT_ALL`，`JT_RANGE` / `JT_REF` / `JT_INDEX_SCAN` 均 fallback；
- access path factory 只在 `AccessPath::TABLE_SCAN` 尝试 PQ；
- `CopyRangeScanAccessPath()` 返回 `nullptr`；
- `PQblockScanIterator` / `PQRefIterator` 仍 fail-closed；
- InnoDB `pq_leader_scan_init()` 拒绝非 clustered index；
- `InnoDB_pq_scan_ctx::partition()` 使用空 `Parallel_reader::Scan_range{}`，等价 clustered full scan range export。

因此 M9-B 不应一次性打开 secondary index range 正例。

## 拆分结论

### M9-B0: Design And Guard

只做设计、状态记录和负向护栏。

允许修改：

- `Docs/pq_tasks/m9-b0-secondary-range-design.md`
- `Docs/pq_tasks/commercial-port-m9-ref-icp.md`
- `Docs/pq_tasks/commercial-port-gap-analysis.md`
- `Docs/pq_tasks/README.md`
- 必要时增强 `pq_commercial_ref_icp` negative MTR

禁止修改：

- `sql/handler.h`
- `sql/join_optimizer/access_path.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/pq_iterators.*`
- `sql/parallel_query/pq_clone.cc`
- `sql/range_optimizer/*`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/include/row0pread_pq.h`

### M9-B1: Secondary Range Candidate Probe

目标是 synthetic/probe-only，不产生真实 secondary row。

建议范围：

- SQL eligibility 识别极窄 secondary range candidate，但仍 fallback；
- access path 层记录 `INDEX_RANGE_SCAN` probe，不创建 PQ iterator；
- `CopyRangeScanAccessPath()` 只做 metadata clone/probe，失败时有状态变量；
- 不进入 InnoDB secondary scan。

建议状态变量：

- `Parallel_secondary_range_probe_attempts`
- `Parallel_secondary_range_probe_unsupported`
- `Parallel_secondary_range_clone_attempts`
- `Parallel_secondary_range_clone_failed`

### M9-B2: InnoDB Secondary Range Partition

目标是 InnoDB secondary range partition 正例的最小执行前置。

建议限制：

- 单表 InnoDB；
- 单 secondary index range；
- 正向扫描；
- 非 partition；
- 不做 ref / dependent ref；
- 不做 ICP；
- 先 DOP=1 或 hidden debug DOP；
- 先覆盖索引或明确回表路径二选一。

建议状态变量：

- `Parallel_secondary_ranges_built`
- `Parallel_secondary_rows_produced`

### M9-B3: Secondary Range Callback Row Production

目标是真实 row production 的最小正例。

必须先明确：

- secondary index tuple 到 MySQL record 的 materialization；
- 覆盖索引与非覆盖索引回表边界；
- MVCC read view 与 deleted-mark record 处理；
- end range / next-key 边界；
- KILL / worker error cleanup；
- 与 current clustered callback producer 的代码复用边界。

## 商用实现参考

商用仓库关键路径：

- `sql/parallel_query/pq_iterators.cc`: `PQblockScanIterator` / `PQRefIterator`
- `sql/parallel_query/sql_parallel.cc`: `SetupPQTab()` / `InitPQTab()`
- `storage/innobase/handler/ha_innodb_pq.cc`: `pq_range_scan_init()` / `pq_ref_build_ranges()`
- `storage/innobase/row/row0pread_pq.cc`: secondary index row production、ICP、cluster lookup

## 已有护栏

M9-A 已提交 `pq_commercial_ref_icp` negative MTR，覆盖：

- secondary ref fallback；
- secondary range + ICP on fallback；
- secondary range + ICP off fallback；
- secondary covering index scan fallback；
- multi-table ref fallback；
- actual SELECT 不增加 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_ranges_built`、`Parallel_ranges_dispatched`。

## Review 结论

M9-B 设计检视 Agent 确认：

- 不建议直接进入 secondary index range 正例编码；
- 下一步应先做 M9-B0 设计与护栏；
- SQL eligibility/access path、range clone、InnoDB secondary partition、callback row production 必须拆阶段推进；
- InnoDB secondary partition 与 callback row production 不应 synthetic 合并进同一编码阶段。

## 下一步

推荐进入 M9-B1：secondary range candidate probe。

M9-B1 完成前，不应修改 InnoDB secondary row production，也不应放开 `NON_FULL_TABLE_SCAN` fallback。
