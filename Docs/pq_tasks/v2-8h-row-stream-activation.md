# V2-8H Row Stream Activation Boundary 任务书

## Goal

V2-8H 的目标是为真实 `PQTableScanIterator::Read()` 接管做最后一层边界收敛：

- 明确 leader 从 Exchange 读取 ROW/FINISH/ERROR 的三态协议；
- 明确 row image materialization 到 `table->record[0]` 的所有权；
- 明确 no-data、EOF、ERROR、KILL、fatal-after-start 的处理差异；
- 先抽出可复用 helper，不打开真实 row stream。

## Current Baseline

- V2-8B 已有 typed MQ row image protocol；
- V2-8G 已能在 safe fallback window 内尝试 EXECUTE callback smoke；
- `PQTableScanIterator::Read()` 仍委托 serial iterator；
- `pq_worker_scan_next()` 仍 disabled；
- `Parallel_queries_executed/workers_launched/rows_scanned` 未打开。

## Design Requirements

### Helper Contract

`Exchange_nosort::materialize_next_record_image(TABLE *table, bool *eof, bool *row)`：

- 成功 materialize 一行：返回 `false, row=true, eof=false`；
- 所有 worker FINISH：返回 `false, row=false, eof=true`；
- 当前没有数据但尚未 EOF：返回 `false, row=false, eof=false`；
- ERROR token 或 row image 非法：返回 `true`；
- helper 不等待事件、不检查 KILL；真实 `Read()` 后续在外层加 wait/kill policy。

### Read() Gate

后续真正打开 `PQTableScanIterator::Read()` 前必须满足：

- worker 已经启动，不能再 serial fallback；
- `m_gather` 和 Exchange 已初始化；
- leader `m_leader_ctx` 为 `EXECUTE` context；
- 每条 ROW payload 已复制到 leader `table->record[0]` 后才能读取下一条 MQ message；
- EOF 后必须 wait/cleanup workers；
- ERROR/KILL 必须返回查询错误，不能静默 fallback；
- 只有第一条真实 ROW 成功返回后才允许设置 `PQ_execution_state::EXECUTED`
  和真实执行计数。

## Scope

允许修改：

- `sql/parallel_query/exchange.h`
- `sql/parallel_query/exchange.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8h-row-stream-activation.md`

禁止修改：

- `PQTableScanIterator::Read()` 真实 row path；
- worker thread launch；
- `pq_worker_scan_next()`；
- `PQ_execution_state::EXECUTED`；
- optimizer eligibility；
- InnoDB cursor/visibility 逻辑。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_exchange_rows_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] `materialize_next_record_image()` helper 已增加；
- [x] synthetic row image smoke 已复用 helper；
- [x] helper 明确 row/eof/error/no-data 语义；
- [x] 不打开 `PQTableScanIterator::Read()`；
- [x] 不改变真实执行计数；
- [x] build 和 MTR 通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Completion Report

实现内容：

- 增加 `Exchange_nosort::materialize_next_record_image()`；
- 将 `run_synthetic_row_image_smoke()` 改为复用该 helper；
- helper 显式区分 row/eof/no-data/error，不等待事件、不接 KILL policy；
- 不连接 `PQTableScanIterator::Read()`，不改变真实执行计数。

验证结果：

```text
cmake --build build-ninja --target mysqld -j 16
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query pq_exchange_rows_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed, 19 tests successful
```
