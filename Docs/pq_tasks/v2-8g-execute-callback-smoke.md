# V2-8G EXECUTE Callback Smoke 任务书

## Goal

V2-8G 的目标是在不打开真实 `PQTableScanIterator::Read()` row stream 的前提下，
把 V2-8E 的 `EXECUTE` read-view commit point 和 V2-8F 的
`Parallel_reader` callback conversion primitive 串成一个可观测 smoke：

- SQL iterator 仍在 safe fallback window 内；
- leader 用 `PQ_leader_scan_mode::EXECUTE` 绑定 statement read view；
- worker 打开独立 `THD/TABLE/handler/prebuilt`；
- handler 尝试把 callback row 转换到 worker `TABLE::record[0]`；
- 无论 smoke 是否实际转换到 row，查询仍回到 serial fallback；
- 不设置 `PQ_execution_state::EXECUTED`，不增加真实执行计数。

## Scope

允许修改：

- `sql/handler.h`
- `storage/innobase/handler/ha_innodb.h`
- `storage/innobase/handler/ha_innodb.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/*.test`
- `mysql-test/suite/parallel_query/r/*.result`
- `Docs/pq_tasks/README.md`

禁止修改：

- `pq_worker_scan_next()` 真实 row pull；
- `PQ_execution_state::EXECUTED`；
- `Parallel_queries_executed` / `Parallel_workers_launched` /
  `Parallel_rows_scanned` 真实执行计数；
- optimizer eligibility 扩展；
- worker-side WHERE/projection/Item/JOIN clone；
- DOP>1 真实执行。

## Design

新增 handler virtual API：

```c++
pq_worker_scan_callback_smoke(PQ_Worker_context *worker_ctx,
                              uchar *record,
                              bool *converted)
```

InnoDB 实现中，该 API 从 typed worker context 取回 leader scan context，
调用 `InnoDB_pq_scan_ctx::smoke_callback_conversion()`。该路径只用于内部
smoke，不作为 public pull-row API。

`Gather_operator::run_worker_callback_conversion_smoke()` 负责：

1. 创建 worker THD；
2. 打开 worker TABLE；
3. 调用 `pq_worker_scan_init()`；
4. 记录 `Parallel_callback_smoke_attempts`；
5. 调用 handler callback smoke；
6. 清理 worker ctx、TABLE、THD。

SQL iterator 在已有 PROBE smoke 完成后，尝试创建一个固定 DOP=1 的
`EXECUTE` leader context 来运行该 smoke。由于该能力还只是观测点，
callback smoke 失败不会让用户查询报错；查询继续 serial fallback。

## Observability

新增状态变量：

- `Parallel_callback_smoke_attempts`：handler callback smoke 被尝试的次数；
- `Parallel_callback_smoke_rows`：callback 实际转换到 row 的次数。

当前测试只把 attempts 作为硬验收。`Parallel_reader` 在小表、空表或特定
range/context 形态下可能没有 callback row，因此 rows 暂不作为硬验收。
后续进入真实 row stream 时再把 row conversion 变成必须通过的 gate。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] handler callback smoke API 已增加；
- [x] InnoDB typed worker context 到 callback conversion primitive 已串联；
- [x] SQL iterator 内部 EXECUTE smoke 不破坏 serial fallback；
- [x] `pq_worker_scan_next()` 仍 disabled；
- [x] `EXECUTED` 和真实执行计数未打开；
- [x] attempts/rows 状态变量可观测；
- [x] `pq_worker_dop1` 通过；
- [x] 完整 `parallel_query` suite 通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Completion Report

实现内容：

- 增加 `handler::pq_worker_scan_callback_smoke()` virtual API；
- 增加 `ha_innobase::pq_worker_scan_callback_smoke()`；
- 增加 `Gather_operator::run_worker_callback_conversion_smoke()`；
- 在 `PQTableScanIterator::Init()` 的 fallback-safe 阶段尝试固定 DOP=1
  EXECUTE callback smoke；
- 新增 `Parallel_callback_smoke_attempts` 和
  `Parallel_callback_smoke_rows`；
- 更新 `pq_worker_dop1` 和 `pq_stats` 测试预期。

验证结果：

```text
cmake --build build-ninja --target mysqld -j 16
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed, 19 tests successful
```

剩余风险：

- `Parallel_callback_smoke_rows` 当前可能为 0，说明还没有把 callback
  row conversion 作为真实 row stream 的硬 gate；
- callback smoke 失败当前不影响用户查询，后续真实执行阶段需要将
  unsupported/fatal/OOM 分层处理；
- 真实 `Read()` 接管、EOF/error/MQ row image 仍未打开。
