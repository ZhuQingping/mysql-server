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

### M11-D2: Lifecycle Status Smoke

Only after D1 review accepted. Add observability for fail-closed lifecycle
cleanup without enabling `PARALLEL_SCAN`. Exact counters/MTR must be designed
separately.

### M11-D3: Guarded PARALLEL_SCAN Factory Probe

Only after D1/D2 accepted. Evaluate a debug-only `PARALLEL_SCAN` construction
probe. It must not start workers or call handler/InnoDB.

## Deferred

- real cloned JOIN execution；
- `make_pq_worker_plan()` positive path；
- worker-side `PQblockScanIterator` handler scan；
- worker-side `PQRefIterator` / ICP / dependent ref；
- `Exchange_sort` real ORDER BY；
- stable output rowid；
- full resource control / `parallel_max_threads` budget；
- `Field_raw_data` / `Batch_buffer` commercial row serialization。

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
