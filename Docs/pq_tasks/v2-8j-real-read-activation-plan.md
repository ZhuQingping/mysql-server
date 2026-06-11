# V2-8J Real Read Activation Plan 任务书

## Goal

V2-8J 的目标是定义真实 DOP=1 full scan 打开的最小闭环和硬阻塞项。
本阶段先做设计收敛，不直接启用 `PQTableScanIterator::Read()`。

真实闭环必须满足：

1. `PQTableScanIterator::Init()` 到达 no-fallback commit point；
2. leader 使用 `PQ_leader_scan_mode::EXECUTE`；
3. worker 使用独立 THD/TABLE/handler/prebuilt；
4. worker 从 InnoDB 读取 row 并发送 typed ROW image；
5. leader `Read()` 通过 Exchange materialize 到 `table->record[0]`；
6. EOF/error/kill 可控；
7. 第一条真实 row 返回后才设置 `EXECUTED` 和真实执行计数。

## Current Blockers

### Blocker 1: Worker producer loop 尚未存在

当前 worker lifecycle smoke 只创建/销毁元数据，不启动 OS worker 线程，也没有生产
ROW/FINISH/ERROR token 的 loop。

需要新增：

- worker entry；
- worker state transition；
- handler row fetch loop；
- MQ send ROW/FINISH/ERROR；
- abort/kill 检查。

### Blocker 2: `pq_worker_scan_next()` 仍 disabled

当前 `pq_worker_scan_next()` 返回 unsupported。V2-8F/G 只证明 callback conversion
primitive 和 smoke attempts，不提供 pull-row API。

下一步可选路线：

- 路线 A：继续 `Parallel_reader` callback producer，把 callback row image 直接写 MQ；
- 路线 B：实现安全的 worker-local pull adapter，再由 worker loop 调
  `pq_worker_scan_next()`。

当前更稳妥的是路线 A，因为 read view/visibility 已选定 `Parallel_reader` route。

### Blocker 3: callback rows 还不是硬验收

`Parallel_callback_smoke_attempts` 已可观测，但 `Parallel_callback_smoke_rows`
当前可能为 0。打开真实执行前，必须先让 callback row conversion 在目标表上稳定产出。

Status: In progress. `smoke_callback_conversion()` 已从 pull adapter 的
whole-range gate 中拆出，不再因为 range planning 产出多个 range 而跳过
callback conversion smoke；目标是让 fixed-row InnoDB 表稳定观察到
`Parallel_callback_smoke_rows >= 1`。

Status: In progress. 已新增 `Exchange_nosort::enqueue_record_image_smoke()`，
callback conversion 成功后会把 worker record image 发送为 typed ROW，并由
leader 通过 `materialize_next_record_image()` 消费。该路径仍是 smoke-only，
不接 `PQTableScanIterator::Read()`。

### Blocker 4: fatal-after-start 还没有执行路径使用

V2-8I 已有 iterator runtime state，但当前没有调用 `mark_pq_started()` /
`mark_pq_row_returned()`。真实打开时必须：

- worker/MQ 启动成功后调用 `mark_pq_started()`；
- 第一条 row 返回前不能计 `EXECUTED`；
- 第一条 row 返回后调用 `mark_pq_row_returned()`；
- `PQ_STARTED` 后任何错误都不能 serial fallback。

## Proposed Split

### V2-8J-1: Callback Row Producer Smoke

目标：

- 不启动 SQL worker；
- 让 InnoDB 在 EXECUTE context 下通过 `Parallel_reader` callback 产出 row image；
- row image 走 typed MQ；
- leader 用 `materialize_next_record_image()` 消费；
- 仍不接 `PQTableScanIterator::Read()`。

验收：

- `Parallel_callback_smoke_rows >= 1` 对普通 fixed-row InnoDB 表稳定成立；
- `pq_exchange_rows_dop1` 或新增 `pq_callback_row_image_dop1` 通过；
- 完整 suite 通过。

Status: Completed for smoke scope. 当前 callback-converted worker record
image 已发送为 typed MQ ROW，leader 通过
`Exchange_nosort::materialize_next_record_image()` 消费。该路径仍不接
`PQTableScanIterator::Read()`。

### V2-8J-2: Worker Producer Loop Skeleton

目标：

- worker loop 只跑一个 controlled producer smoke；
- 发送 FINISH/ERROR；
- leader wait/cleanup 不死等；
- 不返回 row 给 SQL executor。

### V2-8J-3: `Read()` Shadow Path

目标：

- `PQTableScanIterator::Read()` 具备不可达/受控 shadow path；
- 使用 runtime state、Exchange helper 和 cleanup 逻辑；
- 默认仍走 serial fallback。

### V2-8J-4: DOP=1 Real Full Scan Gate

目标：

- 显式 gate 下打开真实 DOP=1；
- `SELECT *`、simple WHERE、projection 与串行一致；
- `Parallel_queries_executed=1`、fallback 不增加、rows/workers 计数合理。

## Acceptance Checklist

- [x] callback row conversion smoke 能稳定产出 row；
- [x] callback row producer smoke 能稳定产出 ROW；
- [ ] worker producer loop 有 FINISH/ERROR/abort 语义；
- [ ] `Read()` shadow path 可编译、默认不可达；
- [ ] DOP=1 real full scan MTR 通过；
- [ ] full `parallel_query` suite 通过；
- [ ] fatal-after-start 不 fallback。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

已完成 V2-8J-1 smoke scope：

- `InnoDB_pq_scan_ctx::smoke_callback_conversion()` 不再复用 pull adapter 的
  whole-range gate；
- callback conversion smoke 现在只要求 clustered index + active read view；
- `pq_worker_dop1` 已把 `Parallel_callback_smoke_rows >= 1` 作为硬验收；
- 增加 `Exchange_nosort::enqueue_record_image_smoke()`；
- callback conversion 成功后，worker record image 被发送为 typed MQ ROW；
- leader 通过 `materialize_next_record_image()` 消费该 row image；
- 仍不接真实 `Read()`。

验证：

```text
cmake --build build-ninja --target mysqld -j 16
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed, 19 tests successful
```

下一步进入 V2-8J-2：worker producer loop skeleton，补 FINISH/ERROR/abort
语义，但仍不返回 row 给 SQL executor。
