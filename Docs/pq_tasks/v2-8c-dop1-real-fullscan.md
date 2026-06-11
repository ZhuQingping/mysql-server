# V2-8C DOP=1 Real Full Scan 任务书

## Goal

V2-8C 是 V2-8 第一个真实 row execution gate。目标是在极窄范围内打开
single-table InnoDB clustered full scan 的 DOP=1 真实路径：worker 使用独立
mutable scan state 读取 row，按 V2-8B typed row image protocol 发送给 leader，
leader materialize 到 `TABLE::record[0]`，上层 SQL executor 继续执行
WHERE/projection。

本阶段只允许 DOP=1、single range、no BLOB/TEXT/JSON/GEOMETRY、clustered full scan。
DOP>1 range correctness 留给 V2-8D。

## Current Baseline

- Baseline commit: `db14645bcf4 Add PQ V2-8B row image protocol`
- V2-8A 已完成 worker handler/prebuilt/read-view contract design。
- V2-8B 已完成 typed MQ row image protocol 和 synthetic materialization smoke。
- InnoDB `pq_worker_scan_init()` / `pq_worker_scan_next()` 仍返回 unsupported；
  `PQTableScanIterator::Read()` 仍委托 serial fallback。

## Hard Gates

1. 真实执行只能在 `actual_dop == 1` 且 single range 时打开。
2. `TABLE_SHARE::blob_fields > 0` 必须 fallback 或 reject real PQ execution。
3. `PQ_Worker_context` 必须有真实 polymorphic wrapper，禁止 `reinterpret_cast`。
4. worker 不能共享 leader `ha_innobase::m_prebuilt` / `row_prebuilt_t`。
5. probe 阶段不能绑定 read view；execute commit point 后不能再 transparent fallback。
6. read view 必须由 leader 在 no-fallback boundary 创建/pin，并在 worker 停止后释放。
7. 真实 row path 成功启动后，才允许更新 `PQ_execution_state::EXECUTED`、
   `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_rows_scanned`。

## Required Design Before Coding

V2-8C 编码前必须先完成两个只读确认：

### Explorer A - Worker Open / TABLE Handler Boundary

Scope:

- `sql/handler.h`
- `sql/handler.cc`
- `sql/table.h`
- `sql/sql_base.cc`
- `sql/parallel_query/pq_handler.*`
- `sql/parallel_query/sql_parallel.*`

Output:

- worker 独立 `TABLE` / handler 的最小打开/关闭路径；
- 是否可安全复用现有 `handler::clone()`，以及为什么仅 clone handler 不够；
- V2-8C 最小 worker open context；
- cleanup 顺序和失败路径。

### Explorer B - InnoDB Worker Wrapper / DOP=1 Scan

Scope:

- `storage/innobase/handler/ha_innodb.cc`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/row/row0sel.cc` 中必要的 `row_search_mvcc()` 边界

Output:

- `InnoDB_pq_sql_worker_context final : public PQ_Worker_context` wrapper 设计；
- DOP=1 whole-index full scan 如何安全调用现有 `row_search_mvcc()`；
- read view / trx 共享或 clone 的可行性与风险；
- `pq_worker_scan_end()` 如何消除 `reinterpret_cast`；
- V2-8C 不能做的 DOP>1 range boundary。

## Implementation Plan

### Task 1: Contract Types

- 在 SQL/handler PQ boundary 增加 worker open context；
- `PQ_Worker_context` 保持 SQL-visible base class；
- InnoDB 新增 typed wrapper，拥有或引用 worker internal ctx；
- 移除 worker end path 的 `reinterpret_cast`。

### Task 2: Execute Commit Point

- `pq_leader_scan_init()` 区分 probe 与 execute mode；
- probe 只建可释放上下文，不绑定 read view；
- execute mode 在 fallback 不再允许的点绑定 read view；
- commit point 后错误走 fatal，不 serial fallback。

### Task 3: DOP=1 Worker Row Path

- 只允许 `actual_dop == 1`；
- 只 dispatch 一个 whole-index range；
- worker 读取 row 到 worker-local record buffer；
- 通过 V2-8B typed row image protocol 发送给 leader；
- leader `Read()` materialize 后返回 `0`。

### Task 4: Counters / State

- 成功启动真实 row path 后设置 `PQ_execution_state::EXECUTED`；
- 正确更新 executed/workers_launched/rows_scanned；
- fallback path 不污染 real execution counters；
- post-start error 不增加 fallback counter。

### Task 5: MTR

新增或扩展：

- `pq_fullscan_real_dop1`
  - `SELECT *` 与 serial 结果一致；
  - simple WHERE/projection 与 serial 结果一致；
  - `Parallel_queries_executed` 增加；
  - `Parallel_queries_fallback` 不增加；
  - `Parallel_rows_scanned > 0`；
  - `Parallel_workers_launched >= 1`。
- `pq_fullscan_blob_fallback`
  - BLOB/TEXT/JSON 表不走 real PQ execution；
  - 仍保持结果正确。

## Allowed Files

- `sql/parallel_query/pq_handler.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/pq_iterator.*`
- `sql/handler.h`
- `storage/innobase/handler/ha_innodb.*`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `mysql-test/suite/parallel_query/**`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8c-dop1-real-fullscan.md`

## Forbidden Files

- optimizer eligibility expansion unrelated to V2-8C gates；
- worker-side Item/JOIN clone；
- GROUP BY/ORDER BY/JOIN/secondary index/ICP/partition real execution；
- DOP>1 real scan；
- upstream `Parallel_reader` large refactor；
- reference implementation bulk import。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_fullscan_real_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [ ] 两个 V2-8C explorer 完成；
- [ ] worker context typed wrapper 完成；
- [ ] probe/execute mode 和 commit point 完成；
- [ ] DOP=1 single range gate 完成；
- [ ] BLOB/TEXT/JSON real execution gate 完成；
- [ ] leader `Read()` 真实 materialize row；
- [ ] post-start error 不 fallback；
- [ ] counters/state 只在真实 row path 更新；
- [ ] `pq_fullscan_real_dop1` 通过；
- [ ] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Planned
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

待实现后补充。
