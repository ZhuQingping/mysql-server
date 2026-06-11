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

Status: Completed for smoke scope. 已新增 smoke-only producer loop skeleton：worker metadata
进入 RUNNING，发送 typed FINISH，leader 通过 Exchange 观察 EOF，worker 进入
FINISHED。该路径不创建 OS worker thread，不读 InnoDB row。

Status: In progress. 已新增 typed ERROR producer smoke：worker metadata 进入
RUNNING，发送 typed ERROR，leader 通过 Exchange 观察 expected error，worker 进入
ERROR。该 expected error 被 smoke 内部消费，不影响用户查询。

Status: Completed for smoke scope. 已新增 leader abort producer smoke：worker
metadata 进入 RUNNING，leader 调用 `abort_workers()`，MQ consumer side detach，
leader 通过 Exchange 观察 EOF，worker 进入 ABORTED。该 abort 被 smoke 内部
消费，不影响用户查询。

### V2-8J-3: `Read()` Shadow Path

目标：

- `PQTableScanIterator::Read()` 具备不可达/受控 shadow path；
- 使用 runtime state、Exchange helper 和 cleanup 逻辑；
- 默认仍走 serial fallback。

Status: Completed for scaffold scope. 已新增 debug-only `pq_read_shadow_path`
gate；默认不可达。shadow path 在 DOP=1 fixed-row 表上可绕过 serial fallback，
进入 `PQ_STARTED` no-fallback 状态，并让 `Read()` 通过
`Exchange_nosort::materialize_next_record_image()` 处理 ROW/EOF/ERROR。
首次真实 ROW 返回时才设置 `EXECUTED` 并递增真实执行/行计数。当前仍没有真实
worker producer，因此默认测试路径继续 serial fallback。

Status update: shadow path 已增加 PROBE-supported guard；如果 PROBE 返回
unsupported，即使 debug gate 打开也继续 serial fallback，避免 EXECUTE 越过
fallback-safe 边界。已尝试把既有 callback conversion row 重新放入持久
Exchange 供 shadow `Read()` 消费，但普通 `SELECT *` 仍会走 fallback，说明
DOP=1 real gate 前还需要收敛 PROBE/range gate 和真实 producer 入口。

### V2-8J-4: DOP=1 Real Full Scan Gate

目标：

- 显式 gate 下打开真实 DOP=1；
- `SELECT *`、simple WHERE、projection 与串行一致；
- `Parallel_queries_executed=1`、fallback 不增加、rows/workers 计数合理。

当前阻塞：

- 普通 `SELECT *` 在 debug shadow gate 下仍可能因 PROBE unsupported 走
  serial fallback；
- callback conversion primitive 当前最多产出一行，不能作为 full scan；
- `materialize_next_record_image()` 已有 bounded wait/kill policy 边界，但尚未
  接真实异步 worker；
- 打开 DOP=1 前必须先补一个受控 producer，把多行 ROW/FINISH 放入持久
  `m_gather`，并定义 no-message 时的 wait/kill 语义。

Status update: 已新增 `Parallel_probe_attempts` /
`Parallel_probe_success` / `Parallel_probe_unsupported` 诊断计数。`pq_worker_dop1`
现在验证 DOP=1/2/4 执行路径会同时观察到 PROBE success 和 unsupported，
用于后续定位 DOP=1 real gate 卡点。

Status update: 已新增
`Exchange_nosort::materialize_next_record_image_status()`，把 ROW、EOF、
WOULD_BLOCK、ERROR 显式拆开。旧 `materialize_next_record_image()` 保持兼容；
shadow `Read()` 已切到 status helper。真实异步 worker 仍需在 WOULD_BLOCK
外层补 wait/kill policy。

Status update: 已拆出 `Exchange_nosort::enqueue_record_image()` 作为 ROW-only
producer helper；旧 `enqueue_record_image_smoke()` 继续包装 ROW+FINISH。后续
多行 producer 可以连续发送 ROW，最后再发送 FINISH。

Status update: 已新增 push-style callback producer API：
`handler::pq_worker_scan_callback_produce()`、`PQ_row_sink` 和
`InnoDB_pq_scan_ctx::produce_callback_rows()`。它沿用 Parallel_reader callback
路线推送多行，不启用 `pq_worker_scan_next()` pull 路线。当前仅完成 API 和
InnoDB 实现，尚未接入 SQL worker loop / Exchange sink。

Status update: 已把 callback producer API 接入 SQL 层 limited multi-row
producer smoke。`Gather_operator::run_worker_callback_multirow_producer_smoke()`
在 EXECUTE context 下创建 worker THD/TABLE/handler，使用 SQL-owned
`PQ_row_sink` 连续发送 2 条 ROW，再发送 FINISH；leader 通过
`materialize_next_record_image_status()` 消费 ROW/EOF，并把
`Parallel_callback_smoke_rows` 验收从 `>= 1` 提升到 `>= 3`。该路径仍是
smoke-only，不接默认 `Read()`，真实 DOP=1 full scan 仍未打开。

Status update: 已在 `PQTableScanIterator::Read()` 增加 bounded wait/kill
policy。`Exchange_nosort::wait_for_message()` 每次最多等待 1ms；`Read()`
在每轮 materialize 前检查 leader kill，遇到 `WOULD_BLOCK` 时等待后重试，
遇到 kill 则传播到 worker/MQ 并返回 kill error。该边界仍不启动真实异步
worker，但已经避免把暂时无消息误判为内部错误。

## Acceptance Checklist

- [x] callback row conversion smoke 能稳定产出 row；
- [x] callback row producer smoke 能稳定产出 ROW；
- [x] worker producer loop skeleton 有 FINISH/EOF 语义；
- [x] worker producer loop 有 ERROR 语义；
- [x] worker producer loop 有 abort 语义；
- [x] `Read()` shadow path 可编译、默认不可达；
- [x] callback multi-row producer smoke 接入 SQL worker loop / Exchange sink；
- [x] `Read()` wait/kill policy 边界已实现；
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
- 增加 `Exchange_nosort::enqueue_finish_smoke()`；
- callback conversion 成功后，worker record image 被发送为 typed MQ ROW；
- leader 通过 `materialize_next_record_image()` 消费该 row image；
- 增加 `Gather_operator::run_worker_producer_loop_smoke()`，验证
  RUNNING -> typed FINISH -> EOF -> FINISHED；
- 增加 `Gather_operator::run_worker_producer_error_smoke()`，验证
  RUNNING -> typed ERROR -> expected leader error -> ERROR；
- 增加 `Gather_operator::run_worker_producer_abort_smoke()`，验证
  RUNNING -> leader abort -> MQ detach -> EOF -> ABORTED；
- 增加 debug-only `PQTableScanIterator::Read()` shadow path scaffold，默认
  不可达，且受 PROBE-supported guard 保护；
- shadow path 使用 `mark_pq_started()` 锁定 no-fallback 边界，并仅在第一条
  ROW 返回时 `mark_pq_row_returned()` / `EXECUTED` / 真实计数；
- 增加 Exchange materialize status helper，明确 WOULD_BLOCK 与 EOF 的边界；
- 拆出 ROW-only record image enqueue helper，保留旧 smoke ROW+FINISH 语义；
- 增加 callback multi-row producer API 和 InnoDB 实现，仍不接执行路径；
- 增加 SQL 层 limited callback multi-row producer smoke，验证 2-row ROW +
  FINISH 经 Exchange 被 leader materialize；
- `pq_worker_dop1` 将 callback rows 下限提升到 3，覆盖 single-row
  conversion smoke + multi-row producer smoke；
- 增加 `Exchange_nosort::wait_for_message()` 和 shadow `Read()` bounded
  wait/kill loop，明确 WOULD_BLOCK 不等于内部错误；
- 新增 `Parallel_worker_producer_smoke_runs` 状态变量；
- 默认仍不接真实 `Read()`。

验证：

```text
cmake --build build-ninja --target mysqld -j 16
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed, 19 tests successful
```

下一步继续 V2-8J-4：在 debug gate 下验证真实 DOP=1 full scan，并决定是否
把 limited producer 从 smoke-only 推进到 shadow `Read()` producer；真实
DOP=1 full scan 仍未打开。
