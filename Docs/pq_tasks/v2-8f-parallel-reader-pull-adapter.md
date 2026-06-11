# V2-8F Parallel_reader Pull Adapter 任务书

## Goal

V2-8F 的目标是为 PQ worker 提供一个 pull-style InnoDB row adapter：

- 复用 upstream `Parallel_reader` 的 clustered cursor traversal 和 visibility 语义；
- 使用 leader active read view 做 consistent-read visibility；
- 使用 worker-local `row_prebuilt_t` template 做 MySQL record conversion；
- 暂不接入 SQL `PQTableScanIterator::Read()`，不打开真实 row stream。

本阶段的产物应是可编译、可测试的 InnoDB 内部 primitive，而不是完整并行执行。

## Current Baseline

- V2-8D 已选择 `Parallel_reader` visibility adapter 路线；
- V2-8E 已实现 `PQ_leader_scan_mode::EXECUTE` read-view commit point；
- `pq_worker_scan_next()` 仍 disabled；
- `row0pread_pq` 中旧 `row_search_mvcc()` pull adapter 只作为 latent disabled code，
  不作为真实执行路线。

## Design Direction

优先复用上游代码，而不是复制完整 cursor/mtr/latch 实现：

1. 从 `Parallel_reader::Scan_ctx` 抽出或新增一个受控 public/internal helper；
2. helper 输入为：
   - clustered `dict_index_t *`；
   - leader `trx_t *`，要求 `trx->read_view` active；
   - `[start,end)` range，V2-8F first gate 仅支持 whole range；
   - worker `row_prebuilt_t *` 和 `mysql_rec`；
3. helper 内部复用：
   - start positioning / page cursor traversal；
   - `check_visibility()`；
   - `row_vers_build_for_consistent_read()`；
   - `row_sel_store_mysql_rec()` conversion；
4. helper 输出：
   - `0 + eof=false` 表示拿到一行；
   - `0 + eof=true` 表示 range exhausted；
   - handler error 表示 fatal。

## Hard Gates

- leader `trx->read_view` 必须 active；
- `select_lock_type == LOCK_NONE`；
- clustered index only；
- DOP=1 whole range only；
- no BLOB/TEXT/JSON/GEOMETRY first gate，直到 per-worker blob heap 和 row image
  ownership 被完整验证；
- 不允许 worker-local `trx_assign_read_view()`；
- 不允许共享 leader `row_prebuilt_t` mutable cursor state；
- 不允许设置 `PQ_execution_state::EXECUTED`。

## Implementation Plan

### Step 1: Internal Helper Skeleton

- 在 `row0pread_pq` 中新增 helper 类型或函数名，先不接 `pq_worker_scan_next()`；
- helper 构造时检查 leader read view active；
- helper 明确 whole-range/DOP=1 限制；
- helper 注释引用 `Parallel_reader` visibility adapter 路线。

Status: Implemented first gate primitive. `InnoDB_pq_scan_ctx` now exposes
`has_active_read_view()` and `validate_pull_adapter_gate()`, which check
clustered index, leader active read view, and a single whole range without
reading rows.

### Step 2: Cursor / Visibility Extraction

- 评估能否把 `Parallel_reader::Scan_ctx::check_visibility()` 和相关 cursor traversal
  从 private 改成内部可复用 helper；
- 如果必须改 `row0pread.h/cc`，只暴露最小 internal API；
- 不把 `Scan_ctx` 大量私有结构直接公开给 SQL/handler。

### Step 3: Conversion Smoke

- 用 worker prebuilt template 和 `row_sel_store_mysql_rec()` 转换一行；
- 只在 internal smoke 中验证，不进入 SQL iterator `Read()`；
- 继续保持 `pq_worker_scan_next()` disabled，直到 EOF/error/MQ/materialization 都补齐。

## Allowed Files

- `storage/innobase/include/row0pread.h`
- `storage/innobase/row/row0pread.cc`
- `storage/innobase/include/row0pread_pq.h`
- `storage/innobase/row/row0pread_pq.cc`
- `storage/innobase/handler/ha_innodb.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8f-parallel-reader-pull-adapter.md`

## Forbidden Files

- SQL iterator `Read()` real row path；
- `PQ_execution_state::EXECUTED` / execution counters；
- optimizer eligibility expansion；
- worker-side Item/JOIN clone；
- DOP>1 real execution；
- bulk copy of upstream `Parallel_reader` internals。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] pull adapter internal API 第一段完成；
- [x] leader active read view gate 明确；
- [x] whole-range DOP=1 first gate 明确；
- [ ] 不公开 `Parallel_reader::Scan_ctx` 大量私有状态；
- [ ] `pq_worker_scan_next()` 仍 disabled；
- [x] build 和完整 `parallel_query` suite 通过。

## Current Status

- Status: Gate primitive completed
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

已完成第一段 gate primitive：

- `InnoDB_pq_scan_ctx::has_active_read_view()`；
- `InnoDB_pq_scan_ctx::validate_pull_adapter_gate()`；
- 不读取 row，不接 `pq_worker_scan_next()`，不改变执行状态。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

结果：`mysqld` build 通过；`pq_worker_dop1` 通过；完整 `parallel_query` suite
19 项通过。
