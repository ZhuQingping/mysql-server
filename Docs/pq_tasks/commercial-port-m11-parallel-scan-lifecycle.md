# M11-D ParallelScanIterator Lifecycle Contract Taskbook

## 状态

M11-D0 design-only completed / Docs-Design Review accepted。

## 目标

为商用 `ParallelScanIterator` 主生命周期平移建立最小合同。M11-D 不直接
打开 `PARALLEL_SCAN` 正路径，不接真实 handler/InnoDB worker scan，不接
cloned JOIN worker plan。第一阶段只明确 owner、cleanup、fallback
boundary、worker start commit point、Read/EOF/ERROR 语义，后续再按小步
编码。

## 当前事实

当前仓：

- `ParallelScanIterator` / `PQblockScanIterator` / `PQRefIterator` 已存在
  商用命名 class boundary；
- 三个 iterator 当前都是 fail-closed skeleton：`Init()` 返回失败，
  `Read()` 返回 error/EOF-like failure；
- `ParallelScanIterator` 当前只保存 raw pointer：`QEP_TAB`、`JOIN`、
  `Gather_operator`、`AccessPath`，没有明确 ownership / cleanup guard；
- 当前真实实验入口仍是 `pq_iterator.cc::PQTableScanIterator`，不是
  `pq_iterators.*`；
- `AccessPath::PARALLEL_SCAN` / `PQ_BLOCK_SCAN` / `PQ_REF_SCAN` 的 factory
  存在，但当前没有稳定正向生产入口，且对应 iterator fail-closed；
- 当前分支已有 `Gather_operator` worker lifecycle、`Query_result_mq`
  smoke worker、KILL/error/read-view 回归护栏。

商用仓：

- `AccessPath::PARALLEL_SCAN` 创建 `ParallelScanIterator`；
- `ParallelScanIterator::Init()` 初始化 gather/exchange、handler leader
  scan context、snapshot，并启动 workers；
- `Read()` 从 `MQ_record_gather` / `Exchange` 拉取 worker row；
- `End()` / destructor 负责 detach MQ、wait/join workers、merge worker
  error / examined rows / warnings；
- worker plan 中 `Query_result_mq` 挂到 cloned JOIN 的 query result；
- `PQblockScanIterator` / `PQRefIterator` 是 worker-side row source，最终由
  worker执行树上层 `Query_result_mq` 发送结果。

结论：M11-D 先迁 lifecycle contract，不迁真实 worker plan、handler/InnoDB
row source、ORDER/GROUP、ref/ICP、resource control。

## Lifecycle Contract

### Ownership

`ParallelScanIterator` 后续必须明确 owner：

- owns leader `Gather_operator` only if it allocates it；
- owns leader scan context only after successful handler `EXECUTE` init；
- does not own `JOIN` / `QEP_TAB` / `TABLE` / `AccessPath`；
- destructor must be safe after partial `Init()` failure；
- cleanup helper must be idempotent and callable from `Init()` failure,
  `Read()` EOF, `Read()` error, and destructor。

`PQblockScanIterator` / `PQRefIterator` 后续必须明确：

- worker-side iterator does not own leader `Gather_operator`；
- worker-side handler scan context is owned by the iterator after successful
  `pq_worker_scan_init()` and must be ended exactly once；
- `MQueue_handle` is borrowed from worker manager / exchange；
- no worker-side row source may return rows directly to SQL user result；worker
  output must flow through worker query result / MQ。

### Fallback Boundary

- before clone preflight, handler PROBE, and worker thread start, fallback may
  remain serial and counted as fallback；
- after handler `EXECUTE` succeeds and worker launch/row-stream attach begins,
  failure must not silently serial fallback；
- after any worker starts or any worker row is made visible to leader, error
  handling must abort/drain/wait and propagate ERROR/KILL according to priority；
- D1 compile-only skeleton must keep `Init()` fail-closed and must not alter
  current `PQTableScanIterator` fallback behavior。

### Init Contract

Future positive `ParallelScanIterator::Init()` order:

1. validate query shape and explicit gate；
2. run clone/worker-result preflight；
3. allocate/initialize `Gather_operator` / exchange；
4. initialize handler leader context in PROBE then EXECUTE mode；
5. attach worker plan / worker result sink；
6. launch workers；
7. mark no-fallback commit point only after worker launch succeeds。

Any failure before step 6 may fallback if no worker was started. Any failure at
or after step 6 must clean up and return an error, not serial fallback.

### Read Contract

Future positive `Read()`:

- reads only from gather/exchange / worker result adapter；
- maps ROW to leader record materialization；
- maps FINISH/EOF to `-1` after worker wait/cleanup；
- maps worker ERROR / leader KILL to an error return after abort/wait；
- must not call handler/InnoDB row scan directly in leader `Read()`；
- must not invoke current leader-local secondary/ref gates。

### Cleanup Contract

Cleanup helper requirements:

- safe to call multiple times；
- detach MQ/abort workers if leader exits before EOF；
- wait/join all started workers；
- call handler leader scan end if leader context was created；
- destroy owned `Gather_operator`；
- merge worker error priority before returning final error；
- preserve existing KILL > leader fatal/OOM > worker fatal > MQ closed >
  normal finish priority。

## M11-D0 Design Output

This taskbook is M11-D0 design output. It must be reviewed before coding.

## Suggested Split

### M11-D1: Compile-only Lifecycle Skeleton

目标：

- add explicit lifecycle state fields to `ParallelScanIterator`；
- add an idempotent private cleanup/end helper；
- exercise the cleanup helper from destructor and `Init()` failure paths even
  while it remains mostly no-op；
- document ownership for borrowed vs owned pointers in code comments；
- keep `Init()` fail-closed；
- keep `Read()` fail-closed；
- no factory/hook behavior change；
- no MTR behavior change。

Allowed files:

- `sql/parallel_query/pq_iterators.h`；
- `sql/parallel_query/pq_iterators.cc`；
- this taskbook。

Forbidden:

- `sql/parallel_query/pq_iterator.*`；
- `sql/parallel_query/sql_parallel.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*`；
- `sql/handler.*`；
- `storage/innobase/**`；
- `sql/parallel_query/pq_clone*`；
- optimizer eligibility files；
- MTR result/test files。

Validation:

```bash
cmake --build build-ninja --target mysqld -j 16

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_worker_result_adapter \
  pq_clone_diagnostics pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d1_target --tmpdir=/tmp/pqt_m11d1_target
```

Status: coding/validation completed / Code-Docs-Test Review accepted。

Implementation:

- added explicit `ParallelScanIterator` lifecycle state；
- added destructor and idempotent private cleanup helper；
- documented borrowed optimizer/executor pointers and current borrowed
  `Gather_operator` ownership；
- cleanup helper is exercised from `Init()` failure and destructor；
- `Init()` remains fail-closed；
- `Read()` remains fail-closed；
- no factory/hook behavior changed。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed: `pq_commercial_worker_result_adapter`
  `pq_clone_diagnostics` `pq_stats`, 4/4。

Code-Docs-Test Review:

- Review Agent returned `ACCEPT`；
- confirmed only `pq_iterators.*` and taskbook changed；
- confirmed `ParallelScanIterator::Init()` and `Read()` remain fail-closed；
- confirmed cleanup helper is called from `Init()` failure and destructor and is
  guarded by `m_cleanup_done`；
- confirmed current `Gather_operator` ownership stays borrowed and no
  incomplete/delete risk exists；
- non-blocking suggestion to explicitly mention base `TABLE` borrowed ownership
  was applied；
- post-review build and targeted MTR passed again。

### M11-D2: Lifecycle Status Smoke

Only after D1 review accepted. Add observability for fail-closed lifecycle
cleanup without enabling `PARALLEL_SCAN`. Exact counters/MTR must be designed
separately.

Revised design:

- D2 remains design-only / docs-only；
- do not add counters in D2 because there is no legal MTR trigger for
  `ParallelScanIterator::Init()` without either going through the
  `PARALLEL_SCAN` AccessPath factory or touching the current `pq_iterator.*`
  entry path；
- do not create a synthetic SQL-visible trigger in `pq_stats`；
- defer lifecycle counter implementation to D3, where a debug-only
  `PARALLEL_SCAN` construction probe can explicitly own the trigger boundary；
- D2 closes the status-smoke design question by recording that the smoke needs
  a guarded construction probe before it can be testable。

Counter semantics reserved for D3:

- `Parallel_scan_lifecycle_smoke_attempts`；
- `Parallel_scan_lifecycle_fail_closed`；
- `Parallel_scan_lifecycle_cleanup_calls`。

When D3 implements these counters, they must mean synthetic/debug lifecycle
smoke only. MTR must also assert that real execution counters and unrelated
smoke counters do not increase:

- `Parallel_queries_executed`；
- `Parallel_workers_launched`；
- `Parallel_rows_scanned`；
- handler probe/open/handler smoke counters；
- clone probe/preflight counters；
- worker-result smoke counters。

Allowed files for D2:

- this taskbook。

Forbidden for D2:

- `sql/parallel_query/pq_iterators.h`；
- `sql/parallel_query/pq_iterators.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/mysqld.cc`；
- `mysql-test/suite/parallel_query/t/pq_stats.test`；
- `mysql-test/suite/parallel_query/r/pq_stats.result`；
- `sql/parallel_query/pq_iterator.*`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*`；
- `sql/handler.*`；
- `storage/innobase/**`；
- `sql/parallel_query/pq_clone*`；
- optimizer eligibility files。

Validation:

```bash
git diff --check -- Docs/pq_tasks/commercial-port-m11-parallel-scan-lifecycle.md
```

Status: docs-only design closure completed / Docs-Design Review accepted。

Review:

- first Docs-Design Review returned `REVISE` because D2 had no legal MTR
  trigger for `ParallelScanIterator::Init()` without going through
  `PARALLEL_SCAN` factory or touching `pq_iterator.*`；
- D2 was narrowed to docs-only and records that lifecycle counters/testability
  require a guarded D3 construction probe；
- re-review returned `ACCEPT`；
- validation: document `git diff --check` passed。

### M11-D3: Guarded PARALLEL_SCAN Factory Probe

Only after D1/D2 accepted. Evaluate a debug-only `PARALLEL_SCAN` construction
probe. It must not start workers or call handler/InnoDB.

Design:

- D3 owns the first legal trigger for `ParallelScanIterator` lifecycle smoke；
- add a debug-only construction helper that builds a local
  `ParallelScanIterator` skeleton and calls `Init()`；
- the helper must not route through normal query AccessPath selection；
- the helper must not start worker threads, allocate `Gather_operator`, call
  handler/InnoDB, or call `Read()`；
- the helper lives in `pq_iterators.*`；
- a minimal DBUG-only hook in `PQTableScanIterator::Init()` may invoke the
  helper before handler PROBE, before `Gather_operator` allocation, and before
  the existing guarded smoke chain；
- after the DBUG hook runs, `PQTableScanIterator::Init()` must continue through
  the existing serial fallback behavior；
- the DBUG hook must be independently gated and must not run in normal
  execution；
- counters from D2 may be added only for synthetic/debug lifecycle smoke。

Required D3 counters:

- `Parallel_scan_lifecycle_smoke_attempts` increments when the synthetic helper
  is invoked；
- `Parallel_scan_lifecycle_fail_closed` increments when `Init()` returns the
  expected fail-closed result；
- `Parallel_scan_lifecycle_cleanup_calls` increments from the cleanup helper。

Required D3 MTR assertions:

- lifecycle smoke attempts delta >= 1；
- lifecycle fail-closed delta >= 1；
- cleanup calls delta >= 1；
- `Parallel_queries_executed` delta remains 0 for the lifecycle smoke window；
- `Parallel_workers_launched` delta remains 0；
- `Parallel_rows_scanned` delta remains 0；
- worker-result smoke worker counter does not change because D3 must not start
  the M11-B3c worker-result probe as part of this isolated assertion window；
- handler/InnoDB probe/open/handler counters do not change in the isolated
  D3 assertion window。

Allowed files for D3 coding:

- `sql/parallel_query/pq_iterators.h`；
- `sql/parallel_query/pq_iterators.cc`；
- `sql/parallel_query/pq_iterator.cc` only for a DBUG-only hook before handler
  PROBE in `PQTableScanIterator::Init()`；
- `sql/parallel_query/sql_parallel.h` only for stat fields and reset updates；
- `sql/mysqld.cc` only for SHOW STATUS exposure；
- one focused MTR test/result under `mysql-test/suite/parallel_query/`；
- this taskbook。

Potential hook:

- add `DBUG_EXECUTE_IF("pq_parallel_scan_lifecycle_smoke", ...)` before
  `PQTableScanIterator::Init()` calls handler `pq_leader_scan_init(PROBE)`；
- the hook calls only the `pq_iterators.*` synthetic lifecycle helper；
- the hook must not call the normal `PARALLEL_SCAN` AccessPath factory；
- the hook must not call the existing post-PROBE smoke chain。

Forbidden:

- worker thread launch；
- `Gather_operator` allocation；
- handler/InnoDB calls；
- `Query_result_mq` / worker result path；
- cloned JOIN / `pq_make_join()`；
- normal AccessPath `PARALLEL_SCAN` positive execution；
- `access_path.*` changes；
- serial fallback behavior changes；
- touching `storage/innobase/**`。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16

TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query <new_or_focused_d3_test> pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d3_target --tmpdir=/tmp/pqt_m11d3_target
```

Status: coding/validation completed / Code-Docs-Test Review accepted。

Review:

- first Docs-Design Review returned `REVISE` because D3 still lacked a legal
  SQL/MTR trigger；
- design was revised to allow a minimal DBUG-only `pq_iterator.cc` hook before
  handler PROBE；
- the hook must call only the `pq_iterators.*` synthetic lifecycle helper and
  then continue existing serial fallback behavior；
- re-review returned `ACCEPT`。

Implementation:

- added synthetic `pq_run_parallel_scan_lifecycle_smoke()` in
  `pq_iterators.*`；
- added a guarded `DBUG_EXECUTE_IF("pq_parallel_scan_lifecycle_smoke", ...)`
  hook in `PQTableScanIterator::Init()` before handler PROBE；
- the hook constructs a local `ParallelScanIterator`, calls `Init()`, verifies
  the current fail-closed result, then returns through existing serial
  fallback；
- added debug-smoke status counters:
  `Parallel_scan_lifecycle_smoke_attempts`,
  `Parallel_scan_lifecycle_fail_closed`,
  `Parallel_scan_lifecycle_cleanup_calls`；
- added focused MTR `pq_parallel_scan_lifecycle_smoke` and updated
  `pq_stats` for the new status variables。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_parallel_scan_lifecycle_smoke` and `pq_stats`, 3/3 including
  `shutdown_report`；
- full `parallel_query` suite passed, 81/81；
- `--record pq_parallel_scan_lifecycle_smoke` executed successfully but MTR
  failed to copy the new result file with errno 1, so the result file was
  synchronized from the generated test log and then verified by normal MTR。

Code-Docs-Test Review:

- Review Agent returned `ACCEPT`；
- confirmed the DBUG hook is before handler PROBE and returns through serial
  fallback without entering the existing post-PROBE smoke chain；
- confirmed the synthetic helper only constructs local `ParallelScanIterator`
  and calls `Init()`；
- confirmed cleanup counting is guarded by `m_cleanup_done` inside the cleanup
  helper；
- confirmed SHOW STATUS/reset wiring and MTR assertions are complete；
- confirmed no forbidden files or real `PARALLEL_SCAN` positive path were
  touched。

## Deferred

- real cloned JOIN execution；
- `make_pq_worker_plan()` positive path；
- worker-side `PQblockScanIterator` handler scan；
- worker-side `PQRefIterator` / ICP / dependent ref；
- `Exchange_sort` real ORDER BY；
- stable output rowid；
- full resource control / `parallel_max_threads` budget；
- `Field_raw_data` / `Batch_buffer` commercial row serialization。

### M11-D4: Positive Path Migration Design

Status: design completed / Docs-Design Review accepted。

Goal:

- define the smallest safe path from D3's synthetic fail-closed construction
  probe toward a guarded positive `ParallelScanIterator` execution path；
- do not edit source in D4；
- do not enable user-visible `PARALLEL_SCAN`；
- preserve current typed handler context, callback row sink, ROW/ERROR/FINISH
  worker-result frame, and explicit no-fallback commit point。

Commercial reference:

- commercial `ParallelScanIterator::Init()` creates `MQ_record_gather`,
  initializes `Gather_operator`, initializes handler/InnoDB scan state, creates
  snapshot state, and launches workers；
- commercial `Read()` pulls rows from MQ into leader record；
- commercial `End()` detaches MQ, waits workers, merges worker diagnostics, and
  returns the final error；
- commercial `PQblockScanIterator` worker side calls `pq_worker_scan_init()`,
  `ha_pq_next()`, and `pq_worker_scan_end()`；
- commercial `make_pq_worker_plan()` attaches `Query_result_mq` to cloned JOIN
  worker execution。

Current branch differences:

- handler/InnoDB contract is typed:
  `PQ_Leader_context` / `PQ_Worker_open_context` / `PQ_Worker_context`；
- current worker row model is callback-produce through `PQ_row_sink`, not
  commercial pull-row `ha_pq_next()`；
- current worker-result protocol already has explicit ROW/ERROR/FINISH frames；
- current commit point is represented by `PQTableScanIterator::mark_pq_started()`
  and must remain the only point after which serial fallback is forbidden；
- current `ParallelScanIterator` remains fail-closed and is only exercised by
  D3's debug-only lifecycle smoke。

Design decisions:

- do not directly copy commercial `void *scan_ctx` handler contract；
- do not migrate commercial `PQblockScanIterator::Read()` pull-row loop in
  the next coding step；
- prefer callback-produce + `PQ_row_sink` for early worker row production；
- use current `Query_result_mq` ROW/ERROR/FINISH frames instead of commercial
  EOF-via-`my_eof()` semantics；
- keep normal execution fallback before EXECUTE/worker-start；
- after EXECUTE succeeds and a worker thread is started, cleanup/error
  propagation must replace silent serial fallback。

Recommended coding split after D4:

#### M11-D4a: Worker Attach Contract Smoke

Scope:

- add a debug-only helper that opens a worker TABLE/handler using existing
  `PQ_Worker_open_context`；
- call `pq_worker_scan_init()` and `pq_worker_scan_end()` under a bounded
  smoke；
- do not produce rows；
- do not create cloned JOIN；
- do not call `ParallelScanIterator::Read()`；
- assert worker context cleanup and no user-visible execution counters。

Candidate files for D4a taskbook refinement:

- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterators.*` only if the hook must live near
  `ParallelScanIterator`；
- `sql/parallel_query/pq_iterator.cc` only for an explicit DBUG hook before
  positive execution；
- focused MTR/status updates。

D4a coding taskbook must replace this candidate list with strict
`Allowed files` and `Forbidden files` before any source edit. It must also
define worker TABLE open/close ownership, whether existing worker-open helpers
are reused, and which failures may still serial fallback before worker start.

#### M11-D4b: Leader Row Stream Adapter Smoke

Scope:

- reuse current `Query_result_mq` / `PQWR` adapter and `Exchange_nosort`
  materialization；
- use controlled worker output, not cloned JOIN；
- leader consumes ROW/FINISH and materializes a small fixed row set through
  current row-image/record materialization path；
- no default user-visible PQ。

#### M11-D4c: Commit-point / Cleanup Error Smoke

Scope:

- model EXECUTE + worker-start as the no-fallback commit point；
- inject worker ERROR / leader abort / empty range cases；
- verify abort, detach/close MQ, wait/join workers, worker scan end, leader scan
  end, and Gather/Exchange destroy order；
- assert serial fallback counter does not increment after the commit point。

Forbidden until D4a-D4c pass:

- default `ParallelScanIterator` positive `Init()`；
- real `make_pq_worker_plan()`；
- `ExecuteIteratorQuery()` inside worker；
- `PQblockScanIterator::Read()` pull-row positive path；
- secondary range/ref/ICP/dependent ref；
- ORDER BY real merge；
- GROUP BY commercial aggregation；
- partition/reverse/hash join/stable sort；
- direct commercial `Field_raw_data` / `Batch_buffer` serialization。

Validation required for each coding subtask:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query <focused_test> pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d4_target --tmpdir=/tmp/pqt_m11d4_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11d4_full --tmpdir=/tmp/pqt_m11d4_full
```

Docs-Design Review:

- Review Agent returned `ACCEPT`；
- confirmed D4 reflects the commercial/current-branch differences around
  typed handler context, callback row sink, explicit ROW/ERROR/FINISH frames,
  and the existing no-fallback commit point；
- confirmed D4a/D4b/D4c split avoids directly enabling default
  `PARALLEL_SCAN`；
- confirmed forbidden list blocks direct `pq_make_join()` positive path,
  `PQblockScanIterator` pull-row positive path, ORDER/GROUP/ref/ICP, and other
  broad commercial paths；
- non-blocking review notes require D4a to define strict allowed/forbidden
  files and worker TABLE open/close ownership before source edits。

### M11-D4a: Worker Attach Contract Smoke Taskbook

Status: taskbook completed / Docs-Design Review accepted。

Goal:

- add an isolated debug-only smoke for the worker attach contract that D4
  selected；
- open an independent worker TABLE/handler through the existing
  `PQ_Worker_open_context` path；
- call `pq_worker_scan_init()` and `pq_worker_scan_end()` exactly once on the
  worker handler；
- verify cleanup and observability without producing rows；
- keep normal execution fallback before any worker-start commit point。

Existing primitives to reuse:

- `Gather_operator::configure_worker_open_contexts()` fills
  `PQ_Worker_open_context` from leader table and leader context；
- `pq_create_worker_thd()` / `pq_destroy_worker_thd()` own the worker THD；
- `pq_open_worker_table()` / `pq_close_worker_table()` own the worker SQL TABLE
  open/close；
- `ha_innobase::pq_worker_scan_init()` creates a typed
  `PQ_Worker_context` only when worker TABLE/handler independence gates pass；
- `ha_innobase::pq_worker_scan_end()` is idempotent and releases the typed
  worker context。

Ownership rules:

- `Gather_operator` owns `PQ_worker_info` and its `PQ_Worker_open_context`
  carrier；
- `pq_create_worker_thd()` owns worker THD setup until `pq_destroy_worker_thd()`；
- `pq_open_worker_table()` owns worker TABLE acquisition until
  `pq_close_worker_table()`；
- `pq_worker_scan_init()` owns the returned `PQ_Worker_context` until
  `pq_worker_scan_end()`；
- the leader `PQ_Leader_context` remains owned by the caller and must be ended
  by `pq_leader_scan_end()` after worker cleanup。

Execution boundary:

- D4a must run only under a new DBUG gate；
- D4a may call handler leader `EXECUTE` mode because worker attach requires a
  real leader context；
- D4a must not call `mark_pq_started()` or any equivalent no-fallback commit
  point；
- any failure before worker thread start remains serial-fallback eligible；
- no worker thread is started in D4a；
- no worker row is produced or made visible to the leader。

Required counters:

- `Parallel_worker_attach_smoke_attempts` increments when the isolated D4a
  helper is invoked；
- `Parallel_worker_attach_smoke_success` increments after worker TABLE open,
  `pq_worker_scan_init()`, `pq_worker_scan_end()`, worker TABLE close, and
  worker THD destroy all complete；
- `Parallel_worker_attach_smoke_cleanup_calls` increments from the D4a cleanup
  helper once per attempt that reaches cleanup；
- existing execution counters must remain unchanged:
  `Parallel_queries_executed`, `Parallel_workers_launched`,
  `Parallel_rows_scanned`。

Allowed files:

- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.cc` only for a DBUG-only hook before the
  existing handler PROBE, existing post-PROBE smoke chain, and any normal
  positive execution；
- `sql/mysqld.cc` only for SHOW STATUS exposure；
- one focused MTR test/result under `mysql-test/suite/parallel_query/`；
- `mysql-test/suite/parallel_query/r/pq_stats.result` if status variable count
  changes；
- this taskbook。

Forbidden files:

- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/pq_handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*`；
- `sql/sql_optimizer.*`；
- `sql/handler.*`；
- `storage/innobase/**`；
- any ORDER/GROUP/ref/ICP files outside the allowed list。

Forbidden behavior:

- no cloned JOIN；
- no `pq_make_join()`；
- no `ExecuteIteratorQuery()` in worker；
- no worker thread start；
- no `Query_result_mq` send/read；
- no `Exchange_nosort` row materialization；
- no `ParallelScanIterator::Read()`；
- no `PQblockScanIterator::Read()` pull-row path；
- no default user-visible `PARALLEL_SCAN` execution；
- no silent fallback after a worker has been started, though D4a must not start
  one。

Suggested implementation:

1. Add a `Gather_operator` debug helper such as
   `run_worker_attach_contract_smoke(THD *leader_thd, TABLE *leader_table,
   PQ_Leader_context *leader_ctx)`；
2. helper enforces DOP=1, initialized gather, configured worker open context,
   worker THD create, worker table open, worker scan init/end, worker table
   close, worker THD destroy；
3. helper uses a single cleanup block to keep open/close/end/destroy
   idempotent；
4. `PQTableScanIterator::Init()` DBUG hook obtains a leader `EXECUTE` context,
   runs the helper, ends the leader context, then returns existing serial
   fallback；
5. the hook must be placed after the table/blob guard but before
   `probe_attempts.fetch_add()` and before
   `pq_leader_scan_init(PROBE)`；the hook must not fall through to the existing
   PROBE path；
6. the hook is isolated from D3 lifecycle smoke and from the existing
   post-PROBE smoke chain。

Required MTR assertions:

- attempt delta >= 1；
- success delta >= 1；
- cleanup delta >= 1；
- `Parallel_queries_executed` delta = 0；
- `Parallel_workers_launched` delta = 0；
- `Parallel_rows_scanned` delta = 0；
- `Parallel_worker_result_smoke_workers` delta = 0；
- `Parallel_worker_open_smoke_runs` delta = 0 and
  `Parallel_worker_handler_smoke_runs` delta = 0 unless D4a deliberately
  reuses the old open-table smoke helper, in which case the task must document
  that coupling and assert the exact expected delta for both counters；
- SELECT result still comes from serial fallback。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query <d4a_test> pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d4a_target --tmpdir=/tmp/pqt_m11d4a_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11d4a_full --tmpdir=/tmp/pqt_m11d4a_full
```

Docs-Design Review:

- first review returned `REVISE` because the DBUG hook position needed to be
  explicit and the MTR assertions missed
  `Parallel_worker_handler_smoke_runs`；
- taskbook was revised to require the hook after the table/blob guard and
  before `probe_attempts.fetch_add()` / `pq_leader_scan_init(PROBE)`；
- taskbook now requires the hook to call `EXECUTE`, run the D4a helper, end the
  leader context, and directly return serial fallback without falling through；
- taskbook now asserts both old worker open and worker handler smoke counters；
- re-review returned `ACCEPT`。

Implementation:

- added `Gather_operator::run_worker_attach_contract_smoke()` as an isolated
  D4a helper；
- helper creates a worker THD, opens an independent worker TABLE/handler,
  calls `pq_worker_scan_init()` and `pq_worker_scan_end()`, closes the worker
  TABLE, destroys the worker THD, and destroys local gather state；
- added a DBUG-only `pq_worker_attach_contract_smoke` hook in
  `PQTableScanIterator::Init()` after the table/blob guard and before handler
  PROBE accounting；
- the hook creates a leader `EXECUTE` context, runs the helper, ends the leader
  context, and returns existing serial fallback；
- added status counters:
  `Parallel_worker_attach_smoke_attempts`,
  `Parallel_worker_attach_smoke_success`,
  `Parallel_worker_attach_smoke_cleanup_calls`；
- added focused MTR `pq_worker_attach_contract_smoke` and updated `pq_stats`。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- `--record pq_worker_attach_contract_smoke` executed successfully but MTR
  failed to copy the new result file with errno 1, so the result file was
  synchronized from the generated test log and then verified by normal MTR；
- targeted MTR passed:
  `pq_worker_attach_contract_smoke` and `pq_stats`, 3/3 including
  `shutdown_report`；
- full `parallel_query` suite passed, 82/82。

Code-Docs-Test Review:

- Review Agent returned `ACCEPT`；
- confirmed the D4a hook is after the table/blob guard and before handler
  PROBE accounting；
- confirmed the hook performs its own `EXECUTE` leader context setup, helper
  call, leader end, and serial fallback return without falling through；
- confirmed the helper only covers worker THD, worker TABLE open,
  `pq_worker_scan_init/end`, close/destroy cleanup；
- confirmed no worker thread, MQ/Exchange row path, clone/JOIN execution, or
  forbidden file was touched；
- confirmed cleanup uses a single guarded path and MTR covers attach counters,
  old open/handler smoke counters, PROBE delta, and real execution counters。

### M11-D4b: Leader Row Stream Adapter Smoke Taskbook

Status: taskbook completed / Docs-Design Review accepted。

Goal:

- add a debug-only leader row stream smoke that lets the leader consume a
  bounded worker-produced row stream through the existing
  `PQTableScanIterator::Read()` path；
- prove the leader-side runtime state, `Exchange_nosort` materialization, EOF
  cleanup, and execution counters work together under a guarded path；
- keep default execution unchanged；
- do not introduce cloned JOIN, commercial worker plan, or user-visible default
  `PARALLEL_SCAN`。

Relationship to earlier work:

- D4a proves worker TABLE/handler attach and `pq_worker_scan_init/end` cleanup；
- D4b may reuse existing callback row production and `Exchange_nosort`
  record-image materialization；
- D4b must not reuse old local smokes that drain rows entirely inside
  `Gather_operator` if the goal is to exercise `PQTableScanIterator::Read()`；
- D4b must not use `Query_result_mq` as the worker execution result path yet；
  `Query_result_mq` / `PQWR` remains covered by M11-B smokes until cloned JOIN
  worker execution exists。

Allowed path:

1. DBUG-only hook in `PQTableScanIterator::Init()` after table/blob guard and
   before existing handler PROBE accounting；
2. hook obtains handler leader `EXECUTE` context with DOP=1；
3. hook creates and owns `m_gather` for the iterator；
4. hook configures worker open context from leader table/context；
5. hook starts a bounded controlled producer that enqueues record-image ROW
   frames and FINISH into `m_gather->get_exchange()`；
6. hook calls `mark_pq_started()` and returns `false` so the normal executor
   calls `PQTableScanIterator::Read()`；
7. `Read()` materializes rows through
   `Exchange_nosort::materialize_next_record_image_status()`；
8. EOF calls existing `cleanup_pq_resources(false)` and returns `-1`。

Required limits:

- DOP fixed at 1；
- row production bounded to a small fixed limit, suggested 2 rows；
- only non-BLOB single-table InnoDB full scan shapes；
- no ORDER BY / GROUP BY / ref / range / ICP / partition / reverse；
- no worker thread start in D4b unless a later reviewed revision explicitly
  chooses threaded shadow as a separate subtask；
- no cloned JOIN or `ExecuteIteratorQuery()`；
- no `Query_result_mq` send/read in D4b。

Allowed files:

- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.cc` only for a DBUG-only hook before handler
  PROBE and for using existing runtime state helpers；
- `sql/mysqld.cc` only for SHOW STATUS exposure；
- one focused MTR test/result under `mysql-test/suite/parallel_query/`；
- `mysql-test/suite/parallel_query/r/pq_stats.result` if status variable count
  changes；
- this taskbook。

Forbidden files:

- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/pq_handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*`；
- `sql/sql_optimizer.*`；
- `sql/handler.*`；
- `storage/innobase/**`。

Required counters:

- `Parallel_leader_row_stream_smoke_attempts` increments when D4b hook is
  invoked；
- `Parallel_leader_row_stream_smoke_selected` increments after the hook has
  prepared `m_gather`, produced bounded rows, marked PQ started, and returned
  to executor；
- `Parallel_leader_row_stream_smoke_rows` increments by the number of rows
  returned through `PQTableScanIterator::Read()` under this DBUG path；
- existing real execution counters may increment under this debug path only
  according to current `Read()` semantics:
  `Parallel_queries_executed` delta must be 1 and `Parallel_rows_scanned`
  delta must equal the returned row count；
- `Parallel_workers_launched` delta must remain 0。

Required MTR assertions:

- debug SELECT returns through `PQTableScanIterator::Read()` with the bounded
  row count；D4b does not prove final row-value correctness because the existing
  record-image shadow path still has separate materialization limitations；
- attempt delta >= 1；
- selected delta >= 1；
- leader row stream rows delta = returned row count；
- `Parallel_queries_executed` delta = 1；
- `Parallel_rows_scanned` delta = returned row count；
- `Parallel_workers_launched` delta = 0；
- `Parallel_probe_attempts` delta = 0；
- old worker-result smoke worker counter delta = 0；
- D4a attach counters do not grow unless the implementation intentionally
  reuses D4a helper and documents the coupling。

Suggested implementation:

1. Add a `Gather_operator` helper such as
   `prepare_leader_row_stream_smoke(THD *leader_thd, TABLE *leader_table,
   PQ_Leader_context *leader_ctx, uint32 row_limit, uint32 *rows_enqueued)`；
2. helper initializes gather if needed, configures worker open context, opens
   worker TABLE/handler, calls `pq_worker_scan_init()`, produces up to
   `row_limit` record images through existing callback row sink into
   `Exchange_nosort`, enqueues FINISH, then ends/closes worker resources；
3. helper must leave `Exchange_nosort` populated for `PQTableScanIterator::Read()`
   and must not drain rows itself；
4. `PQTableScanIterator::Init()` owns `m_gather` after helper succeeds；
5. on any failure before `mark_pq_started()`, cleanup and return an error or
   serial fallback according to existing pre-commit behavior；the first coding
   step should prefer fail-closed error for unexpected DBUG smoke failure and
   serial fallback only for unsupported handler/shape；
6. after `mark_pq_started()`, use existing `Read()` cleanup/error behavior。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query <d4b_test> pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d4b_target --tmpdir=/tmp/pqt_m11d4b_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11d4b_full --tmpdir=/tmp/pqt_m11d4b_full
```

Docs-Design Review:

- Review Agent returned `ACCEPT`；
- confirmed D4b is a reasonable step after D4a because it exercises
  `PQTableScanIterator::Read()` with a bounded row stream while staying
  DBUG-only；
- confirmed the scope excludes worker thread start, cloned JOIN,
  `Query_result_mq` formal worker plan, ORDER/GROUP/ref/ICP, and default
  `PARALLEL_SCAN`；
- confirmed `Parallel_queries_executed=1` and
  `Parallel_rows_scanned=returned rows` match current `Read()` semantics for
  this debug path；
- non-blocking suggestions were applied: duplicate allowed-file entry removed
  and `Parallel_probe_attempts` delta assertion added。

Implementation:

- added `Gather_operator::prepare_leader_row_stream_smoke()`；
- added DBUG-only `pq_leader_row_stream_smoke` hook in
  `PQTableScanIterator::Init()` after the table/blob guard and before handler
  PROBE accounting；
- the hook creates a leader `EXECUTE` context, prepares a DOP=1 bounded
  record-image row stream, marks PQ started, and returns to the executor so
  `PQTableScanIterator::Read()` consumes the stream；
- added status counters:
  `Parallel_leader_row_stream_smoke_attempts`,
  `Parallel_leader_row_stream_smoke_selected`,
  `Parallel_leader_row_stream_smoke_rows`；
- added focused MTR `pq_leader_row_stream_smoke` and updated `pq_stats`；
- D4b validates row count and counter semantics, not final row-value
  correctness. The existing record-image shadow path can return placeholder
  field values, so row-value correctness remains deferred.

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- `--record pq_leader_row_stream_smoke` executed successfully but MTR failed to
  copy the new result file with errno 1, so the result file was synchronized
  from the generated test log and then revised to hide placeholder row values；
- targeted MTR passed:
  `pq_leader_row_stream_smoke` and `pq_stats`, 3/3 including
  `shutdown_report`；
- full `parallel_query` suite passed, 83/83。

Code-Docs-Test Review:

- Review Agent returned `ACCEPT`；
- confirmed `pq_leader_row_stream_smoke` is after the table/blob guard and
  before handler PROBE accounting；
- confirmed the hook owns the debug-only `EXECUTE` setup, bounded stream
  preparation, `mark_pq_started()`, and then returns to executor `Read()`；
- confirmed `prepare_leader_row_stream_smoke()` leaves rows in
  `Exchange_nosort` for `Read()` and does not drain rows, launch worker
  threads, use `Query_result_mq`, or run cloned JOIN execution；
- confirmed `Read()` increments the D4b row counter only for materialized ROW
  frames under the DBUG path, matching `Parallel_rows_scanned` semantics；
- confirmed docs intentionally defer row-value correctness because current
  record-image shadow materialization can still return placeholder values；
- confirmed SHOW STATUS/reset/`pq_stats` coverage and focused MTR coverage；
- non-blocking notes: new MTR files must be explicitly staged, and D4b helper
  may grow existing callback/exchange smoke counters because it intentionally
  reuses the bounded callback producer。

### M11-D4c: Commit-point Error Cleanup Smoke Taskbook

Status: design-only taskbook created；waiting for Docs-Design Review。

Goal:

- add a debug-only smoke that enters the same leader row-stream commit point as
  D4b and then forces a `Read()`-time ERROR token；
- prove that after `mark_pq_started()` the iterator does not serial fallback；
- prove `PQTableScanIterator::Read()` aborts/cleans owned PQ resources and
  returns an error；
- keep default execution unchanged；
- do not introduce cloned JOIN, commercial worker plan, real worker thread, or
  user-visible default `PARALLEL_SCAN`。

Relationship to earlier work:

- D4b proved a bounded ROW + FINISH stream can be consumed by
  `PQTableScanIterator::Read()`；
- D4c should reuse the D4b setup shape but replace the producer payload with a
  deterministic ERROR after commit point；
- D4c is not a row-value correctness task and must not expand record
  materialization；
- D4c is not a worker thread/KILL task；threaded external KILL remains covered
  by existing V2-8K/V2-8N and M5 tests。

Allowed path:

1. DBUG-only hook in `PQTableScanIterator::Init()` after the table/blob guard
   and before handler PROBE accounting；
2. hook obtains handler leader `EXECUTE` context with DOP=1；
3. hook creates and owns `m_gather` for the iterator；
4. hook initializes/configures the exchange enough for `Read()` to consume；
5. hook enqueues exactly one deterministic ERROR marker, with no ROW payload；
6. hook calls `mark_pq_started()` and returns `false`；
7. executor calls `PQTableScanIterator::Read()`；
8. `Read()` observes ERROR, calls existing `cleanup_pq_resources(true)`,
   reports an error, and returns `1`；
9. the SQL statement fails, and MTR verifies status deltas。

Required limits:

- DOP fixed at 1；
- only non-BLOB single-table InnoDB full scan shapes；
- no successful ROW frame and no FINISH-only EOF in the D4c positive error
  smoke；
- no worker thread start；
- no cloned JOIN or `ExecuteIteratorQuery()`；
- no `Query_result_mq` send/read；
- no ORDER BY / GROUP BY / ref / range / ICP / partition / reverse；
- no new serial fallback after `mark_pq_started()`。

Allowed files:

- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.cc` only for a DBUG-only hook before handler
  PROBE and for DBUG-only row-stream error accounting；
- `sql/mysqld.cc` only for SHOW STATUS exposure；
- one focused MTR test/result under `mysql-test/suite/parallel_query/`；
- `mysql-test/suite/parallel_query/r/pq_stats.result` if status variable count
  changes；
- this taskbook；
- `Docs/pq_tasks/README.md` only to refresh progress status。

Forbidden files:

- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/pq_handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange.*` unless a tiny typed ERROR enqueue helper is
  unavoidable and reviewed before coding；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*`；
- `sql/sql_optimizer.*`；
- `sql/handler.*`；
- `storage/innobase/**`。

Preferred implementation boundary:

- first try to add a small `Gather_operator` helper such as
  `prepare_leader_row_stream_error_smoke(THD *leader_thd, TABLE *leader_table,
  PQ_Leader_context *leader_ctx)`；
- helper may initialize gather/configure worker open contexts only if required
  by existing Exchange/Gather invariants；
- helper must enqueue an existing Exchange ERROR representation and must not
  drain it；
- if no public helper exists for ERROR enqueue, stop and split out a reviewed
  Exchange-only design before touching `exchange.*`；
- `PQTableScanIterator::Init()` must own `m_gather` only after allocation and
  must rely on `cleanup_pq_resources(true)` for failures after leader context
  creation；
- unsupported handler/shape before commit point may still fallback serial；
- any unexpected failure after `mark_pq_started()` must fail closed, not
  fallback。

Required counters:

- `Parallel_leader_row_stream_error_smoke_attempts` increments when the D4c
  hook is invoked；
- `Parallel_leader_row_stream_error_smoke_selected` increments after the hook
  has prepared the ERROR stream, marked PQ started, and returned to executor；
- `Parallel_leader_row_stream_error_smoke_errors` increments when `Read()`
  observes the D4c ERROR path；
- `Parallel_leader_row_stream_error_smoke_cleanup` increments when the D4c
  ERROR path reaches PQ resource cleanup；
- D4c-specific `errors` and `cleanup` accounting must live in the
  `PQTableScanIterator::Read()` ERROR branch under the
  `pq_leader_row_stream_error_smoke` DBUG gate, immediately around
  `cleanup_pq_resources(true)`；do not increment these counters inside generic
  `cleanup_pq_resources()` or destructor cleanup；
- `Parallel_queries_executed` delta must remain 0 because no ROW/EOF success
  is reached；
- `Parallel_rows_scanned` delta must remain 0；
- `Parallel_workers_launched` delta must remain 0；
- `Parallel_queries_fallback` delta must remain 0 after the selected counter
  increments。

Required MTR assertions:

- statement uses `--error ER_GET_ERRNO` for the failing SELECT；
- attempts delta >= 1；
- selected delta >= 1；
- errors delta = 1；
- cleanup delta >= 1；
- `Parallel_queries_executed` delta = 0；
- `Parallel_rows_scanned` delta = 0；
- `Parallel_workers_launched` delta = 0；
- `Parallel_queries_fallback` delta = 0 for the D4c statement window；
- `Parallel_probe_attempts` delta = 0；
- `Parallel_worker_attach_smoke_attempts` delta = 0；
- `Parallel_worker_attach_smoke_success` delta = 0；
- `Parallel_worker_attach_smoke_cleanup_calls` delta = 0；
- `Parallel_leader_row_stream_smoke_attempts` delta = 0；
- `Parallel_leader_row_stream_smoke_selected` delta = 0；
- `Parallel_leader_row_stream_smoke_rows` delta = 0。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query <d4c_test> pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d4c_target --tmpdir=/tmp/pqt_m11d4c_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11d4c_full --tmpdir=/tmp/pqt_m11d4c_full
```

Agent Task Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M11-D4c Commit-point Error Cleanup Smoke。

请阅读：
- Docs/pq_tasks/README.md；
- Docs/pq_tasks/commercial-port-m11-parallel-scan-lifecycle.md；
- sql/parallel_query/pq_iterator.cc；
- sql/parallel_query/sql_parallel.h；
- sql/parallel_query/sql_parallel.cc。

任务目标：
1. 实现 DBUG-only `pq_leader_row_stream_error_smoke`；
2. 在 `PQTableScanIterator::Init()` 中进入 DOP=1 EXECUTE、准备 ERROR stream、
   `mark_pq_started()` 后返回 executor；
3. 让 `PQTableScanIterator::Read()` 观察 ERROR 并走 fail-closed cleanup，不得
   serial fallback；
4. 增加必要 SHOW STATUS counter 和一个 focused MTR；
5. 更新本任务书 Completion Report。

允许修改：
- 见 M11-D4c Allowed files。

禁止修改：
- 见 M11-D4c Forbidden files。

硬停止条件：
- 若必须修改 `exchange.*` 才能表达 ERROR marker，先停止并报告，不要编码；
- 若实现需要 worker thread、`Query_result_mq`、cloned JOIN、handler/InnoDB
  接口变更，停止并报告；
- 若 MTR 无法稳定断言 fallback delta=0，停止并报告。

验证：
- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR: `<d4c_test> pq_stats`；
- full `parallel_query` suite。

完成后不要自行 commit。
```

Docs-Design Review:

- first review returned `REVISE` with no critical findings；
- review requested exact D4a/D4b counter delta assertions, D4c-specific
  `errors/cleanup` accounting placement in `Read()` rather than generic
  cleanup, and stable `--error ER_GET_ERRNO` expectation；
- requested revisions were applied；
- re-review returned `ACCEPT`；
- confirmed D4a `attempts/success/cleanup_calls` deltas and D4b
  `attempts/selected/rows` deltas are all explicitly required to remain 0；
- confirmed D4c `errors/cleanup` counters are scoped to
  `PQTableScanIterator::Read()` ERROR branch under
  `pq_leader_row_stream_error_smoke`；
- confirmed the failing SELECT uses `--error ER_GET_ERRNO`。

Implementation:

- added `Gather_operator::prepare_leader_row_stream_error_smoke()`；
- helper initializes/configures the existing DOP=1 gather as needed, enqueues
  one `Exchange_nosort::enqueue_error_smoke(0)` token, and leaves it for
  `PQTableScanIterator::Read()`；
- added DBUG-only `pq_leader_row_stream_error_smoke` hook in
  `PQTableScanIterator::Init()` after the table/blob guard and before handler
  PROBE accounting；
- the hook creates a leader `EXECUTE` context, prepares the ERROR stream,
  increments selected, calls `mark_pq_started()`, and returns `false`；
- added DBUG-only ERROR and cleanup accounting around the existing
  `Read()` ERROR cleanup path；
- added status counters:
  `Parallel_leader_row_stream_error_smoke_attempts`,
  `Parallel_leader_row_stream_error_smoke_selected`,
  `Parallel_leader_row_stream_error_smoke_errors`,
  `Parallel_leader_row_stream_error_smoke_cleanup`；
- added focused MTR `pq_leader_row_stream_error_smoke` and updated `pq_stats`。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- `--record pq_leader_row_stream_error_smoke` executed successfully but MTR
  failed to copy the new result file with errno 1, so the result file was
  synchronized from the generated test log；
- targeted MTR passed:
  `pq_leader_row_stream_error_smoke` and `pq_stats`, 3/3 including
  `shutdown_report`；
- full `parallel_query` suite passed, 84/84。

Code-Docs-Test Review:

- Review Agent returned `ACCEPT`；
- confirmed the D4c hook is DBUG-only, after the table/blob guard and before
  handler PROBE accounting；
- confirmed the selected path enters DOP=1 `EXECUTE`, prepares one ERROR token,
  increments selected, calls `mark_pq_started()`, and returns to executor
  `Read()` with no post-commit serial fallback；
- confirmed `prepare_leader_row_stream_error_smoke()` uses existing
  `Exchange_nosort::enqueue_error_smoke(0)` and does not modify `exchange.*`,
  drain rows, start worker threads, use `Query_result_mq`, or run cloned JOIN；
- confirmed `Read()` ERROR accounting is scoped to the
  `pq_leader_row_stream_error_smoke` DBUG gate around the existing ERROR
  cleanup path, not generic cleanup/destructor cleanup；
- confirmed status variables, reset, `pq_stats`, and focused MTR coverage；
- non-blocking note: new MTR test/result files must be explicitly staged。

### M11-D5: Row-value Correctness Before Positive Path Taskbook

Status: design-only taskbook created；waiting for Docs-Design Review。

Decision:

- D4a-D4c are sufficient lifecycle/commit-point guards, but they are not enough
  to move to M11-E/F or default `ParallelScanIterator` positive execution；
- D5 must first close the D4b row-value correctness gap；
- D5 must stay debug-only until a visible SELECT can assert actual returned
  values, not just row count and counters。

Explorer findings:

- Next-step Explorer recommended continuing M11-D5 instead of M11-E/F because
  commercial `ParallelScanIterator` is still a fail-closed skeleton in this
  branch, while D4b/D4c live in `PQTableScanIterator` debug hooks；
- Row-Value Correctness Explorer confirmed D4b currently copies raw
  `TABLE::record[0]` images through Exchange, while the commercial reference
  uses field-level `Field_raw_data` / null bitmap / varlen encoding and leader
  conversion；
- the most likely reason for placeholder values in D4b is incomplete worker
  handler scan/template initialization before `pq_worker_scan_callback_produce()`
  fills the worker record buffer；
- the first low-risk suspect is that `run_worker_callback_limited_producer()`
  does not wrap callback production with the same handler scan init/end pattern
  used by threaded producer tasks, especially `ha_rnd_init(true)` /
  `ha_rnd_end()`；
- final commercial-equivalent row serialization still needs later design；
  D5 only proves a narrow fixed-row debug path before any positive path
  migration。

Goal:

- add a debug-only row-value correctness smoke for the D4b-style leader row
  stream path；
- make a visible SELECT return deterministic integer values through
  `PQTableScanIterator::Read()`；
- keep default execution unchanged；
- do not enable user-visible/default `PARALLEL_SCAN`；
- do not introduce worker thread, cloned JOIN, `Query_result_mq`, ORDER/GROUP,
  ref/range/ICP, or handler/InnoDB interface changes。

Scope:

- DOP fixed at 1；
- single InnoDB table；
- fixed-width integer columns only；
- no BLOB, varlen, generated, hidden, virtual, nullable, partitioned, reverse,
  ORDER BY real merge, GROUP BY, secondary/ref/range/ICP；
- row limit fixed and small, suggested first two clustered rows；
- MTR must not hide SELECT output；
- D5 may adjust the D4b bounded producer setup only enough to make worker
  record images valid for this narrow shape。

Allowed files:

- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.cc` only for DBUG-only hook/counter
  accounting if needed；
- `sql/mysqld.cc` only if new SHOW STATUS counters are required；
- one focused MTR test/result under `mysql-test/suite/parallel_query/`；
- `mysql-test/suite/parallel_query/r/pq_stats.result` if status variable count
  changes；
- this taskbook；
- `Docs/pq_tasks/README.md` / M11 main taskbook only for progress status。

Forbidden files:

- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/sql_executor.*`；
- `sql/sql_optimizer.*`；
- `sql/join_optimizer/access_path.*`；
- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange.*`；
- ORDER/GROUP/ref/ICP related expansion；
- default AccessPath/factory hook behavior。

Required implementation direction:

1. Prefer fixing/verifying D4b's existing bounded callback producer setup,
   especially worker handler scan init/end ordering；
2. if adding `ha_rnd_init(true)` / `ha_rnd_end()` around
   `pq_worker_scan_callback_produce()` is sufficient, keep the change inside
   `sql_parallel.cc` and document why it matches existing threaded producer
   practice；
3. do not change raw Exchange record-image protocol in D5；
4. do not add field-level `Field_raw_data` / commercial serialization in D5；
5. if row values cannot be made correct without touching handler/InnoDB,
   `exchange.*`, `Query_result_mq`, or clone/JOIN, stop and write a blocked
   report instead of widening scope；
6. blocked report must include the visible SELECT output observed, the exact
   counter deltas for the statement window, and the suspected missing contract。

Required counters:

- D5 can reuse D4b counters if no new status variable is needed:
  `Parallel_leader_row_stream_smoke_attempts`,
  `Parallel_leader_row_stream_smoke_selected`,
  `Parallel_leader_row_stream_smoke_rows`；
- if a separate D5 hook is introduced, add distinct counters and update
  `pq_stats`；
- `Parallel_queries_executed` delta must be 1；
- `Parallel_rows_scanned` delta must equal visible returned row count；
- `Parallel_workers_launched` delta must remain 0；
- `Parallel_probe_attempts` delta must remain 0；
- `Parallel_queries_fallback` delta must remain 0 after selected/commit point。

Required MTR assertions:

- use focused MTR name `pq_leader_row_stream_row_values`；
- create a two-row InnoDB table with fixed integer columns and no PRIMARY KEY
  or secondary index, suggested:
  `CREATE TABLE pq_row_value_t (id INT NOT NULL, v INT NOT NULL) ENGINE=InnoDB`
  and rows `(1,10)`, `(2,20)`；
- visible SELECT output must contain actual expected values:
  `SELECT id, v FROM pq_row_value_t` returns `(1,10)` and `(2,20)`；
- result log must stay enabled for the SELECT；
- row count/counter assertions:
  - leader row stream attempts delta = 1；
  - leader row stream selected delta = 1；
  - leader row stream rows delta = 2；
  - `Parallel_queries_executed` delta = 1；
  - `Parallel_rows_scanned` delta = 2；
  - `Parallel_workers_launched` delta = 0；
  - `Parallel_probe_attempts` delta = 0；
  - `Parallel_queries_fallback` delta = 0；
- D4a attach counters and D4c error counters must not grow；
- include a guarded negative shape if practical, such as BLOB table fallback or
  skipped hook, without widening scope。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_leader_row_stream_row_values pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11d5_target --tmpdir=/tmp/pqt_m11d5_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11d5_full --tmpdir=/tmp/pqt_m11d5_full
```

Agent Task Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M11-D5 Row-value Correctness Before Positive Path。

请阅读：
- Docs/pq_tasks/README.md；
- Docs/pq_tasks/commercial-port-m11-main-architecture-restart.md；
- Docs/pq_tasks/commercial-port-m11-parallel-scan-lifecycle.md；
- sql/parallel_query/pq_iterator.cc；
- sql/parallel_query/sql_parallel.h；
- sql/parallel_query/sql_parallel.cc；
- sql/parallel_query/exchange.cc/.h（只读）。

任务目标：
1. 让 D4b-style debug leader row stream 能返回真实可断言的固定整数行值；
2. result log 不得隐藏 SELECT 输出；
3. 优先检查/修复 bounded callback producer 的 worker handler scan
   init/end 顺序；
4. 不改变默认执行行为，不打开 `PARALLEL_SCAN` 默认正路径。

允许修改：
- 见 M11-D5 Allowed files。

禁止修改：
- 见 M11-D5 Forbidden files。

硬停止条件：
- 需要修改 handler/InnoDB、`exchange.*`、`Query_result_mq`、clone/JOIN、
  AccessPath/factory、worker thread 才能让值正确；
- SELECT 输出只能隐藏或只能断言行数；
- post-commit fallback counter 增长；
- 需要支持 varlen/BLOB/null/generated/secondary/ref/range/ICP 才能通过；
- 若阻塞，报告必须包含实际 visible SELECT 输出和本语句 counter delta。

验证：
- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR: `pq_leader_row_stream_row_values pq_stats`；
- full `parallel_query` suite。

完成后不要自行 commit。
```

Docs-Design Review:

- first review returned `REVISE` with no critical findings；
- review required `exchange.*` to be unconditionally forbidden, because D5 must
  not change the raw Exchange record-image protocol；
- review required the MTR shape to avoid range/ref access by using a two-row
  fixed integer InnoDB table with no primary or secondary index；
- review required explicit leader row stream attempts/selected/rows deltas；
- requested revisions were applied；
- re-review returned `ACCEPT`；
- confirmed the scope remains debug-only, with no default `PARALLEL_SCAN`, no
  handler/InnoDB, no `Query_result_mq`, no clone/JOIN, no AccessPath, and no
  Exchange protocol changes。

## Review 要求

- D0 requires Docs-Design Review；
- each coding subtask requires Code-Docs-Test Review；
- review must confirm fail-closed behavior remains unchanged before commit；
- D1 coding review must verify cleanup idempotency is mechanically present, not
  only documented。

## Review

Docs-Design Review returned `ACCEPT`：

- D0 is clearly design-only and does not enable `PARALLEL_SCAN`；
- lifecycle contract covers ownership, partial `Init()` failure cleanup,
  `Read()` EOF/error cleanup, idempotency, and no-fallback commit point；
- D1 allowed/forbidden file scope is narrow enough to avoid current
  `PQTableScanIterator`, AccessPath, handler/InnoDB, worker result, clone,
  eligibility, and MTR behavior changes；
- D1 validation is sufficient for compile-only lifecycle skeleton work；
- D2/D3 handoff is natural and defers real worker plan/handler/ORDER/ref work。
