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
