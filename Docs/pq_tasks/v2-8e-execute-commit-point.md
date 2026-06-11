# V2-8E EXECUTE Commit Point 任务书

## Goal

V2-8E 建立真实 PQ row stream 前的 no-fallback commit point。

目标不是打开 `Read()` 真实并行输出，而是让 InnoDB leader 能区分：

- `PROBE`: fallback-safe，只做分区/能力探测，不绑定 statement read view；
- `EXECUTE`: 已决定不能 transparent fallback，leader 在当前线程创建并 pin
  statement read view，供后续 `Parallel_reader` visibility adapter 只读使用。

## Current Baseline

- V2-8D 已选择 `Parallel_reader` visibility adapter 路线；
- worker-local `row_search_mvcc()` 不作为真实执行路线；
- `pq_worker_scan_next()` 仍 disabled；
- `PQTableScanIterator::Init()` 当前仍只调用 `PROBE`，然后 serial fallback。

## Design Requirements

1. `PROBE` 行为不变：不能启动事务、不能分配 read view、失败可 fallback。
2. `EXECUTE` 必须在 leader handler 所属线程中运行。
3. `EXECUTE` 仅允许 `select_lock_type == LOCK_NONE` 的 consistent read。
4. `EXECUTE` 调用 `trx_start_if_not_started()` 和 `trx_assign_read_view()`，确保
   `trx->read_view` active。
5. `EXECUTE` 成功后将 `m_prebuilt->sql_stat_start` 置为 false，避免后续同一
   prebuilt 再尝试 statement-start 分配。
6. `EXECUTE` 成功不等于真实 PQ executed；只有 worker row path 成功启动后才能设置
   `PQ_execution_state::EXECUTED` 和执行类 counters。
7. `EXECUTE` 当前不由 SQL iterator 调用；它只是下一步 pull adapter 接入前的
   InnoDB commit point primitive。

## Implementation Steps

### Step 1: InnoDB EXECUTE Bind

- 修改 `ha_innobase::pq_leader_scan_init()`：
  - `PROBE` 保持原行为；
  - `EXECUTE` 允许通过前置 gate；
  - 在创建 leader context 前绑定 read view；
  - unsupported locking read / reverse / non-clustered 仍返回 unsupported。

### Step 2: SQL Commit Point Hook

后续任务，不在本步打开：

- `PQTableScanIterator::Init()` 在所有 fallback-safe smoke 成功后调用 EXECUTE；
- EXECUTE 成功后启动 worker row path；
- worker start 后失败走 fatal/error，不再 serial fallback。

### Step 3: Pull Adapter

后续任务：

- 基于 `Parallel_reader` 抽出 DOP=1 pull cursor；
- 使用 leader active read view 做 visibility；
- 使用 worker prebuilt template 做 record conversion；
- 通过 V2-8B MQ row image 发回 leader。

## Allowed Files

- `storage/innobase/handler/ha_innodb.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8e-execute-commit-point.md`

## Forbidden Files

- `PQTableScanIterator::Read()` real row materialization；
- `pq_worker_scan_next()` real row return；
- `PQ_execution_state::EXECUTED` / execution counters；
- DOP>1 range execution；
- worker-side Item/JOIN clone。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] `PROBE` 仍不绑定 read view；
- [x] `EXECUTE` 能在 leader 线程绑定 active read view；
- [x] 当前 SQL iterator 仍不调用 EXECUTE；
- [x] `pq_worker_scan_next()` 仍 disabled；
- [x] `parallel_query` suite 通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

已实现 InnoDB leader `EXECUTE` commit point primitive：

- `PROBE` 行为保持 fallback-safe；
- `EXECUTE` 仅允许 `LOCK_NONE` consistent read；
- `EXECUTE` 在 leader 线程调用 `trx_start_if_not_started()` 和
  `trx_assign_read_view()`，并将 `m_prebuilt->sql_stat_start` 置为 false；
- 当前 SQL iterator 仍不调用 `EXECUTE`，真实 worker row stream 未打开。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

结果：`mysqld` build 通过；`pq_worker_dop1` 通过；完整 `parallel_query` suite
19 项通过。
