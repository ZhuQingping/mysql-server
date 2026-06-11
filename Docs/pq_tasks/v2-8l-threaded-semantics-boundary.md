# V2-8L Threaded Semantics Boundary 任务书

## Goal

V2-8L 的目标是在 V2-8K debug-only threaded DOP=1 full scan 基础上，验证并修复
默认启用前最关键的 SQL 语义边界：

- projection 必须返回真实列值；
- leader-side WHERE 必须基于 materialized row 正确过滤；
- WHERE 返回空结果不能被当作 worker error；
- 空表必须正常 EOF；
- zero-row PQ execution 也应记录为 executed，而不是悄悄变成未执行。

本阶段仍不默认打开真实 DOP=1 full scan。

## Scope

允许：

- 修复 worker TABLE read/write bitmap 与 handler scan lifecycle；
- 修复 threaded producer 的 empty EOF 行为；
- 修复 EOF-before-first-row 的 executed counter；
- 新增 debug-only MTR 验证 projection/WHERE/empty table。

禁止：

- 禁止默认启用真实 PQ full scan；
- 禁止打开 DOP>1；
- 禁止 worker-side WHERE/projection/Item/JOIN clone；
- 禁止启用 `pq_worker_scan_next()` pull 路线。

## Completion Report

已完成：

- `pq_open_worker_table()` 在 worker TABLE 打开后同步 leader `read_set` /
  `write_set`，并调用 `column_bitmaps_set_no_signal()`，确保 InnoDB worker
  prebuilt/template 按 leader 需要列构建；
- threaded worker producer task 增加 `ha_rnd_init(true)` / `ha_rnd_end()`，
  让 InnoDB 按正常 table scan handler lifecycle 填充 MySQL row buffer；
- threaded producer 允许 0 row natural EOF，不再把空表当作失败；
- `PQTableScanIterator::Read()` 在 EOF-before-first-row 时也设置
  `PQ_execution_state::EXECUTED` 并增加 `Parallel_queries_executed`；
- 新增 `pq_read_threaded_projection_where` MTR，覆盖：
  - `SELECT *` 完整 row image；
  - projection；
  - leader-side WHERE；
  - WHERE 空结果；
  - 空表 EOF；
  - counters: `executed=5`、`fallback=0`、`rows=20`、`workers=5`。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_projection_where pq_read_threaded_shadow_dop1 pq_read_threaded_worker_error pq_read_threaded_abort_cleanup --parallel=1 --vardir=/tmp/pqv_semantics_group --tmpdir=/tmp/pqt_semantics_group
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

结果：

- `mysqld` build 通过；
- threaded semantics + hardening 组合通过；
- 完整 `parallel_query` suite 24 个测试通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Next

- 继续保持 debug-only gate；
- 默认启用前仍需更真实的 external `KILL QUERY` 测试和 DOP>1 range correctness；
- `pq_worker_scan_next()` 继续 disabled。
