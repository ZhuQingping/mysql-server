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

### Thread / THD Decision

已完成只读调研，结论如下：

- 不能在 leader 线程调用 `create_internal_thd()` 后把 THD 交给另一个线程；
  `create_internal_thd()` 会 `store_globals()` 并绑定当前线程 PSI THD；
- 真实 PQ worker producer 必须先通过 `mysql_thread_create()` 创建 joinable
  worker thread，再在 worker thread 入口内部调用 `pq_create_worker_thd()`；
- worker thread 退出时必须在同一线程内调用 `pq_destroy_worker_thd()`；
- `THD::awake()` / `killed` 必须按 MySQL 模式持有 `LOCK_thd_data`；
- MVP 暂不把 worker THD 加入 `Global_THD_manager`，避免 processlist 生命周期和
  shutdown 等待语义扩大。

参考文件：

- `include/mysql/psi/mysql_thread.h`: `mysql_thread_create()`；
- `sql/sql_thd_internal_api.cc`: `create_internal_thd()` / `destroy_internal_thd()`；
- `sql/event_scheduler.cc`: event worker thread 创建和 THD lifecycle 范式；
- `sql/rpl_replica.cc`: replica worker start/wait/awake 范式；
- `storage/innobase/row/row0pread.cc`: `Parallel_reader` 内部 InnoDB worker
  thread，不能直接作为 SQL THD worker。

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

- Status: V2-8K-2 Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Last updated: 2026-06-11

## Completion Report

### V2-8K-1: Thread Lifecycle Design/Scaffold

已完成：

- 新增 `parallel_query_worker` PSI thread key；
- `PQ_worker_info` 增加 joinable worker thread handle、started/joined 状态；
- `PQ_worker_manager::start()` 使用 `mysql_thread_create()` 创建 scaffold
  worker thread；
- worker thread entry 在 worker 线程内调用 `my_thread_init()`、
  `pq_create_worker_thd()`、`pq_destroy_worker_thd()`、`my_thread_end()`；
- `PQ_worker_manager::wait()` / `cleanup()` join 已启动线程，避免 detach 遗留；
- `PQ_worker_info::m_status` 改为原子状态，`transition_status()` 使用 CAS，
  收敛 start/abort/future worker 并发状态迁移风险。

未打开：

- worker thread 尚不调用 InnoDB callback producer；
- 未新增 `pq_read_threaded_shadow_path`；
- 默认真实 DOP=1 full scan 仍未启用。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 pq_read_shadow_dop1 pq_exchange_rows_dop1 --parallel=1 --vardir=/tmp/pqv_thread --tmpdir=/tmp/pqt_thread
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

结果：

- `mysqld` build 通过；
- 关键 3 个 PQ 用例通过；
- 完整 `parallel_query` suite 20 个测试通过。

后续：

- V2-8K-3：补齐 worker ERROR、leader kill/abort、EOF/cleanup 幂等用例。

### V2-8K-2: Threaded Callback Producer Debug Path

已完成：

- 新增 `PQ_worker_task::CALLBACK_LIMITED_PRODUCER`，保持默认 worker
  lifecycle smoke 为 `NOOP`；
- worker thread entry 根据 task 调用 InnoDB callback producer，把 ROW 写入
  当前 `Gather_operator` 的 `Exchange_nosort`；
- worker thread 成功时发送 FINISH，失败时尝试发送 ERROR，并设置 worker
  terminal status；
- 新增 `Gather_operator::run_worker_callback_threaded_producer()`，只由
  debug-only path 调用；
- `PQTableScanIterator` 新增独立 debug gate
  `pq_read_threaded_shadow_path`，与同步 `pq_read_shadow_path` 分离；
- `Gather_operator::destroy()` 改为走 `PQ_worker_manager::cleanup()`，确保
  EOF/cleanup 路径会 join worker，再释放 leader context；
- 新增 MTR `pq_read_threaded_shadow_dop1`，验证 worker thread producer 与
  leader `Read()` 并发消费完整 8 行。

未打开：

- 默认真实 DOP=1 full scan 仍未启用；
- DOP>1 range-partition producer 未启用；
- worker-side WHERE/projection/Item/JOIN clone 未启用；
- `pq_worker_scan_next()` 继续 disabled。

验证：

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_read_threaded_shadow_dop1 --parallel=1 --vardir=/tmp/pqv_threaded --tmpdir=/tmp/pqt_threaded
TMPDIR=/tmp ./mtr --suite=parallel_query pq_worker_dop1 pq_read_shadow_dop1 pq_read_threaded_shadow_dop1 pq_exchange_rows_dop1 --parallel=1 --vardir=/tmp/pqv_threaded_related --tmpdir=/tmp/pqt_threaded_related
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_full --tmpdir=/tmp/pqt_full
```

结果：

- `mysqld` build 通过；
- 新增 threaded shadow 用例通过；
- 相关 shadow/worker/exchange 用例通过；
- 完整 `parallel_query` suite 21 个测试通过。

后续：

- V2-8K-3：增加 worker ERROR token、leader kill/abort、EOF/cleanup 幂等测试；
- 之后再评估是否把 debug-only threaded DOP=1 full scan 提升为受系统变量保护的
  实验执行路径。
