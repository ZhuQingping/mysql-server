# V2-8M Experimental Threaded Gate 任务书

## Goal

V2-8M 的目标是把 V2-8K/V2-8L 的 debug-only threaded DOP=1 full scan
提升为默认 OFF 的实验变量保护路径，允许在没有 debug build / debug flag 的
情况下显式测试真实 threaded DOP=1 执行。

默认行为必须保持不变：只设置 `parallel_query=ON` 仍保持既有 V1/V2 fallback
边界，不自动进入真实 threaded execution。

## Scope

允许：

- 新增 session/global system variable；
- 将 `PQTableScanIterator` threaded path gate 扩展为 debug flag 或实验变量；
- 新增 MTR 验证 sysvar 默认 OFF、可设置、无 debug 下真实执行。

禁止：

- 禁止默认启用真实 DOP=1 full scan；
- 禁止 DOP>1；
- 禁止改变 V1 fallback/EXPLAIN 语义；
- 禁止启用 `pq_worker_scan_next()` pull 路线。

## Completion Report

已完成：

- 新增 `parallel_query_experimental_threaded_dop1` system variable；
- 默认值为 OFF，session/global 可见，可按 session 设置；
- `PQTableScanIterator::should_enter_threaded_read_shadow_path()` 接受：
  - debug flag `pq_read_threaded_shadow_path`；或
  - `@@session.parallel_query_experimental_threaded_dop1=ON`；
- 仍要求 `parallel_default_dop=1`、无 BLOB、fixed record image gate；
- 新增 `pq_read_threaded_experimental_var` MTR，验证无 debug flag 下：
  - projection/WHERE 返回正确结果；
  - `executed_delta=1`；
  - `fallback_delta=0`；
  - `rows_delta=5`；
  - `workers_delta=1`；
- 更新 `pq_vars` 覆盖新变量默认值、session 设置和 global 可见性。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars pq_read_threaded_experimental_var pq_read_threaded_projection_where --parallel=1 --vardir=/tmp/pqv_experimental_group --tmpdir=/tmp/pqt_experimental_group
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

结果：

- `mysqld` build 通过；
- 变量、实验路径、语义边界组合通过；
- 完整 `parallel_query` suite 25 个测试通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Next

- 保持默认 OFF；
- 外部 `KILL QUERY` 测试、DOP>1 range correctness 和 aggregation real path 继续后移；
- `pq_worker_scan_next()` 继续 disabled。
