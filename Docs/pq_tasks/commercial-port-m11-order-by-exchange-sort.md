# M11-E ORDER BY / Exchange_sort Real Path Gate Taskbook

## 状态

Status: M11-E0/E1/E2/E3/E4/E5a/E5b-0/E5b-1/E5b-2/E5b-3/E5c/E5d/E5d-0
completed and committed；M11-E5d-S0/S1 completed and committed；
M11-E5d-S2 leader save/restore smoke completed，waiting for commit。

## 背景

当前分支的 `Exchange_sort` 只有 synthetic merge smoke：

- `Exchange_sort::run_synthetic_order_merge_smoke()` 使用固定 worker-local
  sorted streams 验证 ASC、DESC 和 rowid tie-break；
- `Gather_operator::run_exchange_sort_smoke()` 只在现有 smoke 链中调用；
- optimizer eligibility 仍以 `HAS_ORDER_BY` 拒绝真实 ORDER BY PQ path；
- M11-D6 已证明 debug-only `ParallelScanIterator::Read()` 可返回固定整数
  row values，但不启动 worker thread、不接 clone/JOIN、不接真实
  `Query_result_mq`。

商用实现的 ORDER BY 主链路更重：

- `ParallelScanIterator::pq_make_filesort()` 在 leader 构造 Filesort /
  order list；
- `ParallelScanIterator::pq_init_record_gather()` 创建 `MQ_record_gather`；
- `MQ_record_gather::mq_scan_init()` 在有 sort 时创建 `Exchange_sort`；
- `Exchange_sort` 依赖 `Filesort`、`Sort_param`、`binary_heap`、worker MQ
  record frame、rowid/ref_length、desc index metadata；
- `ParallelScanIterator::Read()` 通过 `m_record_gather->mq_scan_next()`
  消费 worker output。

## 目标

M11-E 不直接打开真实 ORDER BY user-visible PQ。目标是把商用 ORDER BY
path 拆成可验证的小步，先建立接口/状态/测试护栏，再决定是否进入编码：

1. 明确当前 `Exchange_sort` synthetic smoke 与商用 `Exchange_sort` 的差异；
2. 明确 ORDER BY real path 对 M11-A/B/D 的依赖；
3. 明确最小安全迁移顺序；
4. 防止过早修改 optimizer eligibility 或默认 AccessPath 行为；
5. 保留现有 ORDER BY serial boundary，直到设计和 review 明确允许。

## Scope

M11-E0 仅设计，不改源码。

Explorer conclusion:

- do not code immediately；
- commercial ORDER BY depends on Filesort/JOIN saved order state,
  `MQ_record_gather`, worker-result MQ frames, `Exchange_sort`, and optimizer
  eligibility；
- current repository still treats ORDER BY as a serial boundary through
  `HAS_ORDER_BY`；
- E1 may start only as compile-only shape after docs review；
- M11-F ref/ICP worker path must remain separate because it depends on worker
  pull/callback row production decisions and must not be mixed with ORDER BY。

后续编码子任务必须单独拆分：

- M11-E1: `Exchange_sort` commercial shape compile-only contract；
- M11-E2: debug-only sorted row-frame adapter smoke；
- M11-E3: debug-only `ParallelScanIterator` order gather smoke；
- M11-E4: user-visible ORDER BY gate evaluation，默认仍可拒绝。

## Allowed Files

M11-E0:

- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-main-architecture-restart.md`。

后续编码阶段可按 review 结果逐步开放：

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterators.h`；
- `sql/parallel_query/pq_iterators.cc`；
- focused MTR under `mysql-test/suite/parallel_query/`。

## Forbidden Files

M11-E0 以及 E1/E2 默认禁止：

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_executor.*`；
- `sql/join_optimizer/access_path.*`；
- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/parallel_query/pq_clone*`；
- real worker launch / worker JOIN clone wiring；
- default user-visible ORDER BY PQ eligibility changes。

## Design Requirements

1. ORDER BY real path 必须依赖 M11-B worker-result adapter 和 M11-D
   `ParallelScanIterator` lifecycle 稳定后再打开；
2. compile-only shape 可以先引入商用字段/方法名，但默认必须 fail-closed；
3. debug-only smoke 必须使用专属 DBUG flag 和专属 counters，不能复用普通
   ORDER BY user path；
4. 若需要 `Filesort` / `Sort_param`，必须先证明 leader-only 构造不会改写
   normal executor state；
5. 若需要 worker MQ row frame，必须明确使用 `Query_result_mq` adapter 还是
   current typed row-image frame；
6. `HAS_ORDER_BY` serial boundary 在 E0/E1/E2 保持不变；
7. 任何需要修改 optimizer eligibility、AccessPath factory、InnoDB handler
   或 real worker launch 的步骤必须拆到 E3/E4 后单独 review。

## Required Tests

M11-E0:

- no build/test required beyond docs review。

后续编码阶段至少要求：

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e_target --tmpdir=/tmp/pqt_m11e_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e_full --tmpdir=/tmp/pqt_m11e_full
```

## Proposed Task Split

### M11-E0: ORDER BY Path Contract

Design-only。输出商用调用链、当前缺口、编码拆分、硬停止条件和 review
结论。

### M11-E1: Exchange_sort Commercial Shape

Compile-only。引入最小字段/方法边界或 adapter wrapper，但不消费真实 SQL row、
不连接真实 MQ、不改 optimizer、不改变 `pq_commercial_order_by` 的 serial
boundary。E1 taskbook 必须显式断言真实 ORDER BY SQL 仍保留
`HAS_ORDER_BY` EXPLAIN/fallback 诊断，且 `Parallel_queries_executed` /
`Parallel_workers_launched` / `Parallel_ranges_dispatched` 不增长。

### M11-E2: Sorted Row-frame Adapter Smoke

DBUG-only。使用 synthetic 或 controlled row frames 验证 `Exchange_sort`
能够按商用比较语义消费 row frame；不启动 worker thread，不打开默认 SQL
ORDER BY path。E2 taskbook 必须继续保留真实 ORDER BY SQL 的负向断言，
除非后续 review 明确批准 user-visible gate。

### M11-E3: ParallelScanIterator Order Gather Debug Path

DBUG-only。只在 D6 debug bridge 基础上验证 `ParallelScanIterator` 能选择
order gather path；仍不改变默认 ORDER BY PQ eligibility。

### M11-E4: User-visible ORDER BY Gate Design

设计优先。先设计 `HAS_ORDER_BY` 解除条件和 diagnostics，不立即开放。

### M11-E5: Guarded SQL ORDER BY Path

只有在 E1-E4 review accepted 后才评估是否允许 explicit gate + exact shape
下的简单 `ORDER BY field [ASC|DESC] LIMIT` 进入 PQ，否则继续保持 serial
boundary。

### M11-E5b-1: Controlled Frame K-way Merge Smoke

Coding scope:

- `Exchange_sort::run_orderby_frame_merge_smoke()` 使用 3 个本地 MQ queue
  发送受控 ORDER BY frame stream；
- 每个 worker queue 使用 ROW...FINISH 协议，leader 侧 decode frame 并填充
  `PQ_orderby_record_batch`；
- 复用当前 `pq_orderby_cached_merge()` 验证跨 worker K-way merge 顺序和
  rowid tie-break：期望 rowid 为 `10,11,20,21,30,40`；
- 新增 `Parallel_exchange_sort_frame_merge_smoke_rows` 和
  `Parallel_exchange_sort_frame_merge_smoke_finishes` 状态变量；
- 扩展 `pq_commercial_order_by_frames` 和 `pq_stats`，验证 merge rows >= 6、
  finishes >= 3，且原有 ORDER BY SQL 仍通过 serial boundary 测试。

Hard boundaries:

- 不修改 optimizer eligibility；
- 不修改 `ParallelScanIterator::Read()` 默认用户可见路径；
- 不启动 worker thread；
- 不接 InnoDB handler 或真实 `table->record[0]` materialization；
- `HAS_ORDER_BY` serial boundary 保持不变。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5b1_target --tmpdir=/tmp/pqt_m11e5b1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5b1_full --tmpdir=/tmp/pqt_m11e5b1_full
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，4/4；
- full `parallel_query` suite passed，88/88。

Review:

- Code/Doc/Test Review Agent verdict: `ACCEPT`；
- no blocking findings；
- residual risks moved to M11-E5b-2 follow-up:
  - FINISH-only empty worker queue must be accepted before real merge path；
  - raw sort-key/rowid byte-vector compare remains controlled-smoke only；
  - ERROR frame path is fail-closed and still needs explicit observable smoke。

### M11-E5b-3: Debug-only ORDER BY Frame Row Materialization Smoke

Status: coding completed，waiting for review/commit。

Goal:

- verify that a decoded ORDER BY frame record image can be copied into the
  leader `TABLE::record[0]` through an `Exchange_sort`-owned helper；
- keep this as a debug-only smoke invoked from existing PQ smoke setup；
- do not enable user-visible ORDER BY PQ；
- do not import commercial `Filesort` / `Sort_param` comparison logic yet。

Commercial reference:

- commercial `Exchange_sort::store_mq_record()` deep-copies worker MQ data into
  per-worker cached records；
- commercial `Exchange_sort::read_mq_record()` copies selected cached record
  into `table->record[0]` when `m_sort_param` is present, or decodes MQ data
  into `record[0]` otherwise；
- current branch already has independent ORDER BY frame decode and controlled
  K-way merge, but no `table->record[0]` materialization smoke for
  `Exchange_sort`。

Proposed coding boundary:

- change `Gather_operator::run_exchange_sort_smoke()` to accept an optional
  `TABLE *leader_table` only for smoke validation；
- update the only caller in `ParallelIterator::Init()` to pass `table()`；
- add an `Exchange_sort` helper that:
  - receives a controlled ROW frame；
  - decodes `record_image`；
  - checks `record_image_len == table->s->reclength`；
  - copies the image into `table->record[0]`；
  - verifies at least the first integer field can be read back when the table
    shape is compatible；
- add counters and MTR assertions for materialized rows；
- keep failures fail-closed inside the smoke path。

Hard stop:

- no optimizer eligibility changes；
- no `HAS_ORDER_BY` gate relaxation；
- no real worker thread or InnoDB path；
- no `Filesort::make_sortkey()` / `Sort_param` integration；
- no changes to default `ParallelScanIterator::Read()` behavior；
- if table shape is not a simple integer first field with a valid record buffer,
  the smoke must skip or fail closed without opening user-visible ORDER BY。

Required validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5b3_target --tmpdir=/tmp/pqt_m11e5b3_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5b3_full --tmpdir=/tmp/pqt_m11e5b3_full
```

Completion Report - M11-E5b-3 Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/parallel_query/sql_parallel.cc`；
  - `sql/parallel_query/pq_iterator.cc`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_commercial_order_by_frames.test`；
  - `mysql-test/suite/parallel_query/r/pq_commercial_order_by_frames.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - `Gather_operator::run_exchange_sort_smoke()` now accepts optional
    smoke-only `TABLE *leader_table` and the existing caller passes `table()`；
  - added `Exchange_sort::run_orderby_frame_materialization_smoke()`；
  - helper validates simple leader table shape, prepares a controlled record
    image, sends it through the ORDER BY frame contract, decodes it, copies the
    image back into `table->record[0]`, and reads the first integer field back
    through `Field::val_int()`；
  - helper temporarily sets first-field read/write bitmap bits and restores
    them before returning, fixing a debug assertion seen during the first
    targeted MTR run；
  - incompatible table shapes are counted as unsupported instead of failing the
    broader smoke path；
  - added counters:
    `Parallel_exchange_sort_frame_materialized_smoke_rows` and
    `Parallel_exchange_sort_frame_materialized_smoke_unsupported`。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - first targeted MTR exposed `Field_long::store()` write-set assertion；
  - after bitmap restore fix, targeted MTR passed:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
  - full `parallel_query` suite passed: 88/88。
- scope notes:
  - no optimizer, executor, AccessPath, InnoDB, worker-thread, `Filesort`, or
    `Sort_param` integration；
  - user-visible ORDER BY PQ remains closed by existing `HAS_ORDER_BY`
    boundary；
  - this is only a smoke-level record-image materialization step, not real
    `Exchange_sort::read_mq_record()` migration。

Review:

- Code/Doc/Test Review Agent first pass verdict: `REVISE`；
- no blocking code findings；
- required doc cleanup:
  - remove stale review text copied from earlier E5b steps；
  - update older proposed split numbering so E5b-2 is merge-edge smoke and
    E5b-3 is row materialization smoke。
- Code/Doc/Test Review Agent second pass verdict: `ACCEPT`；
- findings: none；
- confirmed E5b-3 remains smoke-only and does not enable real worker thread,
  InnoDB path, `Filesort` / `Sort_param`, or user-visible ORDER BY PQ；
- residual risks: materialization coverage is intentionally narrow and only
  covers simple leader table shape with integer first field and valid
  `record[0]`。

### M11-E5b-2: Merge-path Empty Worker / ERROR Edge Smoke

Coding scope:

- `pq_load_orderby_frame_batch()` now accepts FINISH-only empty worker queues；
- added `Exchange_sort::run_orderby_frame_merge_edge_smoke()` with a controlled
  3-queue shape:
  - worker 0 sends FINISH only；
  - worker 1 sends ERROR and must fail closed with an observable error count；
  - worker 2 sends one ROW then FINISH；
  - leader merges the empty worker batch plus the one-row batch and expects
    rowid `50`；
- added dedicated counters:
  `Parallel_exchange_sort_frame_merge_edge_smoke_rows`,
  `Parallel_exchange_sort_frame_merge_edge_smoke_finishes`,
  `Parallel_exchange_sort_frame_merge_edge_smoke_errors`；
- extended `pq_commercial_order_by_frames` and `pq_stats` for the edge counters。

Hard boundaries:

- still no optimizer eligibility change；
- still no user-visible ORDER BY PQ；
- still no InnoDB / handler / worker-thread integration；
- still no `table->record[0]` real materialization。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5b2_target --tmpdir=/tmp/pqt_m11e5b2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5b2_full --tmpdir=/tmp/pqt_m11e5b2_full
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，4/4；
- full `parallel_query` suite passed，88/88。

Review:

- Code/Doc/Test Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed FINISH-only empty worker is accepted for controlled merge；
- confirmed ERROR frame remains fail-closed and observable；
- residual risks stay out of scope for this step: real worker-thread,
  InnoDB, optimizer, `table->record[0]` materialization, abort propagation, and
  full Filesort-compatible comparison semantics。

## M11-E5c: User-visible ORDER BY Gate Design Review

Status: design-only completed，Review Agent accepted，waiting for commit。

Decision:

- do not relax `HAS_ORDER_BY` in `pq_optimizer` in this step；
- do not enable user-visible ORDER BY PQ before the commercial real path has a
  reviewed leader Filesort/Sort_param contract, worker ORDER BY frame producer,
  abort/error propagation, and result-order correctness tests；
- keep `pq_commercial_order_by` as the negative serial-boundary guard for now。

Commercial dependency summary:

- `ParallelScanIterator::pq_make_filesort()` reconstructs leader `Filesort` /
  order metadata, including saved order/group state and ordered-index fallback；
- `ParallelScanIterator::pq_init_record_gather()` installs `MQ_record_gather`
  and wires each worker MQ handle；
- `MQ_record_gather::mq_scan_init()` selects `Exchange_sort` when sort metadata
  exists；
- commercial `Exchange_sort::store_mq_record()` deep-copies worker records and
  rowid/ref data；
- commercial `Exchange_sort::read_mq_record()` picks the minimum cached worker
  record and writes it into leader `table->record[0]`；
- comparator semantics depend on `Sort_param::make_sortkey()`, Filesort key
  length, index-sort/reverse metadata, rowid/ref tie-break, and worker
  completion/error semantics。

Current branch readiness:

- completed:
  - independent ORDER BY frame contract；
  - controlled ROW/FINISH/ERROR frame decode；
  - controlled K-way merge with rowid tie-break；
  - FINISH-only empty worker and ERROR fail-closed smoke；
  - debug-only record-image materialization into `table->record[0]`；
  - negative user-visible ORDER BY serial-boundary MTR。
- not complete:
  - real worker ORDER BY frame producer；
  - `Filesort` / `Sort_param` construction and lifetime contract；
  - full commercial comparison semantics for ASC/DESC/NULL/order expressions；
  - abort cleanup from worker ERROR through `Exchange_sort` and iterator；
  - result correctness for multi-worker ORDER BY with LIMIT；
  - optimizer diagnostics for a reviewed visible ORDER BY candidate gate。

Proposed minimum visible-gate prerequisites:

1. E5d Filesort State Contract:
   leader-only construction smoke for `Filesort` / `Sort_param` without mutating
   normal executor state；must preserve `HAS_ORDER_BY` user-visible fallback。
2. E5e Worker ORDER BY Frame Producer Smoke:
   debug-only worker-side producer emits ORDER BY frames using current worker
   record images；leader consumes via `Exchange_sort` frame path。
3. E5f Abort/Error Cleanup Contract:
   controlled ERROR and leader abort paths detach MQ and clean up cached
   records without leaking worker/gather state。
4. E5g Exact-shape Visible Gate Design:
   only after E5d/E5e/E5f pass review, design a narrow gate such as
   single-table InnoDB full scan, simple direct-field `ORDER BY`, no DISTINCT,
   no GROUP/HAVING/window/subquery/order expressions, no DESC/NULL-sensitive
   shape unless explicitly covered, and no optimizer state mutation outside the
   PQ iterator lifecycle。

Required tests before any visible gate:

- keep `pq_commercial_order_by` negative serial-boundary assertions；
- add dedicated debug MTR for `Filesort` state smoke；
- add debug MTR for worker ORDER BY frame producer and leader materialization；
- add ERROR/KILL cleanup MTR for ORDER BY frame path；
- add future positive result-order MTR only after the visible-gate design is
  separately accepted。

Hard stop:

- if opening `HAS_ORDER_BY` would route SQL into current debug-only
  `run_exchange_sort_smoke()` path, stop；
- if `Filesort` or `Sort_param` setup requires unreviewed JOIN/QEP_TAB state
  mutation, stop；
- if worker frame format cannot be kept distinct from existing PQWR frames,
  stop；
- if DESC, NULL ordering, LIMIT, rowid/ref tie-break, or abort cleanup cannot be
  tested independently, keep user-visible ORDER BY serial。

Design Review - M11-E5c:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed E5c keeps `HAS_ORDER_BY` as the user-visible serial boundary；
- confirmed SQL ORDER BY is not routed into current debug-only
  `run_exchange_sort_smoke()` or inert `Exchange_sort::read_mq_message()`；
- suggested next phase: E5d Filesort State Contract, leader-only/debug-only,
  verifying `Filesort` / `Sort_param` construction, lifetime, saved
  ORDER/GROUP restoration, ordered-index/reverse metadata, and no normal
  executor state mutation。

## M11-E5d: Filesort State Contract

Status: design taskbook completed，Review Agent accepted and committed；no
source code change。

Goal:

- establish a leader-only/debug-only contract for commercial ORDER BY
  `Filesort` / `Sort_param` state before any visible ORDER BY gate；
- prove which commercial helpers can be ported safely and which require
  separate optimizer-state preservation work；
- keep `HAS_ORDER_BY` serial boundary unchanged。

Commercial reference:

- `ParallelScanIterator::pq_make_filesort()`:
  - restores saved GROUP or ORDER lists when the optimizer optimized them away；
  - reconstructs index-order sort metadata when `ORDERED_INDEX_ORDER_BY` is used；
  - creates a leader `Filesort` when `m_order || m_stable_sort`；
  - rewrites ORDER direction for reverse range scans；
  - relies on helper functions such as `restore_list()`,
    `restore_optimized_group_order()`, `get_table_key_fields()`, and
    `set_key_order()`。
- `Exchange_sort::init()`:
  - calls `Filesort::make_sortorder()`；
  - marks DESC groups；
  - creates and initializes `Sort_param` using `init_for_filesort()`；
  - allocates key buffers and rowid/ref tie-break buffers；
  - expects stable rowid/ref ownership from handler metadata。

Current branch gap:

- current branch has debug frame/merge/materialization smokes but no
  `Filesort` / `Sort_param` construction；
- current branch does not yet carry the commercial saved ORDER/GROUP helper
  layer；
- current `pq_optimizer` intentionally rejects `query_block->is_ordered()` with
  `HAS_ORDER_BY`；
- current `Exchange_sort::read_mq_message()` remains inert and must not become
  the user-visible ORDER BY path in E5d。

Allowed files for E5d design/coding:

- design:
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
  - `Docs/pq_tasks/README.md`。
- coding only after design review:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/parallel_query/sql_parallel.cc`；
  - focused MTR under `mysql-test/suite/parallel_query/`；
  - helper declarations only if review proves they can be isolated from
    optimizer mutation。

Forbidden in E5d:

- relaxing `HAS_ORDER_BY`；
- modifying `pq_optimizer.cc` eligibility to allow visible ORDER BY；
- default `ParallelScanIterator::Read()` changes；
- worker thread or InnoDB path changes；
- real `MQ_record_gather` replacement；
- importing saved ORDER/GROUP restore helpers into optimizer state without a
  separate review。

Proposed E5d substeps:

1. E5d-0 Read-only Helper Inventory:
   verify current branch availability of saved ORDER/GROUP state and helper
   equivalents；produce a short gap list before coding。
2. E5d-1 Compile-only Filesort Contract Shape:
   add a fail-closed `PQ_orderby_filesort_contract` structure or helper API that
   records whether a leader table/order state is usable；no `Filesort`
   allocation yet if helper state is absent。
3. E5d-2 Leader-only Filesort Smoke:
   only if E5d-0/E5d-1 pass review，construct a leader `Filesort` for a
   narrowly controlled already-present `QEP_TAB::filesort` or direct order
   pointer；do not restore optimized-away ORDER/GROUP state in this step。
4. E5d-3 Sort_param Smoke:
   initialize `Sort_param` against controlled leader table/order metadata and
   generate a sort key for synthetic record images；keep all state local and
   clean up before returning。

Required tests:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16` for coding steps；
- targeted MTR:
  `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`；
- full `parallel_query` suite；
- `pq_commercial_order_by` must continue to show `Not parallel HAS_ORDER_BY`
  and no visible ORDER BY PQ executed/workers/ranges deltas。

Hard stop:

- if current branch lacks saved ORDER/GROUP state equivalent to commercial
  `saved_join_order`, `saved_join_group_list`, or `saved_optimized_vars`, E5d
  coding must stop at helper inventory/compile-only shape；
- if `Filesort::make_sortorder()` or `Sort_param::init_for_filesort()` mutates
  JOIN/QEP_TAB/TABLE state outside the smoke, stop；
- if reverse index-order metadata cannot be proven from current AccessPath/QEP
  state, defer reverse/DESC support；
- if any MTR indicates visible ORDER BY no longer falls back through
  `HAS_ORDER_BY`, revert the E5d attempt。

Design Review - M11-E5d:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- required changes before commit: none；
- suggested next phase: M11-E5d-0 read-only helper inventory first；
- if current branch still lacks `saved_join_order`, `saved_join_group_list`, or
  `saved_optimized_vars` equivalents, stop E5d coding at inventory or
  compile-only fail-closed contract shape；
- keep `pq_commercial_order_by` as the negative guard for `HAS_ORDER_BY`, with
  zero executed/workers/ranges deltas before any later visible ORDER BY design。

### M11-E5d-0: Read-only Helper Inventory

Status: read-only inventory completed，Review Agent accepted and committed；no
source code change。

Inventory commands:

```bash
rg -n "saved_join_order|saved_join_group_list|saved_optimized_vars|\
saved_order_list_ptrs|optimized_order_flags|optimized_group_flags|\
restore_optimized_group_order|record_optimized_group_order|restore_list\\(" \
  sql include
rg -n "m_ordered_index_usage|ORDERED_INDEX_ORDER_BY|range_scan\\(\\)|\
INDEX_RANGE_SCAN|reverse" \
  sql/sql_optimizer.h sql/sql_optimizer.cc sql/parallel_query \
  sql/join_optimizer sql/range_optimizer
```

Current branch findings:

- missing commercial saved ORDER/GROUP state:
  - no `saved_join_order`；
  - no `saved_join_group_list`；
  - no `saved_optimized_vars`；
  - no `optimized_order_flags` / `optimized_group_flags`；
  - no local `restore_list()` / `restore_optimized_group_order()` /
    `record_optimized_group_order()` helper layer。
- available current-branch metadata:
  - `JOIN::m_ordered_index_usage` and `ORDERED_INDEX_ORDER_BY` exist；
  - `QEP_TAB::range_scan()` and `AccessPath::INDEX_RANGE_SCAN` exist；
  - range `reverse` metadata and helper APIs exist in range/join optimizer
    code；
  - current PQ optimizer already has conservative secondary/range gates that
    reject reverse scans in user-visible PQ paths。

Commercial helper gap:

- commercial `pq_make_filesort()` depends on saved optimized ORDER/GROUP state
  to rebuild order lists after optimizer transformations；
- direct port without these helpers risks using stale/null `JOIN::order.order`
  or mutating optimizer-owned ORDER structures；
- commercial `set_key_order()` also depends on `REF_SLICE_PQ_TMP` and saved
  `Ref_item_array` behavior not yet isolated in current branch。

Conclusion:

- do not start E5d-2 leader Filesort construction yet；
- E5d coding, if any, must start with E5d-1 compile-only fail-closed contract
  shape that reports missing saved ORDER/GROUP helpers as unsupported；
- full commercial helper migration requires a separate optimizer-state
  preservation task before any `Filesort` / `Sort_param` construction smoke；
- `HAS_ORDER_BY` remains a hard serial boundary。

Review request:

- confirm E5d-0 inventory is accurate；
- decide whether E5d-1 compile-only fail-closed shape is worthwhile now, or
  whether the next task should be a dedicated saved ORDER/GROUP helper design；
- confirm no coding should attempt `Filesort::make_sortorder()` until helper
  state is available or a narrower already-present `QEP_TAB::filesort` case is
  proven safe.

Inventory Review - M11-E5d-0:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- required changes before commit: none；
- suggested next phase: prefer a dedicated saved ORDER/GROUP helper design
  before any `Filesort::make_sortorder()` work；
- E5d-1 fail-closed contract shape remains reasonable only if it reports
  unsupported when saved helper state is absent；
- do not proceed to E5d-2 Filesort construction yet。

### M11-E5d-S0: Saved ORDER/GROUP Helper Design

Status: design-only taskbook completed，Review Agent accepted；no source code
change。

Goal:

- define the smallest current-branch equivalent for the commercial saved
  ORDER/GROUP helper layer required by `pq_make_filesort()`；
- avoid directly constructing `Filesort` / `Sort_param` until saved optimizer
  state ownership and restore semantics are reviewed；
- keep `HAS_ORDER_BY` as a user-visible serial boundary while helper state is
  introduced and smoke-tested。

Commercial reference:

- commercial `JOIN::save_optimized_vars()` records:
  `grouped`, `group_optimized_away`, `implicit_grouping`,
  `need_tmp_before_win`, `simple_group`, `simple_order`,
  `streaming_aggregation`, `m_ordered_index_usage`, `skip_sort_order`,
  `select_distinct`, and `having_cond`；
- commercial helper state also tracks `saved_join_order`,
  `saved_join_group_list`, `optimized_order_flags`,
  `optimized_group_flags`, and `pq_saved_having_cond`；
- commercial helper APIs include `record_optimized_group_order()`,
  `restore_optimized_group_order()`, `restore_list()`, and key-order helpers
  used by the ORDER BY path；
- current branch already has `Group_list_ptrs`, `ORDER_with_src`,
  `JOIN::m_ordered_index_usage`, `QEP_TAB::range_scan()`, range reverse
  metadata, and PQ clone hooks, but not the saved helper layer itself。

Design decision:

- do not add `Filesort` construction in this step；
- first add an explicit saved ORDER/GROUP state contract, then code it as a
  fail-closed helper；
- prefer a PQ-local sidecar state object for the first coding step unless
  review proves direct `JOIN` fields are needed for commercial compatibility；
- direct `JOIN` field additions remain allowed only after the helper API and
  clone/restore ownership are documented, because optimizer-owned state is
  shared across normal serial execution and PQ clone paths。

Proposed saved state contract:

- captured scalar optimizer flags:
  `grouped`, `group_optimized_away`, `implicit_grouping`,
  `need_tmp_before_win`, `simple_group`, `simple_order`,
  `streaming_aggregation`, `m_ordered_index_usage`, `skip_sort_order`,
  `select_distinct`, and `having_cond`；
- captured ORDER/GROUP list metadata:
  `Query_block::saved_order_list_ptrs`, `Query_block::saved_group_list_ptrs`
  or their current-branch equivalents, plus local optimized ORDER/GROUP flags；
- restore helpers must rebuild local `SQL_I_List<ORDER>` / `ORDER_with_src`
  views without mutating optimizer-owned ORDER nodes unexpectedly；
- clone interaction must be explicit: a worker/clone JOIN can copy saved helper
  state only after source JOIN state is known complete and immutable for the
  query execution window；
- unsupported/missing state must return a clear fail-closed result and must not
  silently fall through into `Filesort::make_sortorder()`。

Allowed files for the later coding step:

- `sql/parallel_query/pq_optimizer.h`；
- `sql/parallel_query/pq_optimizer.cc`；
- `sql/sql_optimizer.h` only if direct `JOIN` fields are approved by review；
- `sql/sql_optimizer.cc` only for save/restore hook wiring approved by review；
- focused MTR under `mysql-test/suite/parallel_query/` for diagnostics；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden until a later reviewed step:

- relaxing `HAS_ORDER_BY` eligibility；
- constructing `Filesort` / `Sort_param`；
- invoking `Filesort::make_sortorder()`；
- changing `ParallelScanIterator::Read()` user-visible ORDER BY behavior；
- starting worker threads or wiring real worker ORDER BY producers；
- importing commercial key-order helpers that depend on unresolved
  `REF_SLICE_PQ_TMP` / `Ref_item_array` semantics。

Proposed task split:

1. E5d-S1 Saved State Contract Shape:
   add compile-only saved ORDER/GROUP state container and helper API that
   reports unsupported until all required current-branch pointers are present。
2. E5d-S2 Leader Save/Restore Smoke:
   debug-only smoke saves scalar flags and ORDER/GROUP pointer metadata, then
   restores into local lists and verifies original serial optimizer state is
   unchanged。
3. E5d-S3 Clone Copy Contract:
   verify PQ clone copy can carry saved state without taking ownership of
   optimizer ORDER nodes or `Item` objects。
4. E5d-1 Filesort Contract Shape:
   only after S1-S3 pass review, create fail-closed `pq_make_filesort()`
   contract shape that refuses unsupported saved state。
5. E5d-2 Leader-only Filesort Smoke:
   only after E5d-1 review, evaluate a debug-only `Filesort` construction smoke
   for a single-table exact ORDER BY shape。

Required validation for design-only S0:

- no build/test required；
- independent design/task Review Agent must confirm scope, ownership, and hard
  stops before coding S1。

Acceptance checklist:

- no source edits in S0；
- design names the missing commercial fields/helpers；
- design chooses sidecar-first unless direct `JOIN` fields are explicitly
  justified；
- design forbids `Filesort` construction and user-visible ORDER BY PQ；
- next coding step is small enough to review independently。

Review request:

- confirm sidecar-first saved state contract is a safe first step；
- confirm whether direct `JOIN` fields should be delayed until S1 review；
- confirm S1-S3 must precede E5d-1/E5d-2；
- check that the allowed/forbidden file boundary prevents accidental optimizer
  or execution-path activation。

Design/Task Review - M11-E5d-S0:

- Review Agent verdict: `ACCEPT`；
- agreed S0 is design-only and S1-S3 must precede E5d-1/E5d-2；
- agreed sidecar-first is the safe first step, with direct `JOIN` fields
  delayed unless S1/S2 prove capture timing must bind to `JOIN::optimize()`
  lifecycle；
- confirmed hard stops are sufficient: no `HAS_ORDER_BY` relaxation, no
  `Filesort` / `Sort_param` construction, no `make_sortorder()`, no
  `ParallelScanIterator::Read()` user-visible change, no worker-thread ORDER BY
  producer；
- next step: E5d-S1 compile-only saved state contract shape, fail-closed and
  without user-visible ORDER BY activation。

### M11-E5d-S1: Saved State Contract Shape

Status: coding, validation, Code/Doc/Test Review, and commit completed。

Goal:

- add a compile-only sidecar contract for saved ORDER/GROUP optimizer state；
- capture only publicly available JOIN / Query_block scalar state；
- report unsupported until the commercial saved helper layer is introduced；
- keep the helper observable through a debug-only smoke without enabling real
  Filesort or user-visible ORDER BY PQ。

Implementation:

- added `PQSavedOrderGroupContractStatus` and
  `PQSavedOrderGroupContract` in `pq_optimizer.h`；
- added `pq_build_saved_order_group_contract()` in `pq_optimizer.cc`；
- the helper captures current public scalar state:
  `has_order`, `has_group`, `has_having`, `grouped`,
  `group_optimized_away`, `implicit_grouping`, `need_tmp_before_win`,
  `simple_group`, `simple_order`, `streaming_aggregation`,
  `skip_sort_order`, `select_distinct`, and `m_ordered_index_usage`；
- the helper deliberately returns unsupported with
  `UNSUPPORTED_MISSING_SAVED_HELPERS` because current branch still lacks
  `saved_join_order`, `saved_join_group_list`, optimized ORDER/GROUP flags, and
  restore helper APIs；
- added `pq_saved_order_group_contract_smoke` DBUG hook inside the existing
  ORDER BY `HAS_ORDER_BY` rejection path；
- added status variables:
  `Parallel_saved_order_group_contract_attempts` and
  `Parallel_saved_order_group_contract_unsupported`；
- added `pq_saved_order_group_contract` MTR to verify the smoke increments
  attempts/unsupported while executed/workers/ranges remain zero；
- updated `pq_stats` for the two new status variables。

Hard boundaries preserved:

- no `HAS_ORDER_BY` relaxation；
- no `Filesort` / `Sort_param` construction；
- no `Filesort::make_sortorder()` call；
- no direct `JOIN` field additions for saved commercial state；
- no `Query_block` private ORDER/GROUP list access；
- no `ParallelScanIterator::Read()` behavior change；
- no worker thread or real ORDER BY producer wiring。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5ds1_target --tmpdir=/tmp/pqt_m11e5ds1_target
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，4/4 including `shutdown_report`。
- full `parallel_query` suite passed，89/89。

Review request:

- confirm the sidecar helper is sufficiently fail-closed；
- confirm the DBUG hook does not alter default ORDER BY eligibility；
- confirm status variables and MTR assertions are scoped to S1；
- confirm next step remains E5d-S2 leader save/restore smoke, not Filesort
  construction。

Code/Doc/Test Review - M11-E5d-S1:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `HAS_ORDER_BY` remains rejected and user-visible ORDER BY PQ is not
  enabled；
- confirmed sidecar captures only public `Query_block` / `JOIN` scalar state,
  does not access private saved ORDER/GROUP list pointers, and does not add
  direct `JOIN` saved fields；
- confirmed no `Filesort` / `Sort_param` construction, no
  `make_sortorder()`, no `ParallelScanIterator::Read()` change, and no worker
  path change；
- confirmed DBUG hook is scoped to
  `pq_saved_order_group_contract_smoke` and MTR verifies
  attempts/unsupported growth while executed/workers/ranges stay zero；
- next step remains E5d-S2 leader save/restore smoke。

Commit:

- `5ad7ed85d8f` Add PQ M11E saved order group contract。

### M11-E5d-S2: Leader Save/Restore Smoke Design

Status: design-only taskbook completed；Review Agent re-review accepted；no
source code change。

Goal:

- add a debug-only smoke around the S1 sidecar to prove leader scalar optimizer
  state can be captured, temporarily perturbed inside the smoke, and restored
  without changing normal serial ORDER BY behavior；
- keep this as a leader-local sidecar smoke, not a commercial
  `JOIN::save_optimized_vars()` port；
- prepare the ownership proof needed before S3 clone-copy contract and later
  Filesort contract shape。

Design boundary:

- S2 may only operate on values already captured by
  `PQSavedOrderGroupContract`；
- S2 must not access `Query_block::order_list_ptrs` or
  `Query_block::group_list_ptrs` because they are private and ownership is not
  reviewed yet；
- S2 must not mutate real optimizer `ORDER` nodes or `Item` pointers；
- any temporary mutation, if used, must be limited to scalar `JOIN` fields and
  restored in the same helper scope before returning；
- if state capture is incomplete or unsupported, the smoke must still
  fail-closed and increment an unsupported counter。

Perturb/restore hard constraints:

- use an RAII/scope-guard object for restore so all early returns restore the
  original values；
- inside the perturbation window, do not call optimizer, executor, EXPLAIN,
  diagnostic, tracing, MTR-visible, or other complex functions that may read
  JOIN state；the window may only perform local assignments and local checks；
- only trivially reversible bool/enum/integer scalar fields may be perturbed；
- do not perturb `having_cond`, `having_for_explain`, `where_cond`, `ORDER *`,
  `Item *`, ORDER/GROUP lists, QEP_TAB, AccessPath, TABLE, handler, MEM_ROOT, or
  ownership-bearing fields；
- after RAII restore, rebuild the S1 sidecar contract and compare every
  captured scalar used by S2 against the initial snapshot；
- if any restored field mismatches, count only unsupported/fail-closed and do
  not increment success；
- even on success, the smoke must keep the full commercial helper contract in
  `UNSUPPORTED_MISSING_SAVED_HELPERS` state and must not produce `READY`。

Allowed files for coding after review:

- `sql/parallel_query/pq_optimizer.h`；
- `sql/parallel_query/pq_optimizer.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/mysqld.cc`；
- focused MTR under `mysql-test/suite/parallel_query/`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden files / actions:

- no `sql/sql_optimizer.h` or `sql/sql_optimizer.cc` changes in S2；
- no direct `JOIN` saved-state fields；
- no `Filesort` / `Sort_param` construction；
- no `Filesort::make_sortorder()`；
- no `HAS_ORDER_BY` relaxation；
- no `ParallelScanIterator::Read()` change；
- no worker thread, InnoDB, handler, MQ, or real ORDER BY producer wiring。

Proposed coding shape:

- add a debug-only helper such as
  `pq_run_saved_order_group_restore_smoke(Query_block *, JOIN *)`；
- helper captures the initial S1 contract；
- helper snapshots a small scalar subset on the stack, for example
  `simple_order`, `simple_group`, `skip_sort_order`,
  `need_tmp_before_win`, and `select_distinct`；
- helper temporarily flips only those scalar fields in a reversible way；
- helper restores the original scalar values unconditionally before return；
- helper rebuilds the S1 contract and verifies restored values match the
  initial snapshot；
- helper reports success only for this scalar restore proof and still reports
  the full commercial saved ORDER/GROUP helper layer as unsupported。

Required observability:

- new status counters:
  `Parallel_saved_order_group_restore_smoke_attempts`；
  `Parallel_saved_order_group_restore_smoke_success`；
  `Parallel_saved_order_group_restore_smoke_unsupported`。
  The `success` counter means only "scalar perturb/restore smoke succeeded"；
  it does not mean commercial saved ORDER/GROUP helper readiness。
- DBUG flag:
  `pq_saved_order_group_restore_smoke`。
- MTR:
  - first run a matching ORDER BY `EXPLAIN` without DBUG and verify restore
    smoke counters do not grow；
  - run an ORDER BY `EXPLAIN` under the DBUG flag；
  - verify restore smoke attempts/success grow；
  - verify S1 contract unsupported still grows or remains explicitly
    fail-closed；
  - verify `Parallel_queries_executed`,
    `Parallel_workers_launched`, and `Parallel_ranges_dispatched` do not grow；
  - verify `pq_commercial_order_by` still reports
    `Not parallel HAS_ORDER_BY`。

Required validation for coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5ds2_target --tmpdir=/tmp/pqt_m11e5ds2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5ds2_full --tmpdir=/tmp/pqt_m11e5ds2_full
```

Acceptance checklist:

- no source edits before design review；
- smoke is debug-only and reversible；
- scalar restore proof does not imply full commercial helper readiness；
- user-visible ORDER BY remains serial；
- next step after S2 is S3 clone-copy contract, not Filesort construction。

Review request:

- confirm the scalar perturb/restore smoke is safe enough for S2；
- confirm S2 must not modify `sql/sql_optimizer.*`；
- confirm the proposed counters/MTR prove no user-visible activation；
- confirm full commercial saved ORDER/GROUP helper readiness remains
  unsupported until S3 and later helper ownership review。

Design Review - M11-E5d-S2 first pass:

- Review Agent verdict: `REVISE`；
- required stronger perturb/restore constraints:
  - use RAII/scope guard for restore across early returns；
  - no complex function calls inside the perturb window；
  - only trivially reversible scalar fields may be changed；
  - no pointer/list/QEP_TAB/AccessPath/MEM_ROOT ownership fields；
  - rebuild S1 contract after restore and compare captured scalars；
  - success must not imply commercial helper readiness or `READY` status；
- confirmed S2 must not modify `sql/sql_optimizer.*`；
- requested MTR include a no-DBUG negative window proving restore counters do
  not grow by default；
- confirmed next step remains S3 clone-copy contract, not Filesort
  construction。

Design Review - M11-E5d-S2 re-review:

- Review Agent verdict: `ACCEPT`；
- confirmed RAII/scope-guard restore and early-return coverage are required；
- confirmed perturb window may only perform local assignments/checks and must
  not call optimizer/executor/EXPLAIN/diagnostic/tracing/MTR-visible functions；
- confirmed pointer/list/QEP_TAB/AccessPath/TABLE/handler/MEM_ROOT fields are
  forbidden；
- confirmed restore must rebuild S1 contract and compare captured scalars；
- confirmed success means only scalar perturb/restore smoke success and never
  commercial helper readiness or `READY`；
- confirmed no-DBUG negative MTR and hard bans on `sql/sql_optimizer.*`,
  Filesort construction, `HAS_ORDER_BY` relaxation, `Read()` changes, worker,
  handler, MQ, or real ORDER BY producer wiring；
- next step: code S2 under this design, then proceed to S3 clone-copy contract。

Completion Report - M11-E5d-S2 Coding:

- changed files:
  - `sql/parallel_query/pq_optimizer.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_saved_order_group_contract.test`；
  - `mysql-test/suite/parallel_query/r/pq_saved_order_group_contract.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added debug-only `pq_saved_order_group_restore_smoke` hook on the existing
    ORDER BY `HAS_ORDER_BY` rejection path；
  - added RAII scalar restore guard for `simple_order`, `simple_group`,
    `skip_sort_order`, `need_tmp_before_win`, and `select_distinct`；
  - perturb window performs only local scalar assignment and local checks；
  - after RAII restore, the helper rebuilds the S1 sidecar contract and
    compares restored scalar subset against the initial snapshot；
  - success is counted only when restore matched and S1 contract remains
    `UNSUPPORTED_MISSING_SAVED_HELPERS`；
  - added counters:
    `Parallel_saved_order_group_restore_smoke_attempts`,
    `Parallel_saved_order_group_restore_smoke_success`, and
    `Parallel_saved_order_group_restore_smoke_unsupported`。
- hard boundaries:
  - no `sql/sql_optimizer.*` changes；
  - no direct `JOIN` saved-state fields；
  - no ORDER/GROUP list, `Item *`, `ORDER *`, QEP_TAB, AccessPath, TABLE,
    handler, or MEM_ROOT ownership mutation；
  - no `Filesort` / `Sort_param` construction or `make_sortorder()`；
  - no `HAS_ORDER_BY` relaxation, `Read()` change, worker, handler, MQ, or real
    ORDER BY producer wiring。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4。
  - full `parallel_query` suite passed，89/89。

Review request:

- confirm RAII restore covers early returns and the perturb window stays local；
- confirm success counter is only scalar smoke success, not commercial helper
  readiness；
- confirm default no-DBUG ORDER BY still does not enter restore smoke；
- confirm user-visible ORDER BY PQ remains closed by `HAS_ORDER_BY`；
- confirm next step remains S3 clone-copy contract。

Code/Doc/Test Review - M11-E5d-S2:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed RAII restore covers the perturb scope and the perturb window only
  performs local scalar assignment/comparison；
- confirmed no `sql/sql_optimizer.*` changes, no direct JOIN saved fields, and
  no ORDER/GROUP list, `Item *`, `ORDER *`, QEP_TAB, AccessPath, MEM_ROOT,
  TABLE, or handler mutation；
- confirmed restore success is counted only when the restored sidecar remains
  `UNSUPPORTED_MISSING_SAVED_HELPERS` and the scalar subset matches；
- confirmed MTR covers no-DBUG negative, DBUG positive, zero
  executed/workers/ranges deltas, and `HAS_ORDER_BY` serial boundary；
- next step remains S3 clone-copy contract。

## Risk Areas

- `Filesort` / `Sort_param` 可能修改 JOIN/QEP_TAB 状态；
- worker frame 与 leader TABLE/Field 布局不一致会导致错误 materialize；
- DESC index、stable output、rowid tie-break 容易产生结果顺序差异；
- ORDER BY 与 GROUP BY、LIMIT、range/ref/ICP 组合风险高；
- 一次性打开 optimizer eligibility 会绕过当前 MTR 护栏。

## Docs-Design Review

Review Agent returned `ACCEPT`:

- E0 is strictly design-only；
- E1-E5 split avoids premature optimizer/AccessPath/handler/InnoDB/real worker
  launch；
- `HAS_ORDER_BY` serial boundary is preserved；
- ORDER BY Explorer and ref/ICP Explorer conclusions are absorbed；
- no blocking findings。

Follow-up applied before commit:

- cleaned stale M11-D6 review status in the main M11 taskbook；
- added required E1/E2 negative assertions for real ORDER BY SQL: preserve
  `HAS_ORDER_BY` diagnostics and ensure PQ executed/workers/ranges counters do
  not grow unless a later reviewed gate permits it。

## Explorer Findings

ORDER BY Explorer returned:

- commercial call chain:
  `ParallelScanIterator::pq_make_filesort()` builds leader Filesort/order
  metadata；`ParallelScanIterator::pq_init_record_gather()` creates
  `MQ_record_gather`；`MQ_record_gather::mq_scan_init()` selects
  `Exchange_sort` when sort metadata exists；`Exchange_sort::read_mq_record()`
  performs K-way merge and writes `table->record[0]`；
- current repository has only synthetic `Exchange_sort` merge smoke and an empty
  real `read_mq_message()` placeholder；
- current `ParallelScanIterator::Read()` consumes only the D6
  `Exchange_nosort` row-image path；
- current optimizer still rejects any ORDER BY through `HAS_ORDER_BY`；
- immediate coding is not recommended；E0 docs/review must precede E1。

Ref/ICP Explorer returned:

- commercial ref/ICP worker path is a worker pull-row path through
  `PQblockScanIterator` / `PQRefIterator` / `handler::ha_pq_next()` /
  InnoDB `pq_worker_scan_next()`；
- current repository intentionally uses typed worker contexts and has
  `pq_worker_scan_next()` disabled until worker mutable scan state is complete；
- current M9 secondary/ref/ICP gates are leader-local producer gates, not
  worker-safe commercial ref/ICP；
- M11-F should get its own taskbook/review and must not be mixed into M11-E。

## Agent Task Prompt

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Agent。
主控 Agent 是 Codex。
当前任务是 M11-E0 ORDER BY / Exchange_sort Real Path Gate。

请只做设计，不改源码。

请阅读：
- Docs/pq_tasks/README.md；
- Docs/pq_tasks/commercial-port-m11-main-architecture-restart.md；
- Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md；
- Docs/pq_tasks/commercial-port-m8-order-by-gather-merge.md；
- 当前仓 sql/parallel_query/exchange_sort.*；
- 当前仓 sql/parallel_query/sql_parallel.*；
- 当前仓 sql/parallel_query/pq_iterators.*；
- 商用仓对应文件：
  /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/exchange_sort.*；
  /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/sql_parallel.*；
  /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_iterators.*。

输出：
1. 商用 ORDER BY path 调用链；
2. 当前仓缺口；
3. E1-E4 拆分是否合理；
4. 是否可以立即编码 E1；
5. 必须保留的 hard stop 条件。
```

## Review Checklist

- E0 remains design-only；
- no source files are changed；
- task split does not require optimizer/AccessPath/InnoDB changes before E3/E4；
- `HAS_ORDER_BY` serial boundary remains explicit；
- review decides whether E1 is safe to code next。

## M11-E1: Exchange_sort Commercial Shape Compile-only

Status: coding, validation, and Code-Docs-Test Review completed；ready to
commit。

Goal:

- add a compile-only commercial-shape boundary to current `Exchange_sort`；
- keep existing synthetic smoke behavior unchanged；
- keep real ORDER BY SQL rejected by `HAS_ORDER_BY`；
- do not consume real MQ rows, do not create `Filesort`, do not modify JOIN,
  optimizer, AccessPath, handler, InnoDB, or worker launch behavior。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook；
- `Docs/pq_tasks/README.md` / main M11 taskbook for status only；
- `mysql-test/suite/parallel_query/t/pq_commercial_order_by.test` and matching
  result only for stronger negative serial-boundary assertions。

Forbidden files:

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_executor.*`；
- `sql/sql_select.*`；
- `sql/join_optimizer/access_path.*`；
- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`。

Required implementation direction:

1. introduce narrow current-repo equivalents for the commercial ORDER BY record
   shape:
   - cached record wrapper；
   - per-worker batch wrapper；
   - batch compare state；
2. add `Exchange_sort` fields for future commercial path ownership:
   - min record array；
   - per-worker record groups；
   - heap pointer；
   - stable output / index-sort flags or metadata placeholders；
3. add fail-closed methods such as `init_order_gather_shape()`,
   `read_ordered_record_shape()`, and `cleanup_order_gather_shape()`；
4. these methods must be callable/compilable but not used by default SQL；
5. constructor defaults must preserve existing synthetic smoke behavior；
6. `read_mq_message()` must remain inert for real ORDER BY path；
7. no `Filesort` / `Sort_param` construction in E1；if forward declarations are
   needed, keep them header-only and unused；
8. no new user-visible status counters unless needed to make compile-only shape
   observable；prefer no new counters for E1。

Required negative assertions:

- `pq_commercial_order_by` must continue to prove real ORDER BY SQL stays
  serial-boundary rejected by `HAS_ORDER_BY`；
- `Parallel_queries_executed`, `Parallel_workers_launched`, and
  `Parallel_ranges_dispatched` must not grow for real ORDER BY SQL；
- keep the existing `Parallel_queries_fallback` delta assertion at 0, but do
  not use fallback growth as proof of ORDER BY PQ execution because current
  behavior is optimizer rejection rather than iterator fallback；
- existing `Parallel_exchange_sort_smoke_*` synthetic counters must remain
  stable。

Required MTR update:

- extend `pq_commercial_order_by` to snapshot `Parallel_workers_launched` and
  `Parallel_ranges_dispatched` around the real ORDER BY statements；
- assert `orderby_workers_launched_delta = 0`；
- assert `orderby_ranges_dispatched_delta = 0`；
- preserve existing `orderby_executed_delta = 0` and
  `orderby_fallback_delta = 0` assertions；
- preserve the `EXPLAIN` output showing `Not parallel HAS_ORDER_BY`。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e1_target --tmpdir=/tmp/pqt_m11e1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e1_full --tmpdir=/tmp/pqt_m11e1_full
```

Agent Task Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M11-E1 Exchange_sort Commercial Shape Compile-only。

请阅读：
- Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md；
- sql/parallel_query/exchange_sort.h；
- sql/parallel_query/exchange_sort.cc；
- sql/parallel_query/exchange.h；
- /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/exchange_sort.h；
- /Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/exchange_sort.cc。

任务目标：
1. 给当前 `Exchange_sort` 补 compile-only 商用字段/方法边界；
2. 默认 fail-closed，不读取真实 MQ，不修改 optimizer/AccessPath/handler/InnoDB；
3. 保持现有 synthetic order merge smoke 行为不变；
4. 更新 `pq_commercial_order_by` 负向断言，证明真实 ORDER BY SQL 不增长
   executed/workers/ranges。

完成后不要自行 commit。
```

Docs-Design Review:

- first review returned `REVISE`；
- required allowing focused `pq_commercial_order_by` test/result edits even
  without new counters；
- required negative assertions for `Parallel_queries_executed`,
  `Parallel_workers_launched`, and `Parallel_ranges_dispatched`；
- requested revisions have been applied；waiting for re-review。
- re-review returned `ACCEPT`。

Implementation:

- added compile-only `PQ_orderby_cached_record`,
  `PQ_orderby_record_batch`, and `PQ_orderby_batch_compare_state` shapes；
- added future commercial ownership placeholders to `Exchange_sort` for
  min-records, per-worker batches, heap pointer, worker count, stable-output
  flag, and index-sort flag；
- added fail-closed shape methods:
  `init_order_gather_shape()`, `read_ordered_record_shape()`, and
  `cleanup_order_gather_shape()`；
- kept `read_mq_message()` inert and did not consume real MQ rows；
- did not construct `Filesort` / `Sort_param`；
- did not modify optimizer, AccessPath, handler, InnoDB, worker launch,
  `Query_result_mq`, clone, or resolver files；
- extended `pq_commercial_order_by` to assert real ORDER BY SQL does not grow
  `Parallel_workers_launched` or `Parallel_ranges_dispatched` in addition to
  the existing `Parallel_queries_executed` and fallback assertions。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- `--record pq_commercial_order_by` executed successfully but MTR failed to
  copy the generated result file with errno 1；the generated result was already
  reflected in the working tree；
- targeted MTR `pq_commercial_order_by pq_stats` passed, 3/3；
- full `parallel_query` suite passed, 86/86。

Code-Docs-Test Review:

- Verdict: `ACCEPT`；
- findings: none；
- required fixes: none；
- confirmed source changes are compile-only shape state with no caller added；
- confirmed `read_mq_message()` remains inert and does not consume MQ；
- confirmed `pq_commercial_order_by` now asserts executed/fallback/workers/ranges
  deltas and preserves `Not parallel HAS_ORDER_BY`；
- confirmed no optimizer, AccessPath, handler, InnoDB, worker launch,
  `query_result_mq`, clone, or resolver files were modified。

## M11-E2: Sorted Row-frame Adapter Smoke

Status: coding, validation, and Code-Docs-Test Review completed；ready to
commit。

Goal:

- add a DBUG-only or smoke-only adapter path inside `Exchange_sort` that
  consumes controlled sorted row-frame-like records through the E1 cached record
  shape；
- prove the commercial-shape cached record/batch structures can drive a
  deterministic K-way merge；
- keep real MQ row consumption disabled；
- keep real ORDER BY SQL rejected by `HAS_ORDER_BY`；
- do not modify optimizer, AccessPath, handler, InnoDB, worker launch,
  `Query_result_mq`, clone, resolver, or `ParallelScanIterator` path。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.h` / `.cc` only if a dedicated smoke helper
  or counters are needed；
- `sql/mysqld.cc` and `pq_stats.result` only if new SHOW STATUS counters are
  added；
- focused MTR under `mysql-test/suite/parallel_query/` if the smoke becomes
  SQL-observable；
- this taskbook and progress docs。

Forbidden files:

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_executor.*`；
- `sql/sql_select.*`；
- `sql/join_optimizer/access_path.*`；
- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- `sql/parallel_query/pq_iterators.*` unless a later review explicitly moves
  E3 into scope。

Required implementation direction:

1. Use E1 `PQ_orderby_cached_record` / `PQ_orderby_record_batch` as the only
   row-frame adapter storage；
2. add a helper such as `run_cached_record_adapter_smoke(uint32 *rows_read)`；
3. helper builds controlled per-worker sorted record batches with:
   - sort key；
   - row id tie-break；
   - worker id；
   - row image payload placeholder；
4. helper performs a K-way merge through `binary_heap` and validates expected
   output order；
5. validate at least ASC and DESC/tie-break cases；
6. helper must not call `read_mq_message()` or consume real MQ handles；
7. `read_mq_message()` remains inert；
8. no SQL-visible ORDER BY path changes；
9. if counters are added, use E2-specific names and keep existing M8
   `Parallel_exchange_sort_smoke_*` stable unless intentionally extended。

Required negative assertions:

- `pq_commercial_order_by` still shows `Not parallel HAS_ORDER_BY`；
- real ORDER BY SQL still has zero delta for `Parallel_queries_executed`,
  `Parallel_workers_launched`, and `Parallel_ranges_dispatched`；
- no new worker-result, handler, or range counters grow from real ORDER BY SQL。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e2_target --tmpdir=/tmp/pqt_m11e2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e2_full --tmpdir=/tmp/pqt_m11e2_full
```

Agent Task Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Code Agent。
主控 Agent 是 Codex。
当前任务是 M11-E2 Sorted Row-frame Adapter Smoke。

请阅读：
- Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md；
- sql/parallel_query/exchange_sort.h；
- sql/parallel_query/exchange_sort.cc；
- sql/parallel_query/exchange.h；
- sql/parallel_query/sql_parallel.cc；
- mysql-test/suite/parallel_query/t/pq_commercial_order_by.test。

任务目标：
1. 使用 E1 cached record/batch shape 增加 controlled sorted row-frame adapter
   smoke；
2. 通过 binary_heap 验证 ASC、DESC/tie-break 输出顺序；
3. 不读取真实 MQ，不打开真实 ORDER BY SQL，不修改 optimizer/AccessPath/
   handler/InnoDB/worker launch；
4. 保持 `pq_commercial_order_by` 的 HAS_ORDER_BY serial boundary 断言。

完成后不要自行 commit。
```

Docs-Design Review:

- Verdict: `ACCEPT`；
- confirmed E2 remains smoke-only；
- confirmed it uses E1 cached record/batch shape and `binary_heap`；
- confirmed allowed/forbidden files are narrow enough；
- confirmed negative assertions protect the `HAS_ORDER_BY` serial boundary。

Implementation:

- added `Exchange_sort::run_cached_record_adapter_smoke()`；
- added controlled cached-record K-way merge using E1
  `PQ_orderby_cached_record` / `PQ_orderby_record_batch` storage；
- validated ASC and DESC/tie-break ordering through `binary_heap`；
- did not call `read_mq_message()` or consume real MQ handles；
- kept `read_mq_message()` inert；
- reused existing `Gather_operator::run_exchange_sort_smoke()` and existing
  `Parallel_exchange_sort_smoke_*` counters by adding cached adapter rows to
  the smoke row total；
- did not modify optimizer, AccessPath, handler, InnoDB, worker launch,
  `Query_result_mq`, clone, resolver, or `ParallelScanIterator` files。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR `pq_commercial_order_by pq_stats` passed, 3/3；
- full `parallel_query` suite passed, 86/86。

Code-Docs-Test Review:

- Verdict: `ACCEPT`；
- findings: none；
- required fixes: none；
- confirmed the cached adapter smoke uses only controlled E1 cached
  record/batch inputs and local `binary_heap` merge；
- confirmed it does not call `read_mq_message()` or consume MQ handles；
- confirmed `read_mq_message()` remains inert；
- confirmed `Gather_operator::run_exchange_sort_smoke()` only extends existing
  smoke counters；
- confirmed no optimizer, AccessPath, handler, InnoDB, worker launch,
  `Query_result_mq`, clone/resolver, or `ParallelScanIterator` files were
  modified；
- residual risk: real filesort key encoding, collation, NULL handling, and live
  worker/MQ row frames remain deferred beyond E2。

## M11-E3: ParallelScanIterator Order Gather Debug Path

Status: design taskbook completed；Docs-Design Review accepted；ready to code。

Goal:

- design a debug-only bridge from the existing D6
  `ParallelScanIterator` row-value path to an ORDER gather smoke path；
- prove the commercial iterator can select an order-gather branch without
  enabling real ORDER BY SQL or optimizer eligibility changes；
- keep E3 DOP=1 or controlled in-process smoke only；
- do not start worker threads, clone JOIN, construct real `Filesort`, consume
  real MQ, or modify handler/InnoDB。

Scope:

- design only in this taskbook；
- E3 coding must be a separate commit after review；
- E3 must remain behind a dedicated DBUG flag, for example
  `pq_parallel_scan_iterator_order_gather_smoke`；
- E3 may reuse `Exchange_sort::run_cached_record_adapter_smoke()` but must not
  treat it as real SQL ORDER BY execution；
- E3 must not remove or weaken `HAS_ORDER_BY` rejection for real ORDER BY SQL。
- E3 must not return visible ordered SQL rows from `Exchange_sort`；visible
  ordered result correctness is deferred to a later reviewed task。

Allowed files for future coding:

- `sql/parallel_query/pq_iterators.h`；
- `sql/parallel_query/pq_iterators.cc`；
- `sql/parallel_query/pq_iterator.cc` only for a DBUG-only bridge hook similar
  to D6；
- `sql/parallel_query/exchange_sort.h` / `.cc` only for helper accessors needed
  by the smoke；
- `sql/parallel_query/sql_parallel.h` / `.cc` only for helper/counter wiring；
- `sql/mysqld.cc` and `pq_stats.result` only if E3-specific counters are added；
- focused MTR under `mysql-test/suite/parallel_query/`；
- this taskbook and progress docs。

Forbidden files:

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_executor.*`；
- `sql/sql_select.*`；
- `sql/join_optimizer/access_path.*`；
- `storage/innobase/**`；
- `sql/handler.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/pq_resolver*`；
- default user-visible ORDER BY PQ eligibility changes。

Required design direction:

1. add a DBUG-only E3 path distinct from D6 row-value smoke；
2. path must be selected through the same executor-driven bridge pattern:
   `PQTableScanIterator::Init()` creates a delegated `ParallelScanIterator`,
   and normal executor `Read()` calls delegate `Read()`；
3. `ParallelScanIterator::Init()` E3 branch must prove order-gather selection
   by invoking controlled `Exchange_sort` smoke or a dedicated order helper；
4. E3 must not return SQL rows from `Exchange_sort` yet；the safe coding path is:
   `PQTableScanIterator::Init()` creates the delegated `ParallelScanIterator`,
   `ParallelScanIterator::Init()` runs the controlled order-gather smoke and
   records E3-specific counters, then `Read()` returns EOF/no-row for that
   debug statement；
5. visible ordered rows are explicitly deferred to a later reviewed task with
   real row materialization semantics；
6. real ORDER BY SQL must remain optimizer-rejected with `HAS_ORDER_BY`；
7. failure after any debug commit point must fail closed, not silently fallback
   as ORDER BY PQ execution；
8. E3-specific counters are mandatory and must not reuse D6 row-value counters
   or M8/E2 exchange sort counters as the proof。

Required E3 counters:

- `Parallel_scan_iterator_order_gather_attempts`；
- `Parallel_scan_iterator_order_gather_selected`；
- `Parallel_scan_iterator_order_gather_smoke_rows` or
  `Parallel_scan_iterator_order_gather_validated`。

Required negative assertions:

- `pq_commercial_order_by` remains unchanged semantically:
  `Not parallel HAS_ORDER_BY`；
- real ORDER BY SQL has zero delta for `Parallel_queries_executed`,
  `Parallel_workers_launched`, and `Parallel_ranges_dispatched`；
- D6 `Parallel_scan_iterator_row_value_*` counters must not grow during an E3
  order-gather statement；
- E3 order-gather statement must show E3 attempts/selected delta = 1 and
  smoke-row/validated delta matching the controlled helper；
- E2 `Parallel_exchange_sort_smoke_*` counters may grow only as subordinate
  helper evidence and must never be the sole proof of E3；
- the E3 order-gather statement itself must assert zero delta for worker,
  range, handler/InnoDB, clone, and resolver counters；
- no worker-result, range, handler/InnoDB, clone, or resolver counters grow from
  real ORDER BY SQL。

Validation for future coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats <new_e3_test> \
  --parallel=1 --vardir=/tmp/pqv_m11e3_target --tmpdir=/tmp/pqt_m11e3_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e3_full --tmpdir=/tmp/pqt_m11e3_full
```

Agent Task Prompt:

```text
请先阅读 AGENTS.md，并遵守其中指向的 CLAUDE.md。

你的角色是 Design Agent。
主控 Agent 是 Codex。
当前任务是 M11-E3 ParallelScanIterator Order Gather Debug Path。

请只做设计，不改源码。

请阅读：
- Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md；
- Docs/pq_tasks/commercial-port-m11-parallel-scan-lifecycle.md；
- sql/parallel_query/pq_iterator.cc；
- sql/parallel_query/pq_iterators.cc；
- sql/parallel_query/exchange_sort.*；
- sql/parallel_query/sql_parallel.cc；
- 商用仓 `pq_iterators.cc` 中 `pq_make_filesort()` /
  `pq_init_record_gather()` / `ParallelScanIterator::Read()`。

输出：
1. E3 debug bridge 是否应该返回可见 SQL rows；当前设计要求不返回；
2. 是否需要 E3-specific counters；
3. 允许/禁止文件是否合理；
4. MTR 应如何证明 E3 path 而不是 D6/E2；
5. 是否可以进入 E3 编码。
```

Docs-Design Review:

- first review returned `REVISE`；
- required E3-specific counters to be mandatory；
- required E3 not to return visible ordered SQL rows；
- required MTR assertions for E3 attempts/selected, D6 row-value counters = 0,
  and worker/range/handler/InnoDB/clone/resolver counters = 0；
- required E2 exchange sort counters to be subordinate evidence only；
- first re-review returned `REVISE` because worker/range/handler/InnoDB/clone/
  resolver zero-delta assertions were only recorded in review notes, not in the
  required negative assertions；
- requested revisions were applied；
- final re-review returned `ACCEPT`。

Completion Report - M11-E3 Coding:

- changed files:
  - `sql/parallel_query/pq_iterator.cc`；
  - `sql/parallel_query/pq_iterators.h`；
  - `sql/parallel_query/pq_iterators.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_parallel_scan_iterator_order_gather_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_parallel_scan_iterator_order_gather_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added dedicated E3 status counters:
    `Parallel_scan_iterator_order_gather_attempts`,
    `Parallel_scan_iterator_order_gather_selected`,
    `Parallel_scan_iterator_order_gather_smoke_rows`；
  - added a DBUG-only factory bridge for
    `pq_parallel_scan_iterator_order_gather_smoke` before clone activation
    probe so E3 can assert clone/preflight zero-delta；
  - `PQTableScanIterator::Init()` creates a delegated
    `ParallelScanIterator` only under the E3 DBUG flag；
  - `ParallelScanIterator::Init()` runs controlled
    `Exchange_sort::run_cached_record_adapter_smoke()` and records E3 counters；
  - `ParallelScanIterator::Read()` returns EOF/no-row for the E3 validated
    state and does not mark `Parallel_queries_executed`。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_parallel_scan_iterator_order_gather_smoke pq_commercial_order_by pq_stats`
    4/4 including `shutdown_report`；
  - full `parallel_query` suite passed: 87/87。
- risk notes:
  - E3 remains debug-only and does not expose user-visible ORDER BY PQ；
  - real ORDER BY remains rejected by `HAS_ORDER_BY`；
  - E3 intentionally bypasses clone activation probe only under its dedicated
    DBUG flag, because the task requires clone/preflight zero-delta for this
    isolated order-gather smoke；
  - visible ordered rows and real MQ/worker order frames remain deferred to
    later reviewed tasks。

Code/Doc/Test Review - M11-E3:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed E3 stays behind `pq_parallel_scan_iterator_order_gather_smoke`；
- confirmed factory bypass of `pq_clone_activation_probe()` is limited to the
  dedicated DBUG flag and occurs after normal PQ eligibility, InnoDB,
  non-worker, and non-EXPLAIN guards；
- confirmed `ORDER_GATHER_VALIDATED` is consumed as EOF/no-row and does not
  mark `Parallel_queries_executed`；
- confirmed focused MTR proves E3 counters grow while D6/E2, executed/fallback,
  worker, range, probe, handler smoke, clone, and worker-result counters stay
  flat；
- residual risk is intentional: debug-only no-row bridge smoke, not visible
  ordered row materialization or real worker/MQ order-frame behavior。

## M11-E4: User-visible ORDER BY Gate Design

Status: design review accepted；waiting for commit。

Goal:

- evaluate whether the current branch can safely relax the user-visible
  `HAS_ORDER_BY` serial boundary；
- define the exact minimum gate conditions for any future visible ORDER BY PQ；
- decide whether coding should happen now or remain blocked。

Design conclusion:

- do not open user-visible ORDER BY PQ in M11-E4；
- keep `HAS_ORDER_BY` as the default serial boundary；
- E4 is a design and diagnostics checkpoint only；
- visible ordered rows must wait for a later task after real ORDER BY row
  materialization, worker frame protocol, and leader merge semantics are wired。

Commercial reference path:

- `ParallelScanIterator::pq_make_filesort()` restores or builds the leader
  `ORDER` / `Filesort` metadata；
- `ParallelScanIterator::pq_init_record_gather()` creates the record gather
  path and passes `Filesort` into the gather layer；
- worker outputs are record frames, not the current E2/E3 controlled smoke
  records；
- `Exchange_sort` reads worker MQ frames, performs K-way merge, and writes
  visible rows into `table->record[0]`；
- optimizer side saves/restores optimized group/order state and handles ordered
  index usage, reverse range, stable output, and order-by-subquery guards。

Current branch gap:

- current `Exchange_sort` has commercial-shaped fields and controlled adapter
  smoke only；
- `Exchange_sort::read_mq_message()` is still inert for real worker frames；
- `ParallelScanIterator::Read()` has a positive D6 `Exchange_nosort` row-value
  path and an E3 no-row order-gather smoke, but no visible ordered row path；
- current worker-result adapter does not yet feed real ORDER BY record frames
  with `Filesort` / `Sort_param` semantics；
- current optimizer still rejects real ORDER BY as `HAS_ORDER_BY`；
- current tests prove no-row shape and serial boundary, not visible ordered
  correctness。

Hard stop conditions:

- do not remove or weaken `HAS_ORDER_BY` rejection for normal SQL in E4；
- do not mark `Parallel_queries_executed` for ORDER BY without visible ordered
  row materialization；
- do not consume `Exchange_sort` real MQ frames until frame ownership,
  record image layout, rowid tie-break, ASC/DESC, NULL ordering, and EOF/error
  protocol are tested；
- do not modify optimizer `JOIN::optimize()` / AccessPath factory eligibility
  for ORDER BY in the same commit as `Exchange_sort` row materialization；
- do not mix M11-F ref/ICP worker path into ORDER BY gate work。

Minimum future user-visible gate, not for E4 coding:

- single query block, single base table；
- InnoDB only, no partition table；
- no GROUP BY, DISTINCT, window functions, HAVING, rollup, subquery in ORDER BY,
  BLOB/TEXT selected fields, generated/hidden/functional-index fields, or
  locking reads；
- projection limited to fixed-length integer fields already proven by row-image
  tests；
- ORDER BY limited to selected fixed-length integer fields with explicit
  ASC-only first; DESC requires a separate reviewed test；
- deterministic tie-break must be explicit in SQL or implemented through
  stable rowid/ref-length semantics before LIMIT is allowed；
- DOP initially 1 for leader-side correctness, then DOP 2/4 after real
  worker-frame merge tests；
- fallback must happen before any PQ commit point if the shape check fails。

Required tests before any future coding opens the gate:

- real SQL positive:
  - `SELECT id, k FROM t ORDER BY k ASC, id ASC` returns the same rows/order as
    serial；
  - duplicate keys verify deterministic tie-break；
  - empty table and one-row table verify EOF handling；
  - optional LIMIT only after deterministic full-order correctness is proven。
- negative SQL:
  - DESC before DESC support, nullable fields, non-integer fields, expressions,
    GROUP BY, DISTINCT, window, HAVING, locking read, partition table, BLOB/TEXT
    all remain serial with diagnostics；
  - `Parallel_queries_executed`, workers, ranges, handler/InnoDB, clone, and
    worker-result counters do not grow for rejected shapes。
- internal counters:
  - future ORDER BY path must use dedicated visible-path counters distinct from
    E2/E3 smoke counters；
  - E2/E3 smoke counters remain non-authoritative for visible SQL execution。

Proposed next split:

- M11-E4a: design review for the above gate and hard-stop decision；
- M11-E4b: optional diagnostics-only patch if review wants a clearer
  `HAS_ORDER_BY` subreason for ORDER BY blocked by missing real row path；
- M11-E5a: real `Exchange_sort` worker-frame materialization design；
- M11-E5b: DBUG-only visible-row `Exchange_sort` adapter path；
- M11-E5c: guarded user-visible ORDER BY path only after E5a/E5b review
  accepts result correctness and failure cleanup。

Validation for E4:

- design review only；
- no source build/test required unless E4b diagnostics-only coding is approved。

Design Review - M11-E4:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed keeping `HAS_ORDER_BY` serial boundary is reasonable because the
  current optimizer still rejects `query_block->is_ordered()` and the iterator
  factory depends on `join->pq_eligible`；
- confirmed current branch gaps are complete: Filesort/JOIN saved order,
  `MQ_record_gather`, worker record frames, `Exchange_sort` real read, and
  optimizer eligibility；
- confirmed hard stops protect default SQL behavior；
- confirmed future gate covers LIMIT, tie-break, DESC, NULL, partition,
  locking read, GROUP BY, DISTINCT, window, HAVING, BLOB/TEXT, and related
  negative tests；
- confirmed E4a/E4b/E5a/E5b/E5c split is safe to submit as design-only。

## M11-E5a: Real Exchange_sort Worker-frame Materialization Design

Status: design drafted；waiting for independent design review。

Goal:

- define how to migrate the commercial `Exchange_sort::read_mq_record()` /
  batch / heap path into the current typed-MQ branch；
- establish a safe worker-frame contract before any source code consumes real
  ORDER BY worker output；
- keep optimizer/user-visible ORDER BY eligibility unchanged。

Design conclusion:

- E5a is design-only；
- do not code real `Exchange_sort::read_mq_message()` or
  `ParallelScanIterator::Read()` ORDER BY consumption yet；
- first introduce a dedicated ORDER BY worker-frame adapter design, because the
  current branch's `Query_result_mq` sends PQWR field-value frames while the
  commercial `Exchange_sort` expects record frames suitable for
  `table->record[0]`, `Sort_param::make_sortkey()`, and rowid tie-break。

Commercial behavior to preserve:

- per-worker cached record groups (`MAX_RECORD_STORE`) allow batch refill from
  MQ and heap-based K-way merge；
- each cached record owns/deep-copies row bytes and, when stable output is
  required, rowid bytes；
- when `Filesort` metadata exists, each record can lazily cache its sort key；
- `read_mq_record()` returns one visible row by copying or converting the
  current min record into leader `table->record[0]`；
- heap comparison uses sort key first and rowid/ref comparison as stable
  tie-break when required；
- EOF is per-worker completion, not a single global FINISH。

Current branch constraints:

- `Exchange_nosort` consumes typed `MQMessageType::ROW` row images；
- `Query_result_mq` emits `PQ_worker_result_frame_header` / PQWR field-value
  frames, not record images with rowid/sort-key metadata；
- `Exchange_sort::read_mq_message()` is currently inert and returns `FINISH`；
- E1/E2/E3 only validate shape and controlled adapter smokes；
- worker threads and DOP range paths are already used by fullscan tests, so
  ORDER BY frame changes must not alter default worker-result behavior。

Proposed ORDER BY worker-frame contract:

- define a new internal ORDER BY frame type or wrapper distinct from existing
  PQWR row frames；
- minimum frame fields:
  - message type: ROW / FINISH / ERROR；
  - worker id or queue id is implicit from MQ queue；
  - record image length and fixed record image bytes；
  - optional rowid length and rowid bytes for stable output；
  - optional prebuilt sort key length and sort key bytes only after sort-key
    ownership is reviewed；initial implementation should prefer lazy leader
    `Sort_param::make_sortkey()` from record image；
  - flags for NULL ordering / DESC semantics only after explicit support；
- initial frame producer should be debug-only and leader-local or worker-smoke
  controlled, not the normal SQL worker path；
- existing PQWR frames must remain unchanged for M11-B worker-result tests。

Proposed `Exchange_sort` materialization shape:

- add typed read helpers that read one ORDER BY frame from a specific worker MQ；
- deep-copy frame data into `PQ_orderby_cached_record` / batch-owned storage；
- keep current synthetic/cached adapter helpers intact；
- introduce an explicit materialization status enum:
  `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `ERROR`；
- preserve per-worker completion and heap removal semantics；
- expose a debug-only helper that consumes controlled ORDER BY frames and
  writes visible rows to `table->record[0]` only inside an isolated smoke；
- do not call the helper from default `ParallelScanIterator::Read()` in E5a。

Files allowed for future coding after design review:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/exchange.h` / `.cc` only for reusable typed-frame helpers
  if needed；
- `sql/parallel_query/query_result_mq.h` / `.cc` only if a separate ORDER BY
  frame helper is added without changing existing PQWR behavior；
- `sql/parallel_query/sql_parallel.h` / `.cc` only for debug helper/counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- this taskbook and README。

Forbidden files until a later reviewed gate:

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/sql_executor.*`；
- `sql/join_optimizer/access_path.*`；
- `storage/innobase/**`；
- default `ParallelScanIterator::Read()` ORDER BY path；
- user-visible `HAS_ORDER_BY` eligibility changes；
- M11-F ref/ICP worker path。

Required future coding tests before any visible SQL gate:

- debug-only ORDER BY frame smoke:
  - three worker queues, duplicate keys, EOF per worker；
  - ASC order with explicit tie-break；
  - DESC stays fail-closed until a separate reviewed DESC path；
  - NULL-ordering stays fail-closed until nullable ORDER BY semantics are
    explicitly implemented and tested；
  - ERROR frame propagation；
  - WOULD_BLOCK does not spin forever and is bounded by existing wait policy；
  - D6 row-value and E3 no-row counters do not grow。
- materialization:
  - visible rows written into leader `table->record[0]` in smoke only；
  - row image length mismatch fails closed；
  - nullable or unsupported field shape fails closed。
- compatibility:
  - existing `pq_commercial_worker_result*`, `pq_parallel_scan_iterator_*`,
    `pq_commercial_order_by`, and full `parallel_query` suite remain passing；
  - `pq_commercial_order_by` still reports `Not parallel HAS_ORDER_BY` until a
    later reviewed E5c gate。

Hard stop conditions:

- if ORDER BY frames cannot be distinguished from PQWR frames, stop and do not
  overload PQWR；
- if leader `Sort_param::make_sortkey()` requires JOIN/TABLE state mutation not
  isolated to the smoke, stop and split a `Filesort` state contract task；
- if rowid/ref_length ownership cannot be proven for the leader table/handler,
  stop and keep stable tie-break unsupported；
- if any coding patch touches optimizer eligibility, reject it as out of E5a
  scope。

Proposed next split after E5a review:

- M11-E5b-0: ORDER BY frame contract helper, compile-only + unit smoke；
- M11-E5b-1: `Exchange_sort` controlled frame K-way merge smoke；
- M11-E5b-2: merge-path empty-worker FINISH and ERROR fail-closed smoke；
- M11-E5b-3: debug-only visible row materialization smoke into
  `table->record[0]`；
- M11-E5c: user-visible ORDER BY path design review after visible-row smoke
  passes。

Validation for E5a:

- design review only；
- no build/MTR required because no source or test code changes。

Design Review - M11-E5a:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed E5a is reasonable as design-only and preserves the `HAS_ORDER_BY`
  serial boundary；
- confirmed current PQWR frames are field-value frames and should not be
  overloaded for `Exchange_sort`；
- confirmed a dedicated ORDER BY frame contract is the correct direction for
  record materialization, rowid/ref tie-break, lazy `Sort_param::make_sortkey()`,
  and per-worker EOF semantics；
- confirmed allowed/forbidden file split protects optimizer, executor,
  AccessPath, InnoDB, default `ParallelScanIterator::Read()`, and user-visible
  eligibility；
- residual risk carried forward: before any visible ORDER BY gate, DESC and
  NULL-ordering tests must be explicit, even if initial E5b only supports ASC
  and fails closed for nullable/DESC shapes。

## M11-E5b-0: ORDER BY Frame Contract Helper

Status: completed and committed；next step is M11-E5b-1 controlled frame K-way
merge smoke。

Goal:

- introduce a dedicated ORDER BY frame contract that is distinct from existing
  PQWR worker-result frames；
- validate ROW / FINISH / ERROR frame decode through a controlled local MQ；
- keep real `Exchange_sort` materialization, optimizer eligibility, and
  user-visible ORDER BY PQ disabled。

Completion Report - M11-E5b-0 Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/parallel_query/sql_parallel.cc`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_commercial_order_by_frames.test`；
  - `mysql-test/suite/parallel_query/r/pq_commercial_order_by_frames.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_frame_header` with `PQ_ORDERBY_FRAME_MAGIC` / version and
    independent `PQ_orderby_frame_type`；
  - added `pq_validate_orderby_frame()` and `pq_decode_orderby_frame()`；
  - added controlled `Exchange_sort::run_orderby_frame_contract_smoke()` using
    a local MQ handle and validating two ROW frames, one FINISH, one ERROR, and
    rejection of a non-ORDER-BY magic；
  - wired the contract smoke into existing `Gather_operator::run_exchange_sort_smoke()`；
  - added dedicated counters:
    `Parallel_exchange_sort_frame_smoke_rows`,
    `Parallel_exchange_sort_frame_smoke_finishes`,
    `Parallel_exchange_sort_frame_smoke_errors`；
  - updated `pq_stats` expected status count from 98 to 101。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4
    including `shutdown_report`；
  - full `parallel_query` suite passed: 88/88；
  - after review hygiene fix, `git diff --check`, `mysqld` build, and targeted
    MTR passed again。
- scope notes:
  - no optimizer, executor, AccessPath, InnoDB, or default
    `ParallelScanIterator::Read()` changes；
  - existing PQWR frame format remains unchanged；
  - `pq_commercial_order_by` still keeps real ORDER BY SQL serial through
    `HAS_ORDER_BY`；
  - this is a frame contract smoke only, not visible ORDER BY row
    materialization。

Code/Doc/Test Review - M11-E5b-0:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed ORDER BY frame contract is distinct from existing PQWR and does not
  change `Query_result_mq` wire format or leader PQWR decode；
- confirmed validator/decode cover null inputs, minimum header length, magic,
  version, type, payload length, total length, and non-ROW zero-payload
  constraints；
- confirmed local MQ smoke does not touch optimizer, executor, InnoDB, default
  `ParallelScanIterator::Read()`, or user-visible ORDER BY；
- confirmed counters are wired through `PQ_global_stats`, reset, SHOW STATUS,
  and `pq_stats.result`；
- confirmed MTR coverage is adequate for E5b-0 and `pq_commercial_order_by`
  still verifies `HAS_ORDER_BY` serial boundary；
- applied review hygiene follow-up: `pq_validate_orderby_frame()` now clears
  output pointers before validating the frame。
