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

Status: Completed in contract/gate step. Real worker row production remains
disabled.

### Task 2: Execute Commit Point

- `pq_leader_scan_init()` 区分 probe 与 execute mode；
- probe 只建可释放上下文，不绑定 read view；
- execute mode 在 fallback 不再允许的点绑定 read view；
- commit point 后错误走 fatal，不 serial fallback。

Status: Partially completed. `pq_leader_scan_init()` now has explicit
`PROBE`/`EXECUTE` mode. Current iterator still calls `PROBE` and serial
fallback; `EXECUTE` remains unsupported until the read-view commit point is
implemented.

### Task 3: DOP=1 Worker Row Path

- 只允许 `actual_dop == 1`；
- 只 dispatch 一个 whole-index range；
- worker 读取 row 到 worker-local record buffer；
- 通过 V2-8B typed row image protocol 发送给 leader；
- leader `Read()` materialize 后返回 `0`。

Status: Partially gated. `pq_worker_scan_init()` now rejects non-DOP=1,
non-single-range, BLOB, shared TABLE/record, and non-current-handler contexts,
but still returns unsupported before real row production.

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

## Read-Only Confirmation Results

### Worker Open / TABLE Handler Boundary

- V2-8C worker 必须由 worker THD 打开完整独立 `TABLE`，不要手工浅拷贝
  `TABLE`，也不要只 clone handler。
- 最小可行路径是 worker side 构造独立 `Table_ref`，使用 `open_ltable()` 或
  `open_table()` 走正常 server open path。该路径会经 `open_table_from_share()`
  构造独立 `TABLE`、独立 `record[0]/record[1]`、独立 `Field`、独立 bitmaps 和
  独立 handler。
- `handler::clone()` 不能作为 V2-8C 主路径：它会创建新 handler，但
  `new_handler->ha_open(table, ...)` 仍传入同一个 `TABLE*`，因此不会得到独立
  `record[0]`、`read_set`、`write_set`、`read_set_internal`、`m_status` 或绑定到新
  record buffer 的 Field。
- `PQ_Worker_open_context` 应定义在 SQL-visible PQ 边界，建议字段：

```cpp
struct PQ_Worker_open_context {
  THD *worker_thd;
  TABLE *worker_table;
  handler *worker_handler;
  TABLE *leader_table;
  PQ_Leader_context *leader_ctx;
  uint worker_id;
  uint actual_dop;
  MQueue_handle *mq_handle;
};
```

- 生命周期建议：
  - `Gather_operator` / `PQ_worker_manager` 拥有 worker metadata 和 open context
    carrier；
  - worker `TABLE` 生命周期由 worker THD open table list 拥有，使用
    `close_thread_tables(worker_thd)` 释放；
  - InnoDB `PQ_Worker_context` 只引用 worker `TABLE` / handler，不拥有它们。
- cleanup 顺序：
  1. leader 停止消费时先 abort/close MQ producer；
  2. 等 worker 退出 row read loop；
  3. 调 `worker_handler->pq_worker_scan_end(worker_ctx)`；
  4. worker THD 调 `close_thread_tables(worker_thd)`；
  5. 释放 worker THD / MEM_ROOT / `PQ_worker_info`；
  6. 所有 worker 停止后，leader 调 `pq_leader_scan_end(leader_ctx)`。
- V2-8C 最小 fallback gate：
  - `actual_dop == 1`；
  - leader ctx 只有 single range；
  - `table->s->blob_fields == 0`；
  - `worker_table != leader_table`；
  - `worker_table->file != leader_table->file`；
  - `worker_table->record[0] != leader_table->record[0]`；
  - typed `PQ_Worker_context` wrapper 已生效；
  - probe 阶段未绑定 read view；
  - worker start/execute commit point 前失败才允许 fallback。

### InnoDB Worker Wrapper / DOP=1 Scan Boundary

- InnoDB 侧应新增
  `InnoDB_pq_sql_worker_context final : public PQ_Worker_context`，仿照
  `InnoDB_pq_sql_leader_context`。wrapper 内部拥有 `InnoDB_pq_worker_ctx`，引用
  leader ctx、worker `THD`、worker `TABLE`、worker `ha_innobase`，并记录
  worker id、assigned range、EOF/error/started/closed 状态，保证 end 幂等。
- wrapper 必须使用 worker handler 的独立 `m_prebuilt`，不能共享 leader
  `m_prebuilt`。`row_prebuilt_t` 不可 copy/move，且包含 cursor/search tuple/mysql
  template/blob heap/back pointers 等 mutable state。
- `pq_worker_scan_end()` 当前对 `PQ_Worker_context*` 做
  `reinterpret_cast<InnoDB_pq_worker_ctx*>` 是 V2-8C 前必须消除的 hard gate。
- `pq_worker_scan_init()` / `next()` / `end()` 最小改动方向：
  - 验证 leader ctx 是 typed InnoDB leader wrapper；
  - V2-8C 只允许 `actual_dop == 1 && n_ranges == 1`；
  - 创建 typed worker wrapper，并在内部创建 `InnoDB_pq_worker_ctx`；
  - `pq_worker_scan_next()` 从 typed wrapper 取 worker handler `m_prebuilt`；
  - `pq_worker_scan_end()` 释放 internal ctx 并从 tracking 容器移除，避免
    `pq_leader_scan_end()` 二次释放。
- DOP=1 可以使用 `row_search_mvcc()`，但不能沿用当前
  `InnoDB_pq_ctx::read_record()` 的 first-call 写法。当前 first call 使用
  `PAGE_CUR_UNSUPP + ROW_SEL_EXACT`，不等价于 `index_first()`。V2-8C first row
  必须走 `index_first()` 等价定位路径，后续再使用 `ROW_SEL_NEXT`。
- read view / trx gate：
  - worker 不能各自创建 read view；
  - 不能让不同 OS thread 盲目共享 leader `trx_t`；
  - 若当前没有安全机制把 worker-local trx 绑定到 leader-pinned statement
    snapshot 或等价 snapshot，则必须 fallback；
  - `select_lock_type != LOCK_NONE` 必须 fallback；
  - isolation/statement 状态不是普通 consistent read 时必须 fallback。
- V2-8C 绝对不做：
  - DOP>1 real scan；
  - `InnoDB_pq_range::m_start/m_end` range seek/end cut；
  - 并发 range scheduler；
  - no-duplicate/no-missing correctness；
  - 手写低层 `btr_pcur_t` cursor/mtr/latch scan；
  - secondary index、ICP、partition、reverse scan、ORDER/GROUP/JOIN、
    worker-side Item/JOIN clone。

## Contract/Gate Implementation Summary

- `sql/parallel_query/pq_handler.h`
  - Added `PQ_Worker_open_context` as the SQL-visible carrier for worker THD,
    worker TABLE, worker handler, leader TABLE/context, worker id, DOP, and MQ
    handle.
  - Added `PQ_Worker_context_kind` and virtual `PQ_Worker_context::kind()` so
    engine cleanup can identify typed contexts without RTTI.
- `sql/handler.h`
  - Changed `pq_worker_scan_init()` to take `PQ_Worker_open_context *`.
  - Changed `pq_leader_scan_init()` to take `PQ_leader_scan_mode`.
- `sql/parallel_query/pq_iterator.cc`
  - Current fallback-safe bridge call now explicitly uses
    `PQ_leader_scan_mode::PROBE`.
- `storage/innobase/handler/ha_innodb.cc`
  - Added `InnoDB_pq_sql_worker_context final : public PQ_Worker_context`.
  - Added `PQ_leader_scan_mode` handling. `PROBE` keeps the existing
    fallback-safe partition probe; `EXECUTE` returns unsupported until
    worker TABLE open and read-view ownership are proven safe.
  - Changed `pq_worker_scan_end()` from `reinterpret_cast` to
    `kind() == INNODB` plus typed `static_cast`.
  - Added conservative contract gates in `pq_worker_scan_init()`:
    `actual_dop == 1`, current worker handler, current leader ctx,
    `m_pq_leader_ctx->n_ranges() == 1`, no BLOB fields, independent worker
    TABLE, and independent worker `record[0]`.
- No real worker TABLE open, read-view pinning, InnoDB row read, or
  `PQ_execution_state::EXECUTED` update was enabled in this step.

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

- [x] 两个 V2-8C explorer 完成；
- [x] worker context typed wrapper 完成；
- [ ] probe/execute mode 和 commit point 完成；已具备 mode API，commit point 未打开；
- [x] DOP=1 single range gate 完成；
- [x] BLOB/TEXT/JSON real execution gate 完成；
- [ ] leader `Read()` 真实 materialize row；
- [ ] post-start error 不 fallback；
- [ ] counters/state 只在真实 row path 更新；
- [ ] `pq_fullscan_real_dop1` 通过；
- [x] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Contract/Gate Implemented
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Confirmed: 2026-06-11
- Contract/Gate Implemented: 2026-06-11
- Agents:
  - `Meitner`: Worker Open / TABLE Handler Boundary completed
  - `Hilbert`: InnoDB Worker Wrapper / DOP=1 Scan completed

## Completion Report

V2-8C 两个只读确认已完成，且 contract/gate 第一段已实现。当前代码已经具备
`PQ_Worker_open_context`、typed InnoDB worker wrapper、DOP=1/single-range/blob/
independent TABLE/record gate，并清除了 `pq_worker_scan_end()` 的
`reinterpret_cast` hard gate。

真实 DOP=1 full scan 仍未打开。后续必须继续完成 probe/execute mode、read-view
commit point、worker THD 完整 open TABLE、first row `index_first()` 等价定位、
leader `Read()` 真实 materialization 和 post-start fatal error path。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

结果：`mysqld` build 通过；完整 `parallel_query` suite 通过，18 个测试加
`shutdown_report` 共 19 项成功。
