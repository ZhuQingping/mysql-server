# V2-8K Worker Thread Producer 任务书

## Goal

V2-8K 的目标是把 V2-8J 的同步 limited callback producer 推进为可并发的
worker thread producer，使 leader `PQTableScanIterator::Read()` 可以边消费
Exchange，worker 边通过 callback producer 写入 ROW/FINISH。

本阶段仍不默认打开真实 DOP=1 full scan。第一步只允许在 debug gate 下验证
worker thread + Exchange + `Read()` 的并发闭环。

## Why This Phase Is Needed

V2-8J 已能在 debug-only shadow path 中完成 2-row + EOF：

- InnoDB callback producer 可连续产出 row；
- SQL-owned `PQ_row_sink` 可写入 typed ROW；
- Exchange 可 materialize ROW/EOF；
- `Read()` 已有 bounded wait/kill policy。

但 V2-8J producer 是同步执行：leader 在 `Init()` 中先生产，再进入 `Read()`。
如果把 `max_rows` 直接改为全表，MQ ring 可能被填满，producer 会在 leader 消费
前阻塞。因此真实 full scan 需要 worker producer 与 leader consumer 并发。

## Scope

允许：

- 给 `PQ_worker_info` 增加真实 worker thread 句柄和最小同步状态；
- 扩展 `PQ_worker_manager::start()/wait()/abort()/cleanup()`，让 debug-gated
  worker producer 可启动、等待、清理；
- 复用 `pq_create_worker_thd()` / `pq_destroy_worker_thd()` 创建 worker THD；
- 让 worker entry 调用 `pq_worker_scan_callback_produce()`，通过
  `PQ_row_sink` 写 Exchange；
- leader `Read()` 继续使用 V2-8J bounded wait/kill policy；
- 新增 debug-only MTR，先验证小表完整 row count、EOF 和 counters。

禁止：

- 禁止默认打开真实 PQ full scan；
- 禁止在没有 join/cleanup 保障时让 worker thread detach 后遗留；
- 禁止 worker-side WHERE/projection/Item/JOIN clone；
- 禁止启用 `pq_worker_scan_next()` pull 路线；
- 禁止在 worker start 后静默 serial fallback；
- 禁止让同步 producer 生产未知数量全表 row。

## Design Requirements

### Worker Thread Ownership

- `Gather_operator` 仍是 worker lifecycle owner；
- 每个 `PQ_worker_info` 至少需要记录：
  - worker id；
  - worker THD；
  - worker open context；
  - worker context；
  - thread handle / join state；
  - terminal status and error code。
- worker thread 退出前必须：
  - 调用 `pq_worker_scan_end()`；
  - 关闭 worker TABLE；
  - 销毁 worker THD；
  - 发送 FINISH 或 ERROR；
  - 设置 terminal status。

### Producer / Consumer Contract

- worker producer 使用 push-style callback route：
  `handler::pq_worker_scan_callback_produce()`；
- sink 写入 `Exchange_nosort::enqueue_record_image()`；
- 正常结束发送 `enqueue_finish_smoke()` 或等价 FINISH helper；
- 发生错误发送 typed ERROR；
- leader `Read()` 在 WOULD_BLOCK 时 bounded wait，再检查 kill。

### Kill / Abort

- leader kill 后：
  - `propagate_kill_to_workers()` 设置 worker terminal intent；
  - MQ consumer abort 使 producer send 看到 detach；
  - worker thread 应尽快退出；
  - cleanup 必须 join。

### Debug Gate

第一阶段只增加新的 debug flag，例如：

```text
pq_read_threaded_shadow_path
```

该 flag 与默认 `pq_read_shadow_path` 分开，避免把同步 2-row shadow 与线程
producer 混淆。

## Proposed Implementation Split

### V2-8K-1: Thread Lifecycle Design/Scaffold

输出：

- 选定 MySQL 线程创建 API；
- `PQ_worker_info` thread handle 字段；
- start/wait/cleanup 可编译；
- 不接 InnoDB producer。

验收：

- `mysqld` build 通过；
- 完整 `parallel_query` suite 通过；
- 新增 smoke 能启动/等待空 worker。

### V2-8K-2: Threaded Callback Producer Debug Path

输出：

- worker entry 打开 worker THD/TABLE/handler；
- 调用 callback producer 写 Exchange；
- leader shadow `Read()` 消费；
- 小表完整 row count MTR。

验收：

- debug-only `pq_read_threaded_shadow_path` 下：
  - `SELECT *` fixed-row 小表返回完整行；
  - fallback 不增加；
  - executed 增加 1；
  - rows_scanned 等于表行数；
  - worker launched 增加；
  - full `parallel_query` suite 通过。

### V2-8K-3: Error/Kill/EOF Hardening

输出：

- worker ERROR token MTR；
- leader kill/abort MTR；
- EOF 和 cleanup 幂等验证。

验收：

- worker fatal 不 fallback；
- leader kill 能 join worker；
- debug build 无 assertion。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_shadow_dop1 pq_worker_dop1 --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

## Current Status

- Status: Planned
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

待实现后补充。
