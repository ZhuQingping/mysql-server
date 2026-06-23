# M11-E ORDER BY / Exchange_sort Real Path Gate Taskbook

## 状态

Status: M11-E0/E1/E2/E3/E4/E5a/E5b-0/E5b-1/E5b-2/E5b-3/E5c/E5d/E5d-0
completed and committed；M11-E5d-S0/S1/S2/S3 completed and committed；
M11-E5d-1 fail-closed Filesort contract shape completed and committed；
M11-E5d-2 design completed and committed；M11-E5d-2a owned ORDER chain copy
smoke completed and committed；M11-E5d-2b optimized flag contract smoke
completed and committed；M11-E5d-2c restore-to-sidecar contract smoke
completed and committed；M11-E5d-2d clone-copy contract smoke completed and
committed；M11-E5d-3 design completed and committed；M11-E5d-3a restored
ORDER Filesort contract completed and committed；M11-E5d-3b Filesort
constructor risk design completed and committed；M11-E5d-3c debug-only
Filesort construction smoke completed and committed；M11-E5d-4 Sort_param /
Exchange_sort initialization boundary design completed and committed；
M11-E5d-4a debug-only Sort_param init smoke completed and committed；
M11-E5d-4b Exchange_sort real-state adapter boundary design completed and
committed；M11-E5d-4b-1 debug-only Exchange_sort scalar sort-state adapter
shape completed and committed；M11-E5d-4c optimizer-side scalar handoff design
completed and committed；M11-E5d-4c-1 debug-only optimizer-to-Exchange_sort
scalar handoff completed，Code/Doc/Test Review Agent accepted，full
`parallel_query` suite passed，committed as `4dc34748200`；M11-E5d-5 real
`Exchange_sort` init / MQ / Read boundary design completed and committed；
M11-E5d-5a `Exchange_sort` real-init state owner shape completed and
committed；M11-E5d-5b sort-key buffer / record-group allocation smoke completed
and committed；M11-E5d-5c controlled ORDER BY MQ-to-record-group loader
completed and committed；M11-E5d-5d debug-only ordered leader Read shadow path
completed and committed；M11-E5d-5e user-visible ORDER BY eligibility gate
design completed and committed；M11-E5d-5e-1 ORDER BY eligibility contract
helper completed and committed as `f54474bda65`；M11-E5d-5e-2 ORDER BY
execution preflight blocker completed and committed as `68b27d804ca`；real
ORDER BY execution, default worker MQ consumption, and default ordered `Read()`
remain disabled；M11-E5d-5f runtime prerequisite diagnostics coding and
validation completed，Code/Doc/Test Review Agent accepted，committed as
`b94ca90cb75`；M11-E5g-0/1/2 completed and committed；M11-E5g-3 ordered
leader materialization smoke from streaming reader completed locally with build,
targeted MTR, full `parallel_query` suite, and Code/Doc/Test Review passed；
M11-E5g-4a/4b/4c/4d/4e completed and committed；M11-E5g-4f minimal
ASC-only visible ORDER BY gate design completed locally，Design Explorer and
Code/Doc/Test Review Agent accepted。M11-E5r closure 已完成，Post-E5r
handoff accepted；当前权威状态是 real execution blocked，source work
stopped。后续真实 ORDER BY path 必须从新的 reviewed phase 启动；M11-E6
已启动为 docs-only phase selection，不继承 E5r source work。

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

Commit:

- `334c4ae48d0` Add PQ M11E saved state restore smoke。

### M11-E5d-S3: Clone-copy Contract Design

Status: design-only taskbook completed；Review Agent accepted；no source code
change。

Goal:

- prove the S1/S2 saved ORDER/GROUP sidecar can be copied as value-only
  diagnostic state for a future PQ clone without owning optimizer ORDER nodes,
  `Item` objects, MEM_ROOT allocations, or `Query_block` private saved list
  pointers；
- keep this as a sidecar copy contract, not a commercial `JOIN::pq_copy_from()`
  activation；
- preserve `HAS_ORDER_BY` user-visible serial boundary and avoid Filesort
  construction。

Commercial reference:

- commercial clone copies saved optimized ORDER/GROUP state as part of a larger
  JOIN clone path；
- current branch `JOIN::pq_copy_from()` is still an inert stub in
  `sql/parallel_query/pq_clone.cc`；
- current branch does not yet have owned commercial saved ORDER/GROUP helper
  fields, so S3 must not wire into the real clone lifecycle。

Design boundary:

- S3 may add a small value-copy helper for `PQSavedOrderGroupContract` or a
  dedicated clone-copy smoke helper；
- S3 may copy only scalar fields and status/detail string pointers to static
  strings；
- S3 must not copy, duplicate, or own `ORDER *`, `Item *`, `TABLE *`,
  `AccessPath *`, `QEP_TAB *`, MEM_ROOT memory, or `Query_block` private
  saved list pointers；
- S3 must not modify `JOIN::pq_copy_from()` or `Query_block::pq_restore()`；
- if source contract is unsupported, the copied contract must remain
  unsupported and must not become `READY`。

Allowed files for coding after review:

- `sql/parallel_query/pq_optimizer.h`；
- `sql/parallel_query/pq_optimizer.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/mysqld.cc`；
- focused MTR under `mysql-test/suite/parallel_query/`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden files / actions:

- no `sql/sql_optimizer.*` changes；
- no `sql/parallel_query/pq_clone.cc` changes in S3；
- no direct `JOIN` saved-state fields；
- no real clone activation, worker JOIN construction, resolver rewrite, or
  `Ref_item_array` ownership changes；
- no Filesort / Sort_param construction or `make_sortorder()`；
- no `HAS_ORDER_BY` relaxation, `Read()` change, worker, handler, MQ, or real
  ORDER BY producer wiring。

Proposed coding shape:

- add value-copy helper, for example
  `pq_copy_saved_order_group_contract(const PQSavedOrderGroupContract &src,
  PQSavedOrderGroupContract *dst)`；
- add debug-only smoke under a DBUG flag such as
  `pq_saved_order_group_clone_copy_smoke`；
- smoke builds an S1 contract from the current leader ORDER BY rejected shape；
- smoke copies the sidecar to a local destination object；
- smoke verifies status, detail pointer, and scalar fields match；
- smoke verifies unsupported source remains unsupported after copy；
- smoke does not call `JOIN::pq_copy_from()`。

Required observability:

- new status counters:
  `Parallel_saved_order_group_clone_copy_smoke_attempts`；
  `Parallel_saved_order_group_clone_copy_smoke_success`；
  `Parallel_saved_order_group_clone_copy_smoke_unsupported`。
- MTR:
  - no-DBUG negative window verifies clone-copy counters do not grow by
    default；
  - DBUG positive window verifies attempts/success grow；
  - S1/S2 unsupported/restore semantics remain unchanged；
  - `Parallel_queries_executed`, `Parallel_workers_launched`, and
    `Parallel_ranges_dispatched` stay zero；
  - ORDER BY EXPLAIN still reports `Not parallel HAS_ORDER_BY`。

Required validation for coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5ds3_target --tmpdir=/tmp/pqt_m11e5ds3_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5ds3_full --tmpdir=/tmp/pqt_m11e5ds3_full
```

Acceptance checklist:

- no source edits before design review；
- sidecar copy is value-only；
- unsupported state remains unsupported；
- no real clone lifecycle hook is touched；
- user-visible ORDER BY remains serial；
- next step after S3 may be E5d-1 fail-closed Filesort contract shape, not
  Filesort construction smoke yet。

Review request:

- confirm S3 should avoid `pq_clone.cc` and real `JOIN::pq_copy_from()` wiring；
- confirm value-only sidecar copy is enough before E5d-1；
- confirm counters/MTR prove no user-visible activation；
- confirm whether E5d-1 after S3 should still be fail-closed Filesort contract
  shape rather than `Filesort::make_sortorder()` smoke。

Design Review - M11-E5d-S3:

- Review Agent verdict: `ACCEPT`；
- confirmed S3 should avoid `pq_clone.cc` and real `JOIN::pq_copy_from()`；
- confirmed value-only sidecar copy is enough before E5d-1；
- confirmed forbidden ownership boundaries cover ORDER/Item/TABLE/AccessPath/
  QEP_TAB/MEM_ROOT/private saved list pointers and resolver/ref-item ownership；
- confirmed counters/MTR design proves no default trigger, no user-visible
  ORDER BY PQ, and unsupported remains unsupported；
- confirmed next step after S3 should be E5d-1 fail-closed Filesort contract
  shape, not `Filesort::make_sortorder()` smoke。

Completion Report - M11-E5d-S3 Coding:

- changed files:
  - `sql/parallel_query/pq_optimizer.h`；
  - `sql/parallel_query/pq_optimizer.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_saved_order_group_contract.test`；
  - `mysql-test/suite/parallel_query/r/pq_saved_order_group_contract.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `pq_copy_saved_order_group_contract()` value-only helper；
  - helper copies the sidecar struct and returns success only when copied state
    is not `READY`；
  - added debug-only `pq_saved_order_group_clone_copy_smoke` hook on the
    existing ORDER BY `HAS_ORDER_BY` rejection path；
  - clone-copy smoke builds a leader sidecar, copies it to a local destination,
    compares every scalar/status/detail field, and counts success only when the
    copied sidecar remains `UNSUPPORTED_MISSING_SAVED_HELPERS`；
  - added counters:
    `Parallel_saved_order_group_clone_copy_smoke_attempts`,
    `Parallel_saved_order_group_clone_copy_smoke_success`, and
    `Parallel_saved_order_group_clone_copy_smoke_unsupported`。
- hard boundaries:
  - no `sql/parallel_query/pq_clone.cc` changes；
  - no real `JOIN::pq_copy_from()` wiring；
  - no `sql/sql_optimizer.*` changes；
  - no direct `JOIN` saved-state fields；
  - no ORDER/GROUP list, `Item *`, `ORDER *`, QEP_TAB, AccessPath, TABLE,
    handler, MEM_ROOT, or private saved-list ownership changes；
  - no Filesort / Sort_param construction, `make_sortorder()`,
    `HAS_ORDER_BY` relaxation, `Read()` change, worker, handler, MQ, or real
    ORDER BY producer wiring。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4。

Review request:

- confirm value-only sidecar copy does not imply clone readiness；
- confirm copied unsupported state remains unsupported and never `READY`；
- confirm no real clone lifecycle is touched；
- confirm MTR covers no-DBUG negative, DBUG positive, zero
  executed/workers/ranges deltas, and `HAS_ORDER_BY` serial boundary；
- confirm next step is E5d-1 fail-closed Filesort contract shape。

Code/Doc/Test Review - M11-E5d-S3:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `pq_copy_saved_order_group_contract()` is value-only sidecar copy；
- confirmed `PQSavedOrderGroupContract` contains no ORDER/Item/TABLE/
  AccessPath/QEP_TAB/MEM_ROOT/private saved-list ownership；
- confirmed copied unsupported state remains unsupported and does not become
  `READY`；
- confirmed no `pq_clone.cc`, real `JOIN::pq_copy_from()`,
  `sql/sql_optimizer.*`, Filesort / Sort_param, `make_sortorder()`, `Read()`,
  worker, handler, or MQ changes；
- confirmed MTR covers no-DBUG negative, DBUG positive, zero
  executed/workers/ranges, `HAS_ORDER_BY` serial boundary, and `pq_stats`
  count/list；
- next step: E5d-1 fail-closed Filesort contract shape, not
  `make_sortorder()` smoke。

Commit:

- `c49fc064cca` Add PQ M11E saved state clone copy smoke。

### M11-E5d-1: Fail-closed Filesort Contract Shape Design

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；waiting for commit。

Goal:

- introduce a leader-only contract shape that represents whether commercial
  ORDER BY Filesort state is usable；
- report fail-closed unsupported while the branch lacks owned saved
  ORDER/GROUP helper state；
- prepare a narrow entry point for later Filesort lifecycle review without
  constructing `Filesort` or `Sort_param`。

Commercial reference:

- commercial `ParallelScanIterator::pq_make_filesort()` restores saved
  ORDER/GROUP state and then prepares leader Filesort metadata；
- commercial `Exchange_sort::init()` calls `Filesort::make_sortorder()` and
  initializes `Sort_param`；
- current branch has S1/S2/S3 sidecar proofs only. It still lacks owned
  commercial saved ORDER/GROUP helper state, so E5d-1 must not invoke the
  commercial Filesort code path。

Design boundary:

- E5d-1 may define a small contract struct such as
  `PQOrderByFilesortContract` with fields for status, detail, has order,
  stable sort request, ordered-index usage, and whether saved helper state is
  ready；
- E5d-1 may build this contract from `PQSavedOrderGroupContract` and public
  leader JOIN / Query_block scalar state；
- E5d-1 must return unsupported unless the saved ORDER/GROUP sidecar is
  `READY`；
- because S1-S3 deliberately keep the sidecar unsupported, E5d-1 must remain
  fail-closed in all current MTRs；
- E5d-1 must not allocate or reference a real `Filesort` object。

Allowed files for coding after review:

- `sql/parallel_query/pq_optimizer.h`；
- `sql/parallel_query/pq_optimizer.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/mysqld.cc`；
- focused MTR under `mysql-test/suite/parallel_query/`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden files / actions:

- no `sql/filesort.*` changes；
- no `sql/iterators/sorting_iterator.*` changes；
- no `sql/sql_optimizer.*` changes；
- no `sql/parallel_query/exchange_sort.*` real Filesort integration；
- no `Filesort`, `Sort_param`, or `Filesort::make_sortorder()` construction or
  invocation；
- no `pq_clone.cc` or real clone lifecycle wiring；
- no `HAS_ORDER_BY` relaxation, `Read()` change, worker, handler, MQ, or real
  ORDER BY producer wiring。

Proposed coding shape:

- add enum / struct for a fail-closed Filesort contract:
  - `UNSUPPORTED_NULL_INPUT`；
  - `UNSUPPORTED_MISSING_SAVED_HELPERS`；
  - reserved `READY` for later phases only。
- add helper such as
  `pq_build_orderby_filesort_contract(Query_block *, JOIN *,
  PQOrderByFilesortContract *)`；
- helper first builds `PQSavedOrderGroupContract`；
- if saved sidecar is not `READY`, set
  `UNSUPPORTED_MISSING_SAVED_HELPERS` and return false；
- add debug-only smoke under
  `pq_orderby_filesort_contract_smoke` on existing ORDER BY reject path；
- smoke verifies attempts/unsupported grow and no ready/success is reported。

Required observability:

- new status counters:
  `Parallel_orderby_filesort_contract_attempts`；
  `Parallel_orderby_filesort_contract_unsupported`。
- MTR:
  - no-DBUG negative window verifies Filesort contract counters do not grow by
    default；
  - DBUG positive window verifies attempts/unsupported grow；
  - saved ORDER/GROUP S1-S3 counters remain compatible；
  - `Parallel_queries_executed`, `Parallel_workers_launched`, and
    `Parallel_ranges_dispatched` stay zero；
  - ORDER BY EXPLAIN still reports `Not parallel HAS_ORDER_BY`。

Required validation for coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d1_target --tmpdir=/tmp/pqt_m11e5d1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d1_full --tmpdir=/tmp/pqt_m11e5d1_full
```

Acceptance checklist:

- no source edits before design review；
- no real Filesort or Sort_param object exists；
- helper is fail-closed because saved helper state is unsupported；
- user-visible ORDER BY remains serial；
- next step after E5d-1 is to decide whether to implement owned saved
  ORDER/GROUP helper state or keep Filesort construction blocked。

Review request:

- confirm E5d-1 should be a fail-closed contract shape only；
- confirm `Filesort::make_sortorder()` smoke remains forbidden；
- confirm using `PQSavedOrderGroupContract` as prerequisite is correct；
- confirm counters/MTR prove no user-visible ORDER BY activation。

Design Review - M11-E5d-1:

- Review Agent verdict: `ACCEPT`；
- confirmed E5d-1 must be fail-closed contract shape only, not
  `Filesort::make_sortorder()` smoke；
- confirmed `PQSavedOrderGroupContract::READY` is the correct prerequisite,
  and current S1-S3 unsupported state means E5d-1 must remain unsupported in
  current MTRs；
- confirmed forbidden file/action list is sufficient to prevent real Filesort
  lifecycle or user-visible ORDER BY activation；
- required coding assertion: no-DBUG Filesort contract counters do not grow；
  DBUG attempts grow and unsupported equals attempts；
  executed/workers/ranges stay zero；ORDER BY EXPLAIN remains
  `Not parallel HAS_ORDER_BY`；
- confirmed next step after E5d-1 should be owned saved ORDER/GROUP helper
  state design/implementation, or explicitly blocked, not Filesort
  construction。

Completion Report - M11-E5d-1 Coding:

- changed files:
  - `sql/parallel_query/pq_optimizer.h`；
  - `sql/parallel_query/pq_optimizer.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_saved_order_group_contract.test`；
  - `mysql-test/suite/parallel_query/r/pq_saved_order_group_contract.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQOrderByFilesortContractStatus` and
    `PQOrderByFilesortContract` as a leader-only fail-closed Filesort readiness
    contract；
  - added `pq_build_orderby_filesort_contract()`；it builds
    `PQSavedOrderGroupContract` first and returns
    `UNSUPPORTED_MISSING_SAVED_HELPERS` unless the saved sidecar is `READY`；
  - added debug-only `pq_orderby_filesort_contract_smoke` on the existing
    ORDER BY reject path；
  - added status counters:
    `Parallel_orderby_filesort_contract_attempts` and
    `Parallel_orderby_filesort_contract_unsupported`；
  - extended `pq_saved_order_group_contract` to verify no-DBUG counters remain
    unchanged, DBUG attempts grow, unsupported equals attempts, and
    executed/workers/ranges stay zero；
  - updated `pq_stats` expected Parallel status variable count from 116 to
    118。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
    including `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- scope notes:
  - no `Filesort`, `Sort_param`, or `Filesort::make_sortorder()` construction
    or invocation；
  - no `sql/filesort.*`, `sql/iterators/sorting_iterator.*`,
    `sql/sql_optimizer.*`, or `sql/parallel_query/exchange_sort.*` real
    Filesort integration changes；
  - no clone lifecycle, worker, handler, MQ, `Read()`, or `HAS_ORDER_BY`
    relaxation；
  - current MTRs intentionally keep the contract unsupported because S1-S3
    saved ORDER/GROUP sidecar remains unsupported。

Code/Doc/Test Review - M11-E5d-1:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed fail-closed boundary holds: helper depends on
  `PQSavedOrderGroupContract::READY` and current saved ORDER/GROUP sidecar
  remains unsupported；
- confirmed DBUG smoke is only called in the existing `HAS_ORDER_BY` reject
  path and does not relax user-visible ORDER BY eligibility；
- confirmed no `Filesort`, `Sort_param`, `Filesort::make_sortorder()`,
  `sql/filesort.*`, sorting iterator, optimizer, `exchange_sort.*`, clone,
  worker, handler, MQ, or `Read()` real path change exists；
- confirmed stats fields, reset, SHOW STATUS entries, and `pq_stats` result
  are complete；
- confirmed MTR covers no-DBUG negative, DBUG attempts/unsupported equality,
  `Not parallel HAS_ORDER_BY`, and zero executed/workers/ranges；
- remaining risk: `READY` path is intentionally untested until owned saved
  ORDER/GROUP helper state is implemented。

Commit:

- `99526a6067d` Add PQ M11E filesort contract shape。

### M11-E5d-2: Owned Saved ORDER/GROUP Helper State Design

Status: design-only taskbook completed；Design Review Agent accepted；
committed。

Goal:

- define the smallest owned PQ sidecar that can preserve and restore the
  `ORDER*` chain required by commercial Filesort path；
- replace the current scalar-only S1/S2/S3 sidecar limitation with a reviewed
  ownership model；
- keep real `Filesort`, `Sort_param`, `Filesort::make_sortorder()`, and
  user-visible ORDER BY PQ disabled until this helper state is proven。

Commercial reference summary:

- commercial `ParallelScanIterator::pq_make_filesort()` restores GROUP/ORDER
  through `saved_join_group_list`, `saved_join_order`,
  `optimized_group_flags`, and `optimized_order_flags` before constructing
  `Filesort`；
- commercial `Exchange_sort::init()` assumes `m_sort->m_order` is already a
  correct restored or synthesized ORDER chain, then calls
  `Filesort::make_sortorder()`；
- commercial `Filesort::make_sortorder()` consumes the `ORDER*` chain,
  especially `ord->item[0]` and `ord->direction`；
- commercial helper state also preserves JOIN scalar execution semantics such
  as `grouped`, `group_optimized_away`, `implicit_grouping`,
  `need_tmp_before_win`, `simple_group`, `simple_order`,
  `streaming_aggregation`, `skip_sort_order`, `m_ordered_index_usage`,
  `select_distinct`, and `having_cond`；
- commercial helpers cannot be directly copied because they depend on fields
  and lifecycle that are not present in this branch:
  `saved_join_order`, `saved_join_group_list`, PQ resolver saved list pointers,
  `saved_optimized_vars`, PQ clone ownership, and commercial `pq_mem_root`
  conventions。

Current branch facts:

- upstream `Query_block::order_list_ptrs` / `group_list_ptrs` preserve
  `ORDER::next` chains for prepared statement repeated optimization only；
- `Query_block::save_order_properties()` is private and saves existing
  pointers, not owned ORDER nodes；
- `Query_block::restore_cmd_properties()` asserts `join == nullptr` and is a
  pre-reoptimization restore entry, not a PQ runtime helper；
- `JOIN::optimize_distinct_group_order()` can mutate `join->order` and
  `join->group_list` through `remove_const()`, `group_list.clean()`, DISTINCT
  to GROUP conversion, and ORDER cleanup；
- current `PQSavedOrderGroupContract` captures only scalar diagnostics and
  deliberately returns `UNSUPPORTED_MISSING_SAVED_HELPERS`。

Design decision:

- do not expose or call upstream `Query_block::save_order_properties()` from PQ；
- do not call `restore_cmd_properties()` inside or after `JOIN::optimize()`；
- do not deep-copy `Item` trees；
- introduce a PQ-owned sidecar that owns copied `ORDER` nodes and `next` links,
  while aliasing already resolved `Item` pointers；
- keep `ORDER_with_src` metadata (`src`, `const_optimized`) and scalar JOIN
  semantics in the sidecar；
- make helper `READY` only after both structure copy and invariant checks pass；
- keep `PQOrderByFilesortContract` fail-closed until this sidecar can provide
  a restored `ORDER*` chain without mutating live optimizer state。

Proposed coding split after design review:

1. M11-E5d-2a Owned ORDER Chain Copy Smoke:
   - allowed files: `sql/parallel_query/pq_optimizer.h`,
     `sql/parallel_query/pq_optimizer.cc`, `sql/parallel_query/sql_parallel.h`,
     `sql/mysqld.cc`, focused MTR, task docs；
   - add a small sidecar type that copies `ORDER` struct nodes into owned
     storage and rewires `next` within the copied chain；
   - copied `ORDER::item` pointers alias source items and are never owned；
   - add DBUG smoke that copies `join->order` for an ORDER BY reject query and
     verifies length, direction, item pointer, source metadata, and copied chain
     independence；
   - sidecar may still report `UNSUPPORTED_MISSING_OPTIMIZED_FLAGS` for real
     Filesort readiness。
2. M11-E5d-2b GROUP Chain and Optimized Flag Contract:
   - record source-vs-optimized membership flags for ORDER/GROUP chains；
   - cover `remove_const()` / cleaned-list semantics through controlled smoke；
   - do not call real `calc_group_buffer()` or mutate live `JOIN::group_list`。
3. M11-E5d-2c Restore-to-Sidecar Contract:
   - reconstruct a restored sidecar `ORDER_with_src` from owned nodes and flags；
   - return a pointer to the sidecar chain for later Filesort lifecycle review；
   - no real `Filesort` object yet。
4. M11-E5d-2d Clone Copy Contract:
   - copy the owned sidecar between leader/clone diagnostic objects；
   - prove copied nodes do not alias node storage while still aliasing expected
     `Item` pointers。

Forbidden files / actions for E5d-2:

- no `sql/filesort.*`；
- no `sql/iterators/sorting_iterator.*`；
- no `sql/sql_optimizer.*` mutation hooks until a separate reviewed phase；
- no `Query_block::restore_cmd_properties()` semantic changes；
- no public exposure of private `save_order_properties()`；
- no real `Filesort`, `Sort_param`, or `Filesort::make_sortorder()`；
- no `sql/parallel_query/exchange_sort.*` real Filesort integration；
- no clone lifecycle, worker, handler, MQ, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation；
- no deep copy or ownership of `Item` trees。

Required validation for first coding subtask:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d2_target --tmpdir=/tmp/pqt_m11e5d2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d2_full --tmpdir=/tmp/pqt_m11e5d2_full
```

Acceptance checklist:

- Design Review Agent accepts the ownership model before coding；
- first coding patch proves owned copied ORDER node chain independence；
- no user-visible ORDER BY PQ activation；
- no real Filesort construction；
- `PQOrderByFilesortContract` remains unsupported until restored sidecar chain
  and optimized flags are both available；
- after each coding subtask, run Code/Doc/Test Review Agent before commit。

Design review request:

- confirm owned ORDER node sidecar with aliased `Item` pointers is the right
  minimal replacement for commercial saved list pointers；
- confirm upstream `restore_cmd_properties()` must not be used in PQ runtime；
- confirm E5d-2a should start with ORDER chain copy smoke only；
- confirm optimized flags, GROUP chain, restore-to-sidecar, and clone copy
  should remain separate follow-up subtasks；
- confirm `Filesort::make_sortorder()` remains forbidden until E5d-2 is
  complete。

Design Review - M11-E5d-2:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed upstream `Query_block::restore_cmd_properties()` and private
  `save_order_properties()` are the wrong lifecycle for PQ runtime helper use；
- confirmed owned copied `ORDER` nodes with aliased resolved `Item*` pointers
  are the smallest safe replacement direction for commercial saved list
  pointers in the current branch；
- confirmed E5d-2a should start with ORDER chain copy smoke only, while
  GROUP/optimized flags, restore-to-sidecar, and clone copy remain separate
  follow-ups；
- confirmed forbidden scope is strong enough to prevent premature Filesort,
  optimizer hook, worker/MQ/Read, AccessPath, and `HAS_ORDER_BY` activation；
- confirmed validation requirements are sufficient for the first coding
  subtask；
- remaining risks:
  - `Item*` aliasing is safe only while the sidecar stays within leader/clone
    diagnostic lifetime and does not outlive resolved query objects；
  - E5d-2c must prove restored sidecar chains cannot mutate live optimizer
    `ORDER_with_src` state；
  - real `Filesort`, `Sort_param`, `Exchange_sort`, and user-visible ORDER BY
    PQ still need separate review before activation。

Commit:

- `61e074704e1` Plan PQ M11E owned order group state。

### M11-E5d-2a: Owned ORDER Chain Copy Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；waiting for commit。

Goal:

- prove a PQ-owned sidecar can copy the optimized `JOIN::order` chain without
  owning or copying `Item` trees；
- prove copied `ORDER` nodes and `next` links are owned by the sidecar, while
  resolved `Item*` aliases stay identical to the source chain；
- keep real saved GROUP restoration, optimized flags, clone lifecycle,
  Filesort, and user-visible ORDER BY PQ disabled。

Implementation:

- added an internal `PQ_owned_order_chain_sidecar` in
  `sql/parallel_query/pq_optimizer.cc`；
- added internal helper `pq_copy_order_chain()`:
  - counts source `ORDER` nodes；
  - reserves vector storage before copy；
  - copies `ORDER` structs by value；
  - rewires copied `next` pointers to copied nodes；
  - preserves `ORDER_with_src::src` and `is_const_optimized()` metadata；
  - aliases `ORDER::item`, `item_initial`, `rollup_item`,
    `field_in_tmp_table`, and other non-owned pointers exactly as source；
- added `pq_saved_order_chain_copy_smoke` on the existing ORDER BY reject path；
- added status counters:
  `Parallel_saved_order_chain_copy_smoke_attempts`,
  `Parallel_saved_order_chain_copy_smoke_success`,
  `Parallel_saved_order_chain_copy_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify:
  - no-DBUG counters remain zero；
  - DBUG attempts/success grow；
  - unsupported stays zero for the controlled ORDER BY shape；
  - `Parallel_queries_executed`, `Parallel_workers_launched`, and
    `Parallel_ranges_dispatched` remain zero；
  - ORDER BY still reports `Not parallel HAS_ORDER_BY`；
- updated `pq_stats` Parallel status variable count from 118 to 121。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no public PQ optimizer API change yet；
- no `sql/filesort.*`, `sql/iterators/sorting_iterator.*`,
  `sql/sql_optimizer.*`, `exchange_sort.*`, AccessPath, clone lifecycle,
  worker, handler, MQ, or `Read()` changes；
- no real `Filesort`, `Sort_param`, or `Filesort::make_sortorder()`；
- no `HAS_ORDER_BY` eligibility relaxation；
- `PQSavedOrderGroupContract` and `PQOrderByFilesortContract` remain
  fail-closed for real Filesort readiness。

Code/Doc/Test Review - M11-E5d-2a:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed sidecar owns only copied `ORDER` nodes and copied `next` links；
- confirmed copied non-owned pointers, including resolved `Item*` and field
  aliases, are preserved but not owned or modified；
- confirmed vector storage is reserved before copy and copied `next` pointers
  are rewired to copied storage；
- confirmed smoke is gated by `pq_saved_order_chain_copy_smoke` and the
  existing ORDER BY reject path；
- confirmed no real Filesort, Sort_param, sorting iterator, optimizer mutation
  hook, exchange sort integration, clone lifecycle, worker/handler/MQ/Read, or
  `HAS_ORDER_BY` activation；
- confirmed stats and MTR coverage are complete；
- remaining risks:
  - 1024-node chain cap remains fail-closed and debug-only；
  - future E5d-2b/2c/2d must still prove optimized flags, restored sidecar
    chain isolation, and clone-copy lifetime before any Filesort path opens。

Commit:

- `bc55cff554f` Add PQ M11E owned order chain smoke。

### M11-E5d-2b: Optimized ORDER Flag Contract Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；committed。

Goal:

- add source-vs-optimized membership flags to the owned ORDER sidecar；
- cover cleaned-list semantics with a controlled debug-only optimized chain；
- continue avoiding live `JOIN::order` mutation, real GROUP restoration,
  optimizer hooks, clone lifecycle, Filesort, and user-visible ORDER BY PQ。

Implementation:

- extended internal `PQ_owned_order_chain_sidecar` with:
  - `source_nodes`: original source `ORDER*` addresses for mapping only；
  - `optimized_flags`: per-source-node membership bits；
- added `pq_record_order_chain_optimized_flags()` to record whether each
  original source node appears in a supplied optimized chain；
- added `pq_saved_order_chain_flags_smoke` on the existing ORDER BY reject
  path；
- smoke builds a controlled optimized view from `join->order.order->next` to
  simulate the first source ORDER node being removed by `remove_const()` or
  cleanup；
- smoke verifies copied chain integrity plus expected flags:
  first source node false, second source node true；
- added counters:
  `Parallel_saved_order_chain_flags_smoke_attempts`,
  `Parallel_saved_order_chain_flags_smoke_success`,
  `Parallel_saved_order_chain_flags_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify no-DBUG zero counters,
  DBUG attempts/success, unsupported zero, serial ORDER BY boundary, and zero
  executed/workers/ranges；
- updated `pq_stats` Parallel status variable count from 121 to 124。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no `sql/sql_optimizer.*` hook or live optimizer list mutation；
- no GROUP chain restore, no `calc_group_buffer()`；
- no real `Filesort`, `Sort_param`, or `Filesort::make_sortorder()`；
- no `exchange_sort.*`, clone lifecycle, worker, handler, MQ, AccessPath,
  `Read()`, or `HAS_ORDER_BY` eligibility relaxation；
- this proves membership flags only. Restored sidecar chain construction
  remains E5d-2c。

Code/Doc/Test Review - M11-E5d-2b:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `source_nodes` and `optimized_flags` only record mapping data and
  do not own or modify source `ORDER`, `Item`, `Field`, or `JOIN::order`；
- confirmed optimized flag recording is membership-only；
- confirmed controlled optimized chain uses `join->order.order->next` without
  relinking the live chain；
- confirmed smoke is DBUG-gated and called only from the existing ORDER BY
  reject path；
- confirmed no forbidden files/actions are touched；
- confirmed stats and MTR coverage are complete；
- remaining risk: E5d-2b proves pointer-membership flags only. Restored
  sidecar chain construction remains E5d-2c。

Commit:

- `6e815347ce5` Add PQ M11E order chain flag smoke。

### M11-E5d-2c: Restore-to-Sidecar Contract Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；committed。

Goal:

- reconstruct a restored sidecar `ORDER` chain from the copied owned nodes and
  recorded optimized membership flags；
- prove restored nodes are sidecar-owned and do not alias the live optimizer
  `ORDER` nodes；
- prove restored chain semantics match a controlled optimized chain before any
  real `Filesort`, `Sort_param`, or `Filesort::make_sortorder()` integration。

Implementation:

- extended internal `PQ_owned_order_chain_sidecar` with `restored_nodes`；
- added `restored_head()` accessors for the restored sidecar chain；
- added `pq_restore_order_chain_from_flags()`:
  - validates owned node count and optimized flag count match；
  - copies only optimized source nodes into `restored_nodes`；
  - rewires restored `next` pointers inside sidecar-owned storage；
  - fails closed when no optimized nodes remain；
- added `pq_restored_order_chain_matches_optimized()` to compare restored
  sidecar nodes against a supplied optimized `ORDER_with_src` while confirming
  restored nodes do not alias expected/live nodes；
- added DBUG smoke `pq_saved_order_chain_restore_smoke` on the existing
  `HAS_ORDER_BY` reject path；
- smoke uses the same controlled optimized view as E5d-2b
  (`join->order.order->next`) to simulate first-node cleanup without mutating
  live `JOIN::order`；
- added status counters:
  `Parallel_saved_order_chain_restore_smoke_attempts`,
  `Parallel_saved_order_chain_restore_smoke_success`,
  `Parallel_saved_order_chain_restore_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify no-DBUG zero counters,
  DBUG attempts/success, unsupported zero for the controlled shape, serial
  ORDER BY boundary, and zero executed/workers/ranges；
- updated `pq_stats` Parallel status variable count from 124 to 127。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no `sql/sql_optimizer.*` hook or live optimizer list mutation；
- no `Query_block::restore_cmd_properties()` semantic change；
- no real `Filesort`, `Sort_param`, `Filesort::make_sortorder()`, sorting
  iterator, or `exchange_sort.*` integration；
- no clone lifecycle, worker, handler, MQ, AccessPath, `Read()`, or
  `HAS_ORDER_BY` eligibility relaxation；
- restored sidecar chain remains an internal contract smoke and is not yet
  exposed as a public PQ optimizer API。

Code/Doc/Test Review - M11-E5d-2c:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- validation gaps: none；
- confirmed `pq_restore_order_chain_from_flags()` only builds
  sidecar-owned `restored_nodes` and rewires restored `next` pointers inside
  that vector；
- confirmed restore does not mutate live `JOIN::order`, `ORDER_with_src`,
  `Item`, or `Field` state；
- confirmed restore helper rejects count mismatch and empty restored chains,
  reserves before push, and rewires after all copies；
- confirmed match helper verifies restored nodes do not alias live optimized
  nodes while preserving expected ORDER semantics；
- confirmed DBUG smoke remains inside the existing `HAS_ORDER_BY` reject path
  and does not open user-visible ORDER BY PQ；
- confirmed stats fields, reset, SHOW STATUS, `pq_stats`, MTR assertions, and
  docs are consistent；
- confirmed forbidden scope is not touched。

Commit:

- `424216c1753` Add PQ M11E order chain restore smoke。

### M11-E5d-2d: Clone-copy Contract Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；committed。

Goal:

- prove the owned ORDER sidecar can be copied from a leader diagnostic object
  to a clone diagnostic object without sharing copied ORDER node storage；
- prove copied `ORDER::next` links are rewired to clone-owned vector storage
  after value copy；
- preserve expected aliases for resolved `Item*`, `Field*`, source `ORDER*`
  mapping, and optimized membership flags；
- keep real clone lifecycle and `JOIN::pq_copy_from()` untouched。

Implementation:

- added `pq_clone_order_chain_sidecar()`:
  - value-copies the sidecar；
  - rewires cloned `nodes` and `restored_nodes` `next` chains inside clone
    vector storage；
  - verifies copied vector sizes for owned nodes, restored nodes,
    source-node mapping, and optimized flags；
- added clone-match helpers that verify:
  - cloned node addresses do not alias leader sidecar nodes；
  - copied ORDER scalar fields and aliased pointers match；
  - cloned `next` links point inside clone-owned vectors；
  - source node mapping and optimized flags are preserved；
- added DBUG smoke `pq_saved_order_chain_clone_copy_smoke` on the existing
  `HAS_ORDER_BY` reject path；
- smoke builds the E5d-2c restored leader sidecar, clone-copies it, then
  validates both the full copied chain and restored optimized chain；
- added counters:
  `Parallel_saved_order_chain_clone_copy_smoke_attempts`,
  `Parallel_saved_order_chain_clone_copy_smoke_success`,
  `Parallel_saved_order_chain_clone_copy_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify no-DBUG zero counters,
  DBUG attempts/success, unsupported zero, serial ORDER BY boundary, and zero
  executed/workers/ranges；
- updated `pq_stats` Parallel status variable count from 127 to 130。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no `sql/parallel_query/pq_clone.*` changes；
- no real `JOIN::pq_copy_from()` wiring；
- no `sql/sql_optimizer.*` hook or live optimizer list mutation；
- no real `Filesort`, `Sort_param`, `Filesort::make_sortorder()`, sorting
  iterator, or `exchange_sort.*` integration；
- no worker, handler, MQ, AccessPath, `Read()`, or `HAS_ORDER_BY`
  eligibility relaxation。

Code/Doc/Test Review - M11-E5d-2d:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `pq_clone_order_chain_sidecar()` value-copies the sidecar and
  rewires both `nodes` and `restored_nodes` `ORDER::next` links to clone-owned
  vector storage；
- confirmed clone-match checks prove copied node storage does not alias leader
  sidecar nodes；
- confirmed scalar fields, aliased `Item*` / field-related pointers,
  `source_nodes`, and optimized flags are preserved；
- confirmed DBUG smoke remains under the existing `HAS_ORDER_BY` reject path
  and does not enable user-visible ORDER BY PQ；
- confirmed SHOW STATUS counters, reset, `pq_stats` count/order, MTR
  assertions, and docs are consistent；
- confirmed forbidden scope is not touched。

Commit:

- `a4cc361c759` Add PQ M11E order chain clone copy smoke。

### M11-E5d-3: Post-Sidecar Filesort Boundary Design

Status: design-only taskbook completed；Design Review Agent accepted；committed
as `2665375de96`。

Goal:

- define the first integration boundary after E5d-2 sidecar completion；
- make `PQOrderByFilesortContract` depend on restored sidecar readiness rather
  than scalar-only `PQSavedOrderGroupContract::READY`；
- keep real `Filesort`, `Sort_param`, `Filesort::make_sortorder()`, sorting
  iterator, `Exchange_sort`, worker MQ, and user-visible ORDER BY PQ disabled；
- prepare a minimal, reviewable E5d-3a coding step that proves contract state
  only, not Filesort lifecycle。

Commercial reference:

- commercial `ParallelScanIterator::pq_make_filesort()` restores optimized
  ORDER/GROUP state before creating leader `Filesort`；
- commercial `restore_optimized_group_order()` rebuilds an optimized ORDER
  chain from original list plus optimized flags；
- commercial `Exchange_sort::init()` assumes `Filesort::m_order` is already
  valid, then calls `Filesort::make_sortorder()` and initializes `Sort_param`；
- current MySQL 8.0.46 `Filesort` constructor calls private
  `make_sortorder()` immediately, so constructing `Filesort` is already a real
  Filesort lifecycle action, not a harmless shape check。

Current branch facts:

- E5d-2a owns copied `ORDER` nodes and rewired `next` links；
- E5d-2b records source-vs-optimized membership flags；
- E5d-2c reconstructs a restored sidecar ORDER chain from owned nodes and
  flags；
- E5d-2d proves leader-to-clone diagnostic copy rewires cloned `next` links to
  clone-owned vectors；
- the sidecar is still internal to `pq_optimizer.cc` and has no public API；
- existing `PQOrderByFilesortContract` remains fail-closed because it depends
  on scalar-only `PQSavedOrderGroupContract` readiness。

Design decision:

- do not construct `Filesort` in the next coding step；
- do not call `Filesort::make_sortorder()` directly or indirectly；
- do not include `sql/filesort.h` in `pq_optimizer.cc` for E5d-3a；
- expose only a narrow contract shape that records whether a restored
  sidecar ORDER chain exists and how many ORDER nodes it contains；
- keep the restored ORDER pointer private/internal until a later reviewed
  Filesort lifecycle phase decides ownership and lifetime；
- preserve `HAS_ORDER_BY` serial rejection and DBUG-only observability；
- treat GROUP-based Filesort, ordered-index synthesized key order, DESC group
  marking, and real `Sort_param` as later separate phases。

Proposed coding split:

1. M11-E5d-3a Restored ORDER Filesort Contract:
   - allowed files:
     - `sql/parallel_query/pq_optimizer.h`
     - `sql/parallel_query/pq_optimizer.cc`
     - `sql/parallel_query/sql_parallel.h`
     - `sql/mysqld.cc`
     - `mysql-test/suite/parallel_query/t/pq_saved_order_group_contract.test`
     - `mysql-test/suite/parallel_query/r/pq_saved_order_group_contract.result`
     - `mysql-test/suite/parallel_query/r/pq_stats.result`
     - this task document and README；
   - extend `PQOrderByFilesortContract` with diagnostics such as:
     - `restored_order_ready`
     - `restored_order_count`
     - `sidecar_clone_ready`
   - add an internal helper that builds the owned ORDER sidecar, records
     optimized flags, restores the sidecar chain, clone-copies it, and fills
     the contract；
   - add DBUG smoke
     `pq_orderby_filesort_restored_order_contract_smoke` on the existing
     ORDER BY reject path；
   - add counters:
     `Parallel_orderby_filesort_restored_order_contract_attempts`,
     `Parallel_orderby_filesort_restored_order_contract_success`,
     `Parallel_orderby_filesort_restored_order_contract_unsupported`；
   - MTR must prove no-DBUG zero counters, DBUG success for the controlled
     two-column ORDER BY shape, `Not parallel HAS_ORDER_BY`, and zero
     executed/workers/ranges。
2. M11-E5d-3b Filesort Constructor Risk Review:
   - design-only unless E5d-3a review accepts；
   - inspect whether constructing `Filesort` on `THD::mem_root` during
     eligibility reject path is safe enough for a debug-only smoke；
   - decide whether the next step may use a temporary MEM_ROOT, leader THD
     mem_root, or must wait until real iterator construction。
3. M11-E5d-3c Debug-only Filesort Construction Smoke:
   - only after E5d-3b review；
   - if approved, construct `Filesort` only under a dedicated DBUG flag and
     still reject `HAS_ORDER_BY`；
   - must not initialize `Sort_param`, `Exchange_sort`, worker MQ, or
     user-visible ORDER BY PQ。

Forbidden files / actions for E5d-3a:

- no `sql/filesort.*` include or edits；
- no `sql/iterators/sorting_iterator.*` edits；
- no `sql/parallel_query/exchange_sort.*` edits；
- no `sql/parallel_query/pq_iterators.*` edits；
- no `sql/parallel_query/pq_clone.*` edits；
- no `sql/sql_optimizer.*` hook or semantic change；
- no `Filesort` allocation；
- no `Sort_param` allocation or initialization；
- no direct or indirect `Filesort::make_sortorder()` call；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation。

Required validation for E5d-3a:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d3a_target --tmpdir=/tmp/pqt_m11e5d3a_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d3a_full --tmpdir=/tmp/pqt_m11e5d3a_full
```

Design review request:

- confirm E5d-3a should remain contract-only and must not construct
  `Filesort`；
- confirm `Filesort` constructor is unsafe as a shape check because it calls
  `make_sortorder()` immediately；
- confirm `PQOrderByFilesortContract` may be extended with restored sidecar
  readiness diagnostics without exposing the internal sidecar pointer；
- confirm GROUP, ordered-index synthesized order, DESC group marking,
  `Sort_param`, and `Exchange_sort` should remain separate follow-ups；
- confirm validation and forbidden scope are sufficient。

Design Review - M11-E5d-3:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- required changes: none；
- confirmed E5d-3a should stay contract-only and continue to reject
  `HAS_ORDER_BY` after DBUG-only probes；
- confirmed MySQL 8.0.46 `Filesort` construction is not a harmless shape
  check because the constructor calls `make_sortorder()` immediately and
  allocates/initializes `sortorder`；
- confirmed extending `PQOrderByFilesortContract` with restored sidecar
  readiness/count/clone-ready diagnostics is the correct minimal next step；
- confirmed not exposing the internal sidecar pointer preserves the lifetime
  boundary；
- confirmed GROUP Filesort, ordered-index synthesized order, DESC group
  marking, `Sort_param`, `Exchange_sort`, worker MQ, and user-visible ORDER BY
  PQ must remain separate follow-ups；
- confirmed allowed/forbidden scope and validation commands are sufficient。

Commit:

- `2665375de96` Plan PQ M11E filesort sidecar boundary。

### M11-E5d-3a: Restored ORDER Filesort Contract

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；committed。

Goal:

- extend `PQOrderByFilesortContract` with restored sidecar readiness
  diagnostics；
- prove the restored ORDER sidecar can satisfy the first post-sidecar Filesort
  contract boundary；
- keep this contract-only: no real `Filesort`, `Sort_param`,
  `Filesort::make_sortorder()`, `Exchange_sort`, or user-visible ORDER BY PQ。

Implementation:

- extended `PQOrderByFilesortContract` with:
  - `restored_order_ready`
  - `restored_order_count`
  - `sidecar_clone_ready`；
- added status
  `PQOrderByFilesortContractStatus::UNSUPPORTED_MISSING_RESTORED_ORDER`；
- kept existing `pq_build_orderby_filesort_contract()` fail-closed behavior for
  the E5d-1 scalar saved-contract smoke；
- added internal helper
  `pq_build_orderby_filesort_restored_order_contract()` that:
  - builds the owned ORDER sidecar；
  - records optimized membership flags；
  - restores the sidecar ORDER chain；
  - verifies restored ORDER semantics；
  - clone-copies the sidecar and verifies clone-owned `next` links；
  - fills restored-order readiness/count/clone-ready diagnostics；
- added DBUG smoke
  `pq_orderby_filesort_restored_order_contract_smoke` on the existing
  `HAS_ORDER_BY` reject path；
- added counters:
  `Parallel_orderby_filesort_restored_order_contract_attempts`,
  `Parallel_orderby_filesort_restored_order_contract_success`,
  `Parallel_orderby_filesort_restored_order_contract_unsupported`；
- extended `pq_saved_order_group_contract` to verify no-DBUG zero counters,
  DBUG attempts/success, unsupported zero, serial ORDER BY boundary, and zero
  executed/workers/ranges；
- updated `pq_stats` Parallel status variable count from 130 to 133。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no `sql/filesort.*` include or edits；
- no `Filesort` allocation；
- no direct or indirect `Filesort::make_sortorder()` call；
- no `Sort_param` allocation or initialization；
- no `sql/iterators/sorting_iterator.*` edits；
- no `sql/parallel_query/exchange_sort.*` edits；
- no `sql/parallel_query/pq_iterators.*` edits；
- no `sql/parallel_query/pq_clone.*` edits；
- no `sql/sql_optimizer.*` hook or semantic change；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation。

Code/Doc/Test Review - M11-E5d-3a:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- validation gaps: none；
- confirmed tracked diff only covers the allowed E5d-3a scope；
- confirmed no `sql/filesort.*`, sorting iterator, `exchange_sort.*`,
  `pq_iterators.*`, `pq_clone.*`, `sql_optimizer.*`, handler/InnoDB,
  AccessPath, `Read()`, MQ, or worker path changes；
- confirmed the new helper only uses owned ORDER sidecar, flag restore, and
  clone-copy checks；
- confirmed no restored sidecar pointer is exposed and live `JOIN::order`,
  `Item`, and `Field` state are not modified；
- confirmed `PQOrderByFilesortContract` reset and status semantics are
  complete；
- confirmed DBUG smoke remains under the existing `HAS_ORDER_BY` reject path
  and success requires `ready()`, `restored_order_ready`,
  `restored_order_count > 0`, and `sidecar_clone_ready`；
- confirmed SHOW STATUS counters, reset, `pq_stats`, MTR assertions, and docs
  are consistent。

Commit:

- `952beeeafa1` Add PQ M11E restored order filesort contract。

### M11-E5d-3b: Filesort Constructor Risk Review

Status: design-only taskbook completed；Design Review Agent accepted；committed
as `1020b3b5c2b`。

Goal:

- decide whether the next coding stage may create a debug-only `Filesort`
  object from the restored ORDER sidecar；
- define strict memory/lifecycle boundaries if a debug-only construction smoke
  is allowed；
- keep `Sort_param`, `Exchange_sort`, worker MQ, real row sorting, and
  user-visible ORDER BY PQ out of scope。

Current branch facts:

- `Filesort::Filesort()` calls `make_sortorder(order, unwrap_rollup)` in its
  initializer；
- `Filesort::make_sortorder()` counts ORDER nodes, allocates `sortorder` from
  `THR_MALLOC`, initializes `st_sort_field`, stores `real_item()`, and sets
  reverse flags from `ORDER::direction`；
- normal executor/finalize paths allocate `Filesort` on `thd->mem_root` and
  attach it to execution-owned cleanup structures such as
  `JOIN::filesorts_to_cleanup` or QEP/AccessPath objects；
- constructing `Filesort` inside an eligibility reject path would be a real
  allocation and sortorder initialization, not a passive shape check；
- E5d-3a already proves the restored sidecar ORDER chain exists and can be
  clone-copied without exposing the sidecar pointer。

Design decision:

- do not construct `Filesort` in the normal eligibility path；
- do not construct `Filesort` unless a dedicated DBUG flag is set；
- if E5d-3c is approved, construct `Filesort` only from the restored sidecar
  ORDER chain after E5d-3a contract success；
- E5d-3c must allocate on the current THD MEM_ROOT like normal MySQL
  `Filesort` construction, then rely on statement MEM_ROOT cleanup；
- E5d-3c must not attach the debug-only `Filesort` to
  `JOIN::filesorts_to_cleanup`, QEP_TAB, AccessPath, iterator state, or any
  persistent PQ state；
- E5d-3c must not call `using_addon_fields()`, `Sort_param::init_for_filesort()`,
  `filesort()`, `Exchange_sort::init()`, or worker MQ code；
- E5d-3c may inspect only constructor-visible diagnostics such as
  `sort_order_length()` and should treat zero length as unsupported；
- E5d-3c must stay in the existing `HAS_ORDER_BY` reject path and must still
  return serial rejection。

Proposed next coding step:

1. M11-E5d-3c Debug-only Filesort Construction Smoke:
   - allowed files:
     - `sql/parallel_query/pq_optimizer.cc`
     - `sql/parallel_query/sql_parallel.h`
     - `sql/mysqld.cc`
     - focused MTR under `mysql-test/suite/parallel_query/`
     - this task document and README；
   - may include `sql/filesort.h` in `pq_optimizer.cc` only for this DBUG-only
     smoke；
   - add internal helper that builds restored sidecar contract, then constructs
     `Filesort(thd, {table}, false, restored_order, HA_POS_ERROR, false,
     false, false)` under DBUG；
   - verify `sort_order_length() == restored_order_count`；
   - add counters:
     `Parallel_orderby_filesort_construct_smoke_attempts`,
     `Parallel_orderby_filesort_construct_smoke_success`,
     `Parallel_orderby_filesort_construct_smoke_unsupported`；
   - MTR must verify no-DBUG zero counters, DBUG success, old
     `HAS_ORDER_BY` rejection, and zero executed/workers/ranges。

Forbidden files / actions for E5d-3c:

- no `sql/filesort.*` edits；
- no `sql/iterators/sorting_iterator.*` edits；
- no `sql/parallel_query/exchange_sort.*` edits；
- no `sql/parallel_query/pq_iterators.*` edits；
- no `sql/parallel_query/pq_clone.*` edits；
- no `sql/sql_optimizer.*` hook or semantic change；
- no `Sort_param` allocation or initialization；
- no `Filesort::using_addon_fields()`；
- no `filesort()` execution；
- no `Exchange_sort::init()`；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation。

Required validation for E5d-3c:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d3c_target --tmpdir=/tmp/pqt_m11e5d3c_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d3c_full --tmpdir=/tmp/pqt_m11e5d3c_full
```

Design review request:

- confirm a DBUG-only `Filesort` construction smoke is acceptable after
  E5d-3a contract success；
- confirm THD MEM_ROOT allocation with statement cleanup is acceptable for
  debug-only smoke；
- confirm the smoke must not attach the `Filesort` to cleanup/execution
  structures；
- confirm inspecting only `sort_order_length()` is enough for this stage；
- confirm `Sort_param`, `Exchange_sort`, worker MQ, and user-visible ORDER BY
  PQ remain later phases。

Design Review - M11-E5d-3b:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- required changes: none；
- confirmed dedicated DBUG-only `Filesort` construction is reasonable after
  E5d-3a success；
- confirmed current THD MEM_ROOT allocation is consistent with normal
  construction paths；
- confirmed not attaching the debug-only `Filesort` to
  `JOIN::filesorts_to_cleanup`, QEP_TAB, AccessPath, or iterator state is
  acceptable because no execution-owned cleanup path should see it；
- confirmed `sort_order_length() == restored_order_count` is sufficient for
  the first construction smoke；
- confirmed `using_addon_fields()` must stay forbidden because it lazily
  initializes `Sort_param` state；
- confirmed no `filesort()`, `Exchange_sort`, worker/MQ, or eligibility
  relaxation should be introduced；
- confirmed E5d-3c must keep the restored sidecar ORDER chain alive through
  the constructor call and treat zero length or failed contract as unsupported。

Commit:

- `1020b3b5c2b` Plan PQ M11E filesort construction smoke。

### M11-E5d-3c: Debug-only Filesort Construction Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；full
`parallel_query` suite passed；committed。

Goal:

- prove a restored sidecar ORDER chain can be consumed by the MySQL 8.0.46
  `Filesort` constructor under a dedicated DBUG flag；
- verify constructor-visible `make_sortorder()` consumes the restored ORDER
  chain by checking `sort_order_length()`；
- keep the constructed `Filesort` debug-only and detached from normal
  execution cleanup/state。

Implementation:

- included `sql/filesort.h` in `pq_optimizer.cc` only for this DBUG-only
  smoke；
- added `pq_find_orderby_filesort_smoke_table()` to conservatively find the
  single leader table from `best_ref`, `qep_tab`, or `Query_block` table list；
- added `pq_run_orderby_filesort_construct_smoke()`:
  - builds and verifies the owned ORDER sidecar；
  - restores the optimized sidecar ORDER chain；
  - clone-copies the sidecar and verifies clone-owned `next` links；
  - constructs `Filesort` on current `THD::mem_root` while the clone sidecar
    remains alive；
  - verifies `filesort->sort_order_length() == restored_order_count`；
  - does not store or attach the `Filesort *` anywhere；
- added DBUG smoke `pq_orderby_filesort_construct_smoke` on the existing
  `HAS_ORDER_BY` reject path；
- added counters:
  `Parallel_orderby_filesort_construct_smoke_attempts`,
  `Parallel_orderby_filesort_construct_smoke_success`,
  `Parallel_orderby_filesort_construct_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify no-DBUG zero counters,
  DBUG attempts/success, unsupported zero, serial ORDER BY boundary, and zero
  executed/workers/ranges；
- updated `pq_stats` Parallel status variable count from 133 to 136。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_saved_order_group_contract pq_commercial_order_by pq_stats` 4/4
  including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Scope notes:

- no `sql/filesort.*` edits；
- no `JOIN::filesorts_to_cleanup`, QEP_TAB, AccessPath, iterator, or PQ state
  attachment；
- no `Filesort::using_addon_fields()`；
- no `Sort_param` allocation or initialization；
- no `filesort()` execution；
- no `sql/iterators/sorting_iterator.*`, `exchange_sort.*`, `pq_iterators.*`,
  `pq_clone.*`, or `sql_optimizer.*` changes；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation。

Code/Doc/Test Review - M11-E5d-3c:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `pq_orderby_filesort_construct_smoke` is dedicated DBUG-only and
  called only inside the existing `HAS_ORDER_BY` reject path；
- confirmed restored clone sidecar stays alive through `Filesort`
  construction and `sort_order_length()` check；
- confirmed the `Filesort *` is local only and not attached to cleanup,
  execution, or PQ state；
- confirmed scope is narrow: constructor plus
  `sort_order_length() == restored_count` only；
- confirmed no `Sort_param`, `using_addon_fields()`, `filesort()`,
  `Exchange_sort`, worker/MQ, `Read()`, or eligibility relaxation；
- confirmed table selection is conservative and returns unsupported when no
  table is available；
- confirmed status counters, reset, MTR expectations, and `pq_stats` count/order
  are consistent。

Commit:

- `c4b7a7953e1` Add PQ M11E filesort construction smoke。

### M11-E5d-4: Sort_param / Exchange_sort Initialization Boundary Design

Status: design-only taskbook completed；Design Review Agent accepted；committed
as `cfc1ad722cd`。

Goal:

- decide the next safe boundary after debug-only `Filesort` construction；
- evaluate whether `Sort_param::init_for_filesort()` and `Exchange_sort::init()`
  can be tested without entering worker MQ or real ORDER BY execution；
- keep user-visible ORDER BY PQ disabled until `Sort_param`, frame format, MQ
  ownership, and `Read()` consumption are reviewed together。

Initial constraints:

- no coding before design review；
- no default path `Sort_param` initialization；
- no `Exchange_sort::init()` on real SQL path；
- no worker MQ, `Query_result_mq`, `ParallelScanIterator::Read()`, handler,
  InnoDB, AccessPath, or optimizer eligibility changes；
- any future coding must be DBUG-only and must preserve `HAS_ORDER_BY`
  rejection。

Commercial reference:

- commercial `Exchange_sort::init()` does all of the following in one path:
  - calls `Exchange::init()`；
  - calls `m_sort->make_sortorder(m_sort->m_order, false)`；
  - applies DESC group metadata to `m_sort->sortorder[pos].reverse`；
  - allocates `Sort_param` on PQ mem_root；
  - pushes the leader table into `m_sort->tables`；
  - calls `Sort_param::init_for_filesort()`；
  - allocates compare keys and tmp rowid buffers；
  - allocates record groups, min-record array, and heap state；
  - later consumes worker MQ frames through `read_mq_record()`。
- current branch already has synthetic `Exchange_sort` smoke and ORDER BY frame
  smoke, but no real `Filesort`/`Sort_param` member state in `Exchange_sort`。

Current branch facts:

- E5d-3c proves a restored sidecar ORDER chain can be consumed by the
  `Filesort` constructor under DBUG；
- `Filesort` constructor already calls `make_sortorder()` once；
- MySQL 8.0.46 `Filesort::make_sortorder()` is private, so commercial
  `Exchange_sort::init()` cannot be copied directly without changing
  `Filesort` visibility or reworking the flow；
- `Sort_param::init_for_filesort()` calls `decide_addon_fields()`, which reads
  table metadata/read sets and decides row-id vs addon-field layout；
- `using_addon_fields()` is lazy in normal code and is intentionally not called
  from the `Filesort` constructor because read sets may not be fully finalized
  at constructor time；
- E5d-3c runs in an eligibility reject path, not in executor iterator
  construction。

Design decision:

- do not copy commercial `Exchange_sort::init()` as-is；
- do not expose `Filesort::make_sortorder()` or change `sql/filesort.*` for
  E5d-4；
- do not call `Exchange_sort::init()` with real `Filesort` state yet；
- split the next work into two smaller gates:
  - E5d-4a: debug-only `Sort_param` initialization smoke, without
    `Exchange_sort`；
  - E5d-4b: design-only `Exchange_sort` real-state adapter after E5d-4a
    passes review；
- E5d-4a may allocate local `Sort_param` only under a dedicated DBUG flag and
  only after E5d-3c-style restored sidecar + debug Filesort construction
  succeeds；
- E5d-4a must not call `Filesort::using_addon_fields()` separately because
  `Sort_param::init_for_filesort()` already performs the required decision；
- E5d-4a must verify only narrow diagnostics:
  - `local_sortorder.size() == filesort->sort_order_length()`；
  - `max_record_length() > 0`；
  - no execution counters grow；
- E5d-4a must not persist `Sort_param`, `Filesort`, sort keys, or buffers；
- E5d-4a must not attach any object to `JOIN`, `QEP_TAB`, `AccessPath`,
  `Exchange_sort`, iterator, MQ, or PQ state。

Proposed next coding step:

1. M11-E5d-4a Debug-only Sort_param Init Smoke:
   - allowed files:
     - `sql/parallel_query/pq_optimizer.cc`
     - `sql/parallel_query/sql_parallel.h`
     - `sql/mysqld.cc`
     - focused MTR under `mysql-test/suite/parallel_query/`
     - this task document and README；
   - may include `sql/sort_param.h` if not already pulled by `filesort.h`；
   - add internal helper that:
     - repeats E5d-3c restored sidecar + debug Filesort construction；
     - builds a local `Sort_param` on the stack；
     - calls `init_for_filesort(filesort, make_array(filesort->sortorder,
       sort_order_length), sortlength(thd, filesort->sortorder,
       sort_order_length), filesort->tables, HA_POS_ERROR, false)`；
     - verifies local sortorder length and max record length；
     - does not store the local `Sort_param` anywhere；
   - add counters:
     `Parallel_orderby_sort_param_init_smoke_attempts`,
     `Parallel_orderby_sort_param_init_smoke_success`,
     `Parallel_orderby_sort_param_init_smoke_unsupported`；
   - MTR verifies no-DBUG zero counters, DBUG success, `HAS_ORDER_BY`
     rejection, and zero executed/workers/ranges。

Forbidden files / actions for E5d-4a:

- no `sql/filesort.*` edits；
- no `sql/sort_param.*` edits；
- no `sql/iterators/sorting_iterator.*` edits；
- no `sql/parallel_query/exchange_sort.*` edits；
- no `sql/parallel_query/pq_iterators.*` edits；
- no `sql/parallel_query/pq_clone.*` edits；
- no `sql/sql_optimizer.*` hook or semantic change；
- no persisted `Filesort` or `Sort_param` pointer；
- no `filesort()` execution；
- no `Exchange_sort::init()`；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or
  `HAS_ORDER_BY` eligibility relaxation。

Required validation for E5d-4a:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4a_target --tmpdir=/tmp/pqt_m11e5d4a_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4a_full --tmpdir=/tmp/pqt_m11e5d4a_full
```

Design review request:

- confirm E5d-4a may call `Sort_param::init_for_filesort()` under DBUG after
  E5d-3c succeeds；
- confirm E5d-4a should remain independent from current `Exchange_sort`；
- confirm not changing `Filesort::make_sortorder()` visibility is required；
- confirm stack-local `Sort_param` and THD MEM_ROOT `Filesort` lifetime are
  acceptable for debug-only smoke；
- confirm E5d-4b should be a separate design for real `Exchange_sort` state
  adapter。

Design Review - M11-E5d-4:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- required changes: none；
- confirmed E5d-4a may call stack-local `Sort_param::init_for_filesort()`
  under a dedicated DBUG flag after E5d-3c succeeds；
- confirmed E5d-4a must stay in the `HAS_ORDER_BY` reject/debug-smoke window；
- confirmed E5d-4a should remain independent from current `Exchange_sort`
  because commercial `Exchange_sort::init()` mixes `make_sortorder`,
  `Sort_param`, buffers, heap, and MQ consumption；
- confirmed `Filesort::make_sortorder()` visibility must not change；
- confirmed `local_sortorder.size() == filesort->sort_order_length()` plus
  `max_record_length() > 0`, with zero executed/workers/ranges counters, is
  sufficient for the first `Sort_param` smoke；
- coding caveat: `init_for_filesort()` may allocate/cache addon descriptors
  through the debug `Filesort` embedded `m_sort_param`, so the temporary
  `Filesort` must remain unpersisted and debug-only；
- confirmed forbidden scope, allowed files, and validation commands are
  complete for E5d-4a。

### M11-E5d-4a: Debug-only Sort_param Init Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；`git diff
--check`, `mysqld` build, targeted MTR, and full `parallel_query` suite passed；
committed as `08579119b9b`。

Goal:

- verify that a restored sidecar ORDER chain can initialize a stack-local
  `Sort_param` through a debug `Filesort` under a dedicated DBUG flag；
- keep this validation inside the existing `HAS_ORDER_BY` reject path；
- do not attach `Filesort`, `Sort_param`, buffers, heap, MQ, or
  `Exchange_sort` state to the user-visible execution path。

Implementation summary:

- added `pq_run_orderby_sort_param_init_smoke()` in
  `sql/parallel_query/pq_optimizer.cc`；
- the helper reuses the E5d-2/E5d-3 sidecar flow:
  - copies the current ORDER chain into leader-owned sidecar storage；
  - records and restores optimized flags for the ORDER chain without the first
    node；
  - clone-copies the restored sidecar；
  - constructs a debug-only `Filesort` from the cloned restored ORDER chain；
  - initializes a stack-local `Sort_param` with `init_for_filesort()`；
  - accepts only when `local_sortorder.size() == sort_order_length()` and
    `max_record_length() > 0`；
- added DBUG flag `pq_orderby_sort_param_init_smoke`；
- added status counters:
  - `Parallel_orderby_sort_param_init_smoke_attempts`；
  - `Parallel_orderby_sort_param_init_smoke_success`；
  - `Parallel_orderby_sort_param_init_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify:
  - no-DBUG counters remain zero；
  - DBUG smoke records one attempt and one success；
  - `HAS_ORDER_BY` still rejects the query；
  - `Parallel_queries_executed`, `Parallel_workers_launched`, and
    `Parallel_ranges_dispatched` remain zero；
- masked the unstable EXPLAIN `rows` estimate in this test with
  `--replace_column 10 #` because it can vary between 4 and 5 across runs。

Scope confirmation:

- no edits to `sql/filesort.*`, `sql/sort_param.*`,
  `sql/iterators/sorting_iterator.*`, `sql/parallel_query/exchange_sort.*`,
  `sql/parallel_query/pq_iterators.*`, `sql/parallel_query/pq_clone.*`, or
  `sql/sql_optimizer.*`；
- no `Filesort::make_sortorder()` visibility change；
- no direct `using_addon_fields()` call；
- no `filesort()` execution；
- no `Exchange_sort::init()`；
- no worker thread, MQ, handler/InnoDB, `Read()`, AccessPath, or ORDER BY
  eligibility relaxation；
- the `Sort_param` is stack-local and never persisted。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4a_target2 --tmpdir=/tmp/pqt_m11e5d4a_target2
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4a_full --tmpdir=/tmp/pqt_m11e5d4a_full
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，4/4 including `shutdown_report`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-4a:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed the DBUG flag is checked before attempts counter increments；
- confirmed the call is only inside the existing `query_block->is_ordered()`
  / `HAS_ORDER_BY` rejection window；
- confirmed stack-local `Sort_param` is not stored；
- confirmed debug `Filesort` is only fed by cloned restored sidecar ORDER
  nodes whose owning vectors remain alive across constructor and
  `init_for_filesort()`；
- confirmed `HAS_ORDER_BY` remains rejected after smoke execution；
- confirmed counters, reset, `SHOW_VAR`, and `pq_stats` are consistent；
- no blocking test or documentation gaps。

### M11-E5d-4b: Exchange_sort Real-state Adapter Boundary Design

Status: design-only taskbook completed；Design Review Agent accepted；committed
as `ea83b4c3c9c`。

Goal:

- define the smallest safe boundary for moving from a standalone
  `Sort_param` smoke to an `Exchange_sort`-owned real-state shape；
- avoid directly copying commercial `Exchange_sort::init()` because it combines
  `Filesort::make_sortorder()`, DESC metadata, `Sort_param`, heap buffers,
  record groups, MQ handles, and `read_mq_record()` consumption in one path；
- keep user-visible ORDER BY PQ disabled until the adapter, frame format, MQ
  consumption, and `Read()` path are jointly reviewed。

Commercial reference summary:

- commercial `ParallelScanIterator::pq_make_filesort()` rebuilds leader ORDER
  state, handles optimized-out ORDER BY / index-order cases, creates a
  `Filesort`, and passes it to `MQ_record_gather::mq_scan_init()`；
- commercial `MQ_record_gather::mq_scan_init()` constructs `Exchange_sort`
  with leader `TABLE`, worker split-table handler, worker count, ref length,
  DESC group metadata, stable-output flag, and index-sort flag；
- commercial `Exchange_sort::init()` then:
  - calls `Exchange::init()`；
  - calls `Filesort::make_sortorder()` again through a public commercial
    method；
  - applies DESC group metadata to `sortorder[pos].reverse`；
  - allocates persistent `Sort_param` on `pq_mem_root`；
  - pushes the leader table into `Filesort::tables`；
  - calls `Sort_param::init_for_filesort()`；
  - allocates compare-key buffers, tmp rowid buffer, per-worker record groups,
    min-record array, and heap；
  - later `store_mq_record()` deep-copies MQ rows and `read_mq_record()` copies
    the selected record into `table->record[0]`。

Current branch facts:

- current `Exchange_sort` is intentionally smaller and owns:
  - typed ORDER BY frame decode/validation；
  - synthetic K-way merge smoke；
  - controlled frame merge smoke；
  - empty-worker / ERROR edge smoke；
  - debug-only row materialization smoke；
  - a lightweight order-gather shape (`init_order_gather_shape()`) with worker,
    stable-output, and index-sort flags；
- current branch does not have commercial `MQ_record_gather`；
- current branch does not expose `Filesort::make_sortorder()`；
- E5d-4a proves `Sort_param::init_for_filesort()` can initialize from a debug
  `Filesort` built from a restored sidecar ORDER chain, but it deliberately
  does not persist state in `Exchange_sort`；
- current worker result format for ORDER BY remains the typed
  `PQ_orderby_frame_header` / `PQ_orderby_cached_record` smoke format, not a
  direct commercial `mq_record_st` copy。

Design decision:

- do not copy commercial `Exchange_sort::init()` wholesale；
- do not add a real `Exchange_sort::init()` override that consumes `Filesort`
  or MQ in the next coding step；
- do not modify `sql/filesort.*` to expose `make_sortorder()`；
- introduce the next coding task as a DBUG-only adapter-shape smoke that
  stores scalar observable metadata only；
- do not persist a real `Sort_param` object/snapshot in E5d-4b-1；persistent
  `Sort_param` ownership and teardown require a separate follow-up design；
- keep all persistent adapter state unreachable from default SQL execution；
- keep the current synthetic/typed-frame smokes as test guards around the
  future adapter。

Proposed next coding step:

1. M11-E5d-4b-1 Debug-only Exchange_sort Sort-state Adapter Shape:
   - allowed files:
     - `sql/parallel_query/exchange_sort.h`
     - `sql/parallel_query/exchange_sort.cc`
     - `sql/parallel_query/sql_parallel.cc` only if the existing smoke runner
       needs to call the new helper；
     - `sql/parallel_query/sql_parallel.h` and `sql/mysqld.cc` only for new
       counters；
     - focused MTR under `mysql-test/suite/parallel_query/`；
     - this task document and README；
   - forbidden for this coding step:
     - `sql/parallel_query/pq_optimizer.cc`；
   - preferred implementation:
     - add a small `PQ_orderby_sort_state_shape` struct or equivalent private
       fields in `Exchange_sort` that records worker count, stable-output flag,
       index-sort flag, sort-order length, `max_record_length`, and whether
       rowid/ref buffers would be required；
     - add a DBUG-only smoke helper driven entirely by synthetic scalar values
       from `Exchange_sort` / `sql_parallel` smoke setup；
     - copy only scalar metadata into `Exchange_sort`, then immediately
       cleanup/resets it；
     - verify the existing frame smokes still pass and `HAS_ORDER_BY` remains
       rejected；
   - follow-up, not E5d-4b-1:
     - passing scalar values produced from E5d-4a-style `Filesort` /
       stack-local `Sort_param` initialization across module boundaries；
     - any `pq_optimizer.cc` hook that connects optimizer-side Filesort
       validation with `Exchange_sort` state。

Forbidden files / actions for E5d-4b-1:

- no `sql/filesort.*` or `sql/sort_param.*` edits；
- no `sql/parallel_query/pq_optimizer.cc` edits；
- no `Filesort::make_sortorder()` visibility change；
- no commercial `MQ_record_gather` import；
- no `Exchange_sort::init()` real-path override；
- no worker MQ consumption from default SQL；
- no `ParallelScanIterator::Read()` changes；
- no optimizer eligibility or AccessPath changes；
- no handler/InnoDB changes；
- no persisted raw `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, or
  handler pointer in adapter state；
- no persisted real `Sort_param` object or snapshot；
- no `filesort()` execution。

Required validation for E5d-4b-1:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_saved_order_group_contract pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5d4b1_target \
  --tmpdir=/tmp/pqt_m11e5d4b1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4b1_full --tmpdir=/tmp/pqt_m11e5d4b1_full
```

MTR requirements for E5d-4b-1:

- no-DBUG counters for the new adapter-shape smoke must remain zero；
- DBUG smoke must record expected attempt/success counters, or a deliberate
  fail-closed unsupported counter if a synthetic prerequisite is absent；
- real ORDER BY SQL must still show `HAS_ORDER_BY` serial rejection；
- `Parallel_queries_executed`, `Parallel_workers_launched`, and
  `Parallel_ranges_dispatched` must remain zero for the real ORDER BY negative
  guard。

Design review request:

- confirm E5d-4b should remain design/shape-first and not copy commercial
  `Exchange_sort::init()`；
- confirm E5d-4b-1 should not persist `Filesort *` / `Sort_param *` pointers
  or real `Sort_param` snapshots；
- confirm scalar sort-state metadata is sufficient for the next adapter smoke；
- confirm `Exchange_sort::init()` real path, worker MQ consumption, and
  `ParallelScanIterator::Read()` integration must remain separate follow-ups；
- confirm the allowed/forbidden file boundaries and validation list are
  complete。

Design Review - M11-E5d-4b:

- first review verdict: `REVISE`；
- required changes applied:
  - resolved `pq_optimizer.cc` ambiguity by forbidding it for E5d-4b-1；
  - moved optimizer-side scalar handoff to a later follow-up；
  - tightened E5d-4b-1 to scalar metadata only；
  - explicitly prohibited persisted real `Sort_param` object/snapshot；
  - updated stale README next action；
  - added MTR requirements for no-DBUG zero counters, DBUG success or
    fail-closed unsupported counters, `HAS_ORDER_BY` rejection, and zero
    executed/workers/ranges；
- re-review verdict: `ACCEPT`；
- findings after revision: none blocking；
- confirmed E5d-4b correctly blocks direct commercial `Exchange_sort::init()`
  copy and separates `make_sortorder`, DESC metadata, `Sort_param`, heap
  buffers, MQ, `Read()`, and eligibility；
- confirmed E5d-4b-1 may proceed as a debug-only scalar sort-state adapter
  shape under `Exchange_sort` / `sql_parallel` synthetic smoke setup。

### M11-E5d-4b-1: Debug-only Exchange_sort Sort-state Adapter Shape

Status: coding completed；Code/Doc/Test Review Agent accepted after
documentation status corrections；`git diff --check`, `mysqld` build,
targeted MTR, and full `parallel_query` suite passed；committed as
`7aaaba10777`。

Goal:

- add a minimal `Exchange_sort`-owned scalar sort-state adapter shape；
- keep the adapter independent from `pq_optimizer.cc`, `Filesort`,
  `Sort_param`, worker MQ consumption, `Read()`, and ORDER BY eligibility；
- verify that the shape can be initialized and cleaned up under a dedicated
  DBUG flag without changing normal ORDER BY serial behavior。

Implementation summary:

- added `PQ_orderby_sort_state_shape` with scalar fields only:
  - worker count；
  - sort-order length；
  - max record length；
  - ref length；
  - stable-output flag；
  - index-sort flag；
  - rowid-required flag；
  - initialized flag；
- added private `Exchange_sort::init_sort_state_shape()` and
  `cleanup_sort_state_shape()`；
- added `Exchange_sort::run_orderby_sort_state_shape_smoke()` using synthetic
  scalar values only；
- wired the smoke from `Gather_operator::run_exchange_sort_smoke()` only under
  DBUG flag `pq_exchange_sort_state_shape_smoke`；
- added status counters:
  - `Parallel_exchange_sort_state_shape_smoke_attempts`；
  - `Parallel_exchange_sort_state_shape_smoke_success`；
  - `Parallel_exchange_sort_state_shape_smoke_unsupported`；
- extended `pq_commercial_order_by_frames` to assert:
  - no-DBUG counters remain zero；
  - DBUG smoke records one attempt and one success；
  - unsupported remains zero；
  - existing frame/merge/materialization smoke counters still pass。

Scope confirmation:

- no `sql/parallel_query/pq_optimizer.cc` edits；
- no `sql/filesort.*` or `sql/sort_param.*` edits；
- no `Filesort::make_sortorder()` visibility change；
- no persisted `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, handler
  pointer, or real `Sort_param` snapshot；
- no commercial `MQ_record_gather` import；
- no `Exchange_sort::init()` real-path override；
- no worker MQ consumption from default SQL；
- no `ParallelScanIterator::Read()` change；
- no optimizer eligibility, AccessPath, handler, or InnoDB change；
- no `filesort()` execution。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_saved_order_group_contract pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5d4b1_target \
  --tmpdir=/tmp/pqt_m11e5d4b1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4b1_full --tmpdir=/tmp/pqt_m11e5d4b1_full
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，5/5 including `shutdown_report`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-4b-1:

- first review verdict: `REVISE`；
- code findings: none blocking；
- documentation finding:
  - E5d-3 commit status was stale and has been corrected to `2665375de96`；
  - E5d-4b commit status has been corrected to `ea83b4c3c9c`；
- confirmed scalar-only state is respected；
- confirmed forbidden files/actions are respected；
- confirmed DBUG gate and MTR no-DBUG/DBUG assertions are correct；
- confirmed cleanup is explicit；
- confirmed counters, reset, `SHOW_VAR`, and `pq_stats` are consistent。
- re-review verdict: `ACCEPT`；
- commit: `7aaaba10777` Add PQ M11E exchange sort state shape。

### M11-E5d-4c: Optimizer-side Filesort/Sort_param Scalar Handoff Design

Status: design-only taskbook completed；Design Review Agent accepted；committed
as `01469aa124c`。

Goal:

- define the next safe boundary after E5d-4a and E5d-4b-1；
- pass only scalar metadata derived from the optimizer-side debug
  `Filesort` / stack-local `Sort_param` smoke into the `Exchange_sort`
  sort-state shape；
- keep the handoff debug-only and inside the existing `HAS_ORDER_BY`
  rejection window；
- do not persist `Filesort`, `Sort_param`, ORDER chain, TABLE, handler, or
  any optimizer object in `Exchange_sort` or PQ runtime state。

Design constraints:

- this is still not real ORDER BY PQ execution；
- `HAS_ORDER_BY` must continue to reject the query after the smoke；
- no `Filesort::make_sortorder()` visibility change；
- no real `Exchange_sort::init()`；
- no worker MQ consumption；
- no `ParallelScanIterator::Read()` integration；
- no AccessPath, handler, InnoDB, or ORDER BY eligibility relaxation。

Proposed data contract:

- add a tiny scalar carrier, for example `PQ_orderby_sort_state_shape_input`,
  with only:
  - `workers`；
  - `stable_output`；
  - `index_sort`；
  - `sort_order_length`；
  - `max_record_length`；
  - `ref_length`；
- the producer may derive `sort_order_length` and `max_record_length` from the
  E5d-4a debug `Filesort` / stack-local `Sort_param` smoke；
- `workers` must be synthetic or derived from session DOP as a scalar only；
- `ref_length` must be a conservative scalar, not a handler pointer；
- `Exchange_sort` must copy the values and immediately cleanup/reset during
  the smoke。

Proposed next coding step:

1. M11-E5d-4c-1 Debug-only Optimizer-to-Exchange_sort Scalar Handoff:
   - allowed files:
     - `sql/parallel_query/pq_optimizer.cc`；
     - `sql/parallel_query/exchange_sort.h`；
     - `sql/parallel_query/exchange_sort.cc`；
     - `sql/parallel_query/sql_parallel.h` and `sql/mysqld.cc` only if new
       counters are required；
     - focused MTR under `mysql-test/suite/parallel_query/`；
     - this task document and README；
   - allowed action:
     - under a dedicated DBUG flag, extend the existing E5d-4a restored
       sidecar + debug `Filesort` + stack-local `Sort_param` smoke to fill a
       scalar handoff input；
     - call an `Exchange_sort` helper that validates and copies only scalar
       metadata；
     - increment dedicated attempt/success/unsupported counters；
   - MTR must verify no-DBUG zero counters, DBUG success, `HAS_ORDER_BY`
     rejection, and zero `Parallel_queries_executed` /
     `Parallel_workers_launched` / `Parallel_ranges_dispatched` for the real
     ORDER BY negative guard。

Forbidden files / actions for E5d-4c-1:

- no `sql/filesort.*` or `sql/sort_param.*` edits；
- no `Filesort::make_sortorder()` visibility change；
- no persisted raw `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, or
  handler pointer；
- no persisted real `Sort_param` object or snapshot；
- no commercial `MQ_record_gather` import；
- no `Exchange_sort::init()` real-path override；
- no worker MQ consumption from default SQL；
- no `ParallelScanIterator::Read()` change；
- no AccessPath, handler, InnoDB, or ORDER BY eligibility change；
- no `filesort()` execution。

Required validation for E5d-4c-1:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by_frames pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5d4c1_target \
  --tmpdir=/tmp/pqt_m11e5d4c1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4c1_full --tmpdir=/tmp/pqt_m11e5d4c1_full
```

Design review request:

- confirm E5d-4c-1 may touch `pq_optimizer.cc` only for DBUG-only scalar
  extraction from the existing E5d-4a smoke；
- confirm the scalar handoff contract is sufficient and does not weaken
  lifetime boundaries；
- confirm `Exchange_sort` must still copy scalar values only and cleanup during
  the smoke；
- confirm real `Exchange_sort::init()`, MQ consumption, `Read()`, and ORDER BY
  eligibility remain separate follow-ups。

Design Review - M11-E5d-4c:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed `pq_optimizer.cc` may be touched only for DBUG-only scalar
  extraction inside the `HAS_ORDER_BY` reject window；
- confirmed scalar handoff contract is small enough and does not pass or
  persist `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, or handler；
- confirmed E5d-4b-1 `Exchange_sort` state remains scalar-only；
- confirmed forbidden scope is complete；
- confirmed validation commands and MTR negative guards are sufficient；
- approved entering E5d-4c-1 coding after design commit。

### M11-E5d-4c-1: Debug-only Optimizer-to-Exchange_sort Scalar Handoff

Status: coding completed；Code/Doc/Test Review Agent accepted；`git diff
--check`, `mysqld` build, targeted MTR, and full `parallel_query` suite passed；
committed as `45c1b4efd30`。

Goal:

- connect the optimizer-side E5d-4a debug `Filesort` / stack-local
  `Sort_param` smoke to the E5d-4b-1 `Exchange_sort` scalar shape；
- pass scalar metadata only；
- keep the handoff DBUG-only and inside the existing `HAS_ORDER_BY` rejection
  window。

Implementation summary:

- added public `Exchange_sort::run_orderby_sort_state_shape_handoff_smoke()`
  accepting only scalar parameters；
- kept `Exchange_sort` shape storage scalar-only and cleanup-scoped；
- added `pq_run_orderby_sort_state_handoff_smoke()` in `pq_optimizer.cc`:
  - repeats restored sidecar ORDER chain construction；
  - constructs debug-only `Filesort`；
  - initializes stack-local `Sort_param`；
  - extracts `workers`, `stable_output`, `index_sort`, `sort_order_length`,
    `max_record_length`, and `ref_length` as scalar values；
  - calls the `Exchange_sort` scalar handoff smoke；
- added DBUG flag `pq_orderby_sort_state_handoff_smoke`；
- added status counters:
  - `Parallel_orderby_sort_state_handoff_smoke_attempts`；
  - `Parallel_orderby_sort_state_handoff_smoke_success`；
  - `Parallel_orderby_sort_state_handoff_smoke_unsupported`；
- extended `pq_saved_order_group_contract` to verify:
  - no-DBUG handoff counters remain zero；
  - DBUG handoff smoke records one attempt and one success；
  - unsupported remains zero；
  - `HAS_ORDER_BY` rejection remains active；
  - `Parallel_queries_executed`, `Parallel_workers_launched`, and
    `Parallel_ranges_dispatched` remain zero。

Scope confirmation:

- no `sql/filesort.*` or `sql/sort_param.*` edits；
- no `Filesort::make_sortorder()` visibility change；
- no persisted raw `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, or
  handler pointer；
- no persisted real `Sort_param` object or snapshot；
- no commercial `MQ_record_gather` import；
- no `Exchange_sort::init()` real-path override；
- no worker MQ consumption from default SQL；
- no `ParallelScanIterator::Read()` change；
- no AccessPath, handler, InnoDB, or ORDER BY eligibility change；
- no `filesort()` execution。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by_frames pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5d4c1_target \
  --tmpdir=/tmp/pqt_m11e5d4c1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d4c1_full --tmpdir=/tmp/pqt_m11e5d4c1_full
```

Result:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed，5/5 including `shutdown_report`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-4c-1:

- Review Agent verdict: `ACCEPT`；
- commit: `4dc34748200` Add PQ M11E optimizer sort state handoff；
- findings: one low documentation finding，top status summary was stale and
  has been refreshed before commit；
- confirmed the handoff is DBUG-gated via
  `pq_orderby_sort_state_handoff_smoke`；
- confirmed the handoff is invoked only inside the `HAS_ORDER_BY` reject
  window and the query still rejects；
- confirmed only scalar values are passed to `Exchange_sort`；
- confirmed `Exchange_sort` remains scalar-only and cleanup-scoped；
- confirmed forbidden files/actions are not touched。

### M11-E5d-5: Real Exchange_sort Init / MQ / Read Boundary Design

Status: design completed；Design Review Agent accepted；committed as
`89dbb6143a5`。

Goal:

- choose the next safe split after E5d-4c-1 scalar handoff；
- define how current debug-only ORDER BY frame, Filesort/Sort_param scalar
  handoff, MQ, and `ParallelScanIterator::Read()` pieces should converge toward
  the commercial `Exchange_sort` path；
- avoid a single large patch that simultaneously changes Filesort ownership,
  MQ worker consumption, leader `Read()`, and user-visible ORDER BY eligibility。

Commercial reference path:

- `ParallelScanIterator::pq_make_filesort()` constructs or reuses a leader
  `Filesort` when merge sort or stable output is needed；
- `ParallelScanIterator::pq_init_record_gather()` creates `MQ_record_gather`
  and passes `Filesort *`, worker count, DESC groups, stable-output, and
  index-sort flags；
- `MQ_record_gather::mq_scan_init()` selects `Exchange_sort` when sort metadata
  exists, otherwise `Exchange_nosort`；
- commercial `Exchange_sort::init()` calls `Exchange::init()`, then mixes:
  `Filesort::make_sortorder()`, DESC group marking, `Sort_param` allocation and
  `init_for_filesort()`, compare-key buffers, rowid/ref-length checks,
  record-group allocation, and heap setup；
- commercial `MQ_record_gather::mq_scan_next()` delegates to
  `Exchange_sort::read_mq_record()`；`ParallelScanIterator::Read()` returns one
  row when `mq_scan_next()` succeeds and EOF otherwise。

Current branch facts:

- E5b-0/E5b-1/E5b-2/E5b-3 introduced an ORDER BY frame contract, controlled
  K-way merge, empty/ERROR edges, and debug-only row materialization smoke；
- E5d-S1/S2/S3 and E5d-2a/2b/2c/2d introduced saved ORDER/GROUP and owned
  sidecar/clone-copy contracts；
- E5d-3a/E5d-3c can construct a restored-order debug `Filesort` in the
  `HAS_ORDER_BY` reject window；
- E5d-4a initializes a stack-local `Sort_param` for that debug `Filesort`；
- E5d-4b-1 and E5d-4c-1 hand scalar Filesort/Sort_param metadata to
  `Exchange_sort` without persisting raw optimizer pointers；
- current `Exchange_sort` still has no persistent real `Sort_param`, no real
  `make_sortorder()` visibility change, and no default worker MQ consumption；
- current `ParallelScanIterator::Read()` consumes `Exchange_nosort`
  materialized record images only；ORDER BY remains rejected by `HAS_ORDER_BY`。

Explorer confirmations:

- Commercial Path Explorer confirmed `Exchange_sort::init()` is not a small
  constructor-equivalent step: it combines base MQ init, `make_sortorder()`,
  DESC marking, `Sort_param::init_for_filesort()`, compare-key buffers,
  rowid/ref-length validation, per-worker record groups, and heap setup；
- Commercial Path Explorer confirmed ordered `Read()` is thin only after
  `MQ_record_gather` and `Exchange_sort` are fully initialized: leader
  `Read()` calls `mq_scan_next()`, which delegates to
  `Exchange_sort::read_mq_record()` and writes the min heap record into
  `table->record[0]`；
- Current Branch Explorer confirmed current `Exchange_sort::read_mq_message()`
  is still a FINISH/no-data stub and all ORDER BY behavior is debug/smoke
  scoped；
- Current Branch Explorer confirmed `PQOF` ORDER BY frames and `PQWR`
  worker-result frames must remain separate；do not mix SELECT-list worker
  result frames with ORDER BY row/sort-key frames；
- both explorers recommend keeping the next step to state/init shape,
  ordered MQ/rowid frame contract, and Read boundary adapter before any
  user-visible ORDER BY eligibility。

Design decision:

- do not implement commercial `Exchange_sort::init()` in one patch；
- keep `HAS_ORDER_BY` rejection and all user-visible ORDER BY PQ disabled；
- split the real-path migration into small reviewed subtasks that each prove one
  ownership/lifetime boundary。

Proposed follow-up split:

1. M11-E5d-5a: `Exchange_sort` real-init state owner shape.
   - compile/debug-only owner struct for scalar sort metadata plus owned buffer
     slots needed by commercial init；
   - no persisted `Filesort *`, `Sort_param *`, `ORDER *`, `TABLE *`, or
     handler pointer；
   - no `Filesort::make_sortorder()` visibility change；
   - validation: debug smoke proves init/cleanup/reset and full suite passes。

2. M11-E5d-5b: sort-key buffer and record-group allocation smoke.
   - allocate compare-key buffers and worker record-group containers from
     `Exchange_sort` owned state using scalar lengths from E5d-4c-1；
   - verify cleanup on success/error and stable rowid-required shape；
   - no MQ receive from normal worker path。

3. M11-E5d-5c: controlled ORDER BY MQ-to-record-group loader.
   - consume only dedicated E5b ORDER BY frames from controlled local MQ；
   - deep-copy record image, rowid, and optional prebuilt sort key into
     `Exchange_sort` owned record groups；
   - handle FINISH/ERROR/WOULD_BLOCK explicitly；
   - existing PQWR worker-result frames must remain unchanged。

4. M11-E5d-5d: debug-only ordered leader Read shadow path.
   - add a DBUG-only shadow helper that lets `ParallelScanIterator` consume an
     already-populated `Exchange_sort` in an isolated smoke；
   - do not route default SQL or normal ORDER BY queries to it；
   - preserve existing `Exchange_nosort` `Read()` behavior and counters。

5. M11-E5d-5e: user-visible ORDER BY eligibility gate design.
   - design only, after 5a-5d pass；
   - define the first visible subset and negative tests；
   - likely limit to single-table, ASC, non-nullable/simple fields, no LIMIT
     pushdown, no GROUP BY/DISTINCT/HAVING/window, and no DESC until explicit
     tests exist。

Allowed files for M11-E5d-5 design only:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Allowed files for future coding after design review:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/exchange.h` / `.cc` only for reusable typed-MQ status
  helpers；
- `sql/parallel_query/sql_parallel.h` / `.cc` only for DBUG counters and smoke
  orchestration；
- `mysql-test/suite/parallel_query/` focused MTRs；
- M11 taskbook and progress README。

Forbidden until a later reviewed gate:

- `sql/filesort.*` and `sql/sort_param.*` visibility or ownership changes；
- persistent raw `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, `handler *`
  inside `Exchange_sort` real state；
- commercial `MQ_record_gather` wholesale import；
- default worker MQ ORDER BY consumption；
- default `ParallelScanIterator::Read()` ORDER BY path；
- optimizer eligibility, AccessPath, handler, or InnoDB changes；
- user-visible `HAS_ORDER_BY` acceptance；
- `filesort()` execution inside PQ ORDER BY path。

Required validation for future coding subtasks:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_saved_order_group_contract \
  pq_commercial_order_by_frames pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5d5_target \
  --tmpdir=/tmp/pqt_m11e5d5_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5_full --tmpdir=/tmp/pqt_m11e5d5_full
```

Design review request:

- confirm the 5a-5e split is small enough and keeps risky ownership/lifetime
  boundaries serial；
- confirm 5a should still avoid persistent raw optimizer/executor pointers；
- confirm 5b/5c should prove allocation and controlled MQ consumption before
  any `Read()` bridge；
- confirm 5d must remain DBUG-only and should not change default
  `ParallelScanIterator::Read()` behavior；
- confirm user-visible ORDER BY eligibility should remain blocked until 5e
  design review。

Design Review - M11-E5d-5:

- Review Agent verdict: `ACCEPT`；
- findings: none after status refresh；
- confirmed the 5a-5e split is safe and serializes risky ownership/lifetime
  boundaries；
- confirmed 5a must avoid persistent raw optimizer/executor pointers；
- confirmed 5b/5c should prove allocation and controlled MQ consumption before
  any `Read()` bridge；
- confirmed 5d must remain DBUG-only and must not change default
  `ParallelScanIterator::Read()` behavior；
- confirmed user-visible ORDER BY eligibility remains blocked until a later
  gate design review。

### M11-E5d-5a: Exchange_sort Real-init State Owner Shape

Status: coding completed；Code/Doc/Test Review Agent accepted；`git diff
--check`, `mysqld` build, targeted MTR, and full `parallel_query` suite passed；
committed as `83d91fed0f5`。

Goal:

- add a compile/debug-only owned state shape inside `Exchange_sort` for the
  future commercial init path；
- keep the state scalar-only and cleanup-scoped；
- do not persist raw `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, or
  handler pointers；
- do not change real `Filesort::make_sortorder()`, MQ consumption,
  `ParallelScanIterator::Read()`, or ORDER BY eligibility。

Implementation summary:

- added `PQ_orderby_real_init_state_shape` with scalar fields for workers,
  sort-order length, record/ref lengths, planned compare-key/tmp-key buffer
  lengths, min-record slots, record-group slots, stable-output, index-sort, and
  rowid-required flags；
- added `Exchange_sort::init_real_init_state_owner_shape()` and
  `cleanup_real_init_state_owner_shape()`；
- extended `Exchange_sort::run_orderby_sort_state_shape_smoke()` to also run
  `run_orderby_real_init_state_owner_smoke()` under the existing
  `pq_exchange_sort_state_shape_smoke` DBUG entry；
- reused existing
  `Parallel_exchange_sort_state_shape_smoke_attempts/success/unsupported`
  counters, so no new status variables are introduced。

Scope confirmation:

- touched only `sql/parallel_query/exchange_sort.h`,
  `sql/parallel_query/exchange_sort.cc`, this taskbook, and README；
- no optimizer, executor, AccessPath, handler, InnoDB, `filesort.*`, or
  `sort_param.*` changes；
- no persistent raw optimizer/executor/storage pointers；
- no default worker MQ ORDER BY consumption；
- no default ordered `ParallelScanIterator::Read()` path；
- no user-visible `HAS_ORDER_BY` acceptance；
- no `filesort()` execution。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5a_target --tmpdir=/tmp/pqt_m11e5d5a_target
```

Result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5a:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `PQ_orderby_real_init_state_shape` is scalar-only and stores no
  `ORDER *`, `Filesort *`, `Sort_param *`, `TABLE *`, handler, or storage
  pointer；
- confirmed init/cleanup/reset smoke is wired only through existing
  `pq_exchange_sort_state_shape_smoke` and default SQL behavior is unchanged；
- confirmed no `Filesort::make_sortorder()`, `Sort_param`, eligibility,
  AccessPath, handler/InnoDB, worker MQ consumption, or
  `ParallelScanIterator::Read()` changes；
- confirmed no MTR/result update is needed because the new owner shape is
  internal, DBUG-only, and reuses existing smoke counters/result assertions。

### M11-E5d-5b: Sort-key Buffer / Record-group Allocation Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；`git diff
--check`, `mysqld` build, targeted MTR, and full `parallel_query` suite passed；
waiting for commit。

Goal:

- allocate controlled, owned buffer/container placeholders from the 5a scalar
  real-init state；
- prove cleanup resets those owned allocations；
- keep the smoke local to `Exchange_sort` and the existing
  `pq_exchange_sort_state_shape_smoke` DBUG entry；
- do not consume real worker MQ frames and do not connect to
  `ParallelScanIterator::Read()`。

Implementation requirements:

- add owned vectors for two compare-key buffers and one temporary key buffer；
- resize `m_min_records` and `m_record_groups` according to the 5a worker
  count；
- buffer lengths must come from 5a shape:
  `compare_key_buffer_length = max_record_length + 1`,
  `tmp_key_buffer_length = ref_length` only when stable output is required；
- the smoke must verify:
  - allocation succeeds for stable-output shape；
  - compare-key buffers have expected lengths；
  - tmp-key buffer has expected rowid/ref length；
  - min-record and record-group slots match workers；
  - cleanup clears buffers, groups, and the real-init owner shape；
- invalid shapes must fail closed and leave no partial owned state。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden files/actions:

- no `sql/filesort.*` or `sql/sort_param.*` changes；
- no optimizer, executor, AccessPath, handler, or InnoDB changes；
- no persistent raw optimizer/executor/storage pointers；
- no real `Filesort::make_sortorder()` visibility or execution；
- no default worker MQ ORDER BY consumption；
- no default `ParallelScanIterator::Read()` ordered path；
- no user-visible `HAS_ORDER_BY` acceptance；
- no new status variables unless review finds existing smoke counters
  insufficient。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5b_target --tmpdir=/tmp/pqt_m11e5d5b_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5b_full --tmpdir=/tmp/pqt_m11e5d5b_full
```

Implementation summary:

- added owned compare-key buffers and a tmp-key buffer to `Exchange_sort`；
- added `Exchange_sort::allocate_real_init_buffers_shape()`；
- extended `run_orderby_sort_state_shape_smoke()` to run the 5b allocation
  smoke after the 5a owner-shape smoke；
- `run_orderby_real_init_allocation_smoke()` verifies expected buffer lengths,
  worker slot counts, and cleanup reset；
- reused the existing `pq_exchange_sort_state_shape_smoke` DBUG entry and
  `Parallel_exchange_sort_state_shape_smoke_*` counters。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review follow-up:

- added explicit 5a shape self-consistency checks before owned allocation；
- added an invalid-shape fail-closed smoke that corrupts
  `compare_key_buffer_length` and verifies allocation fails with owned buffers
  and containers still empty；
- re-ran `git diff --check`, `mysqld` build, and targeted MTR after the fix；
  all passed。
- re-ran full `parallel_query` suite after the fix；89/89 passed including
  `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5b:

- Review Agent verdict: `ACCEPT`；
- findings: none after requested changes；
- confirmed `allocate_real_init_buffers_shape()` pre-clears owned state and
  validates buffer length self-consistency plus `UINT32_MAX`；
- confirmed invalid-shape smoke fails closed with empty owned buffers and
  containers；
- confirmed the change remains scoped to the four allowed files and runs only
  through the existing `pq_exchange_sort_state_shape_smoke` DBUG path；
- confirmed no MTR/result update is needed because existing
  `pq_commercial_order_by_frames` toggles the DBUG entry and detailed buffer
  assertions live inside the C++ smoke。

### M11-E5d-5c: Controlled ORDER BY MQ-to-record-group Loader Design

Status: coding completed；`git diff --check`, `mysqld` build, targeted MTR,
and full `parallel_query` suite passed；Code/Doc/Test Review Agent accepted；
committed as `4163db43197`。

Goal:

- introduce the next boundary after 5b allocation: controlled loader from
  dedicated `PQOF` ORDER BY frames into `Exchange_sort` owned record groups；
- keep the loader local/debug-only and disconnected from normal worker MQ
  production；
- prove frame decode, deep-copy, per-worker FINISH, ERROR, and WOULD_BLOCK
  handling before any ordered `Read()` bridge。

Design constraints:

- use only existing `PQ_orderby_frame_header` / `PQ_orderby_decoded_frame`
  format；
- do not mix or reinterpret `PQWR` worker-result frames；
- loader may read only from controlled local `MQueue_handle` instances created
  inside a smoke；
- loader output is `m_record_groups[worker_id].records`, using owned
  `std::vector` storage already present in `PQ_orderby_cached_record`；
- record image, rowid, and sort key must be deep-copied；
- FINISH marks only that worker's group complete；
- ERROR returns fail-closed and leaves default SQL unaffected；
- WOULD_BLOCK must be observable as a loader status, not converted to EOF；
- no user-visible result is materialized in this phase。

Proposed implementation shape:

- add an internal loader status enum scoped to `Exchange_sort`, for example:
  `ROW`, `FINISH`, `WOULD_BLOCK`, `ERROR`；
- add a helper that reads one controlled ORDER BY frame from a specific
  `MQueue_handle` and updates one `PQ_orderby_record_batch`；
- add a smoke that:
  - initializes 5a/5b owned state for 3 workers；
  - sends controlled `PQOF` ROW/FINISH frames through local MQ handles；
  - verifies row image, rowid, and sort key bytes are deep-copied into the
    expected worker record groups；
  - verifies one empty/FINISH worker；
  - verifies an ERROR frame fails closed；
  - verifies a no-message read returns WOULD_BLOCK without mutating groups；
  - verifies cleanup clears groups and buffers。

Allowed files for future coding:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden files/actions:

- no `Query_result_mq` frame format changes；
- no `PQWR` frame reuse；
- no `sql/filesort.*` or `sql/sort_param.*` changes；
- no `Sort_param` construction, use, or integration from `Exchange_sort` or any
  other 5c code path；
- no optimizer, executor, AccessPath, handler, or InnoDB changes；
- no persistent raw optimizer/executor/storage pointers；
- no real `Filesort::make_sortorder()` visibility or execution；
- no default worker MQ ORDER BY consumption；
- no default `ParallelScanIterator::Read()` ordered path；
- no user-visible `HAS_ORDER_BY` acceptance；
- no visible row materialization from this loader。

Validation for future coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5c_target --tmpdir=/tmp/pqt_m11e5d5c_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5c_full --tmpdir=/tmp/pqt_m11e5d5c_full
```

Required validation expectations:

- real ORDER BY SQL still reports `Not parallel HAS_ORDER_BY`；
- `Parallel_queries_executed`, `Parallel_workers_launched`, and
  `Parallel_ranges_dispatched` must not increase for ORDER BY negative tests；
- no default ordered `ParallelScanIterator::Read()` path is selected；
- controlled loader smoke must be observable only through the dedicated DBUG
  path and existing ORDER BY frame smoke counters。

Design review request:

- confirm this phase may read controlled local `PQOF` frames but must not
  consume default worker MQ；
- confirm `PQWR` and `PQOF` frame formats must remain separate；
- confirm WOULD_BLOCK needs an explicit status before any `Read()` bridge；
- confirm visible row materialization and ordered `Read()` stay deferred to
  later phases。

Design Review - M11-E5d-5c:

- Review Agent verdict: `ACCEPT`；
- findings: none after requested changes；
- confirmed 5c is small and codable: controlled local `PQOF` only, deep-copy
  into owned record groups, and ROW/FINISH/ERROR/WOULD_BLOCK covered；
- confirmed `PQWR` / `Query_result_mq` separation is clear；
- confirmed `Sort_param` construction/use/integration is explicitly forbidden；
- confirmed validation requires `Not parallel HAS_ORDER_BY`, no
  executed/workers/ranges growth, no default ordered
  `ParallelScanIterator::Read()`, and loader observability only through DBUG /
  frame-smoke counters。

Implementation summary:

- added `PQ_orderby_loader_status` with `ROW`, `FINISH`, `WOULD_BLOCK`, and
  `ERROR` states；
- added `Exchange_sort::load_orderby_frame_to_record_group()`；
- added `Exchange_sort::run_orderby_frame_loader_smoke()` and chained it after
  the 5a/5b state-shape/allocation smokes；
- the smoke initializes controlled local MQ handles, sends `PQOF` ROW/FINISH
  and ERROR frames, deep-copies row image / rowid / sort key bytes into owned
  record groups, verifies empty FINISH worker behavior, verifies ERROR, verifies
  WOULD_BLOCK on an empty controlled queue, and verifies cleanup clears buffers
  and groups；
- no new status variables were added；the loader remains covered by the
  existing `pq_exchange_sort_state_shape_smoke` DBUG entry。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`。
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5c:

- Review Agent verdict: `ACCEPT`；
- findings: none after follow-up fix；
- confirmed `MQ_DETACHED` is fail-closed as `ERROR` and does not mark a worker
  group complete；
- confirmed ROW deep-copies row image, rowid, and sort key into owned
  `PQ_orderby_cached_record` vectors；
- confirmed FINISH only marks the target worker complete and WOULD_BLOCK stays
  distinct from EOF；
- confirmed the smoke covers controlled local MQ only, including empty FINISH
  worker, `PQOF` ERROR frame, WOULD_BLOCK, detach fail-closed, and cleanup
  reset；
- confirmed the diff does not open real ORDER BY eligibility, default worker MQ
  consumption, ordered `ParallelScanIterator::Read()`, `Filesort` /
  `Sort_param`, optimizer, AccessPath, handler, or InnoDB behavior。

### M11-E5d-5d: Debug-only Ordered Leader Read Shadow Path Design

Status: coding completed；`git diff --check`, `mysqld` build, targeted MTR,
and full `parallel_query` suite passed；Code/Doc/Test Review Agent accepted；
committed as `06b68f558d3`。

Goal:

- define the smallest post-5c bridge from controlled `Exchange_sort` record
  groups to a leader-read-like state machine；
- keep the bridge DBUG/smoke-only and disconnected from user-visible ORDER BY
  SQL；
- prove explicit `ROW` / `EOF` / `ERROR` ordered-read status boundaries before
  any default `ParallelScanIterator::Read()` branch or eligibility gate is
  opened；
- preserve the current `Exchange_nosort` `PQRM` row-image protocol and ordinary
  PQ row path unchanged。

Current branch facts:

- `ParallelScanIterator::Read()` currently consumes only `Exchange_nosort` via
  `materialize_next_record_image_status()`；
- the current `ORDER_GATHER_VALIDATED` lifecycle branch only cleans up and
  returns EOF；it does not produce ordered rows；
- `Exchange_sort::read_mq_message()` is still a FINISH/no-data stub and must not
  be treated as a real ordered reader；
- `Exchange_sort::read_ordered_record_shape()` currently only checks that order
  gather shape was initialized；
- 5c added controlled `PQOF` frame loading into owned `m_record_groups` but did
  not add a leader `Read()` bridge；
- `PQOF`, `PQWR`, and `PQRM` remain separate frame/protocol families。

Commercial reference facts:

- commercial ordered flow is
  `ParallelScanIterator::Read()` -> `MQ_record_gather::mq_scan_next()` ->
  `Exchange_sort::read_mq_record()`；
- commercial `Exchange_sort::read_mq_record()` eventually writes the selected
  ordered row into leader `table->record[0]`；
- the commercial bool return shape folds EOF/error diagnosis through surrounding
  thread/error state, which is too broad for the current branch's debug shadow
  boundary；
- the commercial path depends on full `Filesort` / `Sort_param` lifecycle,
  `make_sortorder()`, worker row serialization, rowid tie-break, heap state,
  queue detach handling, and worker manager diagnostics, so it must not be
  copied as one 5d patch。

Design shape for future coding:

- add a local ordered-read shadow status, for example `ROW`, `EOF_REACHED`, and
  `ERROR`；
- add an `Exchange_sort` helper that consumes already-loaded owned record groups
  and returns exactly one shadow status per call；
- the helper may copy a cached row image into a caller-owned debug buffer, but
  must not write `TABLE::record[0]` unless a later reviewed task explicitly
  designs that step；
- the helper must treat incomplete groups with no available row as `ERROR` or a
  separate debug-only blocked state only if that state is explicitly tested；
- the first coding step should use fully completed controlled groups so EOF is
  deterministic；
- add a smoke that loads controlled `PQOF` frames through the 5c loader, then
  drains them through the ordered-read shadow helper and verifies:
  - rows are returned in the expected deterministic order for the debug data；
  - EOF is returned only after all completed groups are drained；
  - ERROR is returned for incomplete or invalid group state；
  - cleanup clears owned groups and buffers；
  - no ordinary SQL counter indicates visible ORDER BY execution。

Recommended coding scope:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.cc` only if smoke orchestration needs a new
  DBUG-gated call site；
- focused MTR under `mysql-test/suite/parallel_query/` only if existing
  `pq_commercial_order_by_frames` cannot observe the new smoke；
- M11 taskbook and progress README。

Forbidden for 5d coding:

- no default `ParallelScanIterator::Read()` ORDER BY branch；
- no user-visible `HAS_ORDER_BY` acceptance；
- no `Filesort::make_sortorder()` visibility change or persistent
  `Filesort` / `Sort_param` ownership；
- no `TABLE::record[0]` writes from the ordered shadow helper；
- no changes to `Exchange_nosort`, `PQRM`, `PQWR`, `Query_result_mq`, or
  `msg_queue` semantics；
- no optimizer, AccessPath, handler, InnoDB, sysvar, or default SQL behavior
  changes；
- no claim that visible ORDER BY result correctness is supported。

Validation for future coding:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5d_target --tmpdir=/tmp/pqt_m11e5d5d_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5d_full --tmpdir=/tmp/pqt_m11e5d5d_full
```

Required validation expectations:

- real ORDER BY SQL still reports `Not parallel HAS_ORDER_BY`；
- ORDER BY negative tests do not increase executed/workers/ranges counters；
- default `ParallelScanIterator::Read()` still consumes only `Exchange_nosort`
  materialized row images；
- ordered shadow status is visible only through a DBUG/smoke path。

Design review request:

- confirm 5d should use explicit `ROW` / `EOF` / `ERROR` shadow status instead
  of copying the commercial bool-return boundary；
- confirm no `TABLE::record[0]` writes are allowed in 5d；
- confirm `PQOF` controlled groups must remain separate from `PQWR` and `PQRM`
  worker-result paths；
- confirm visible ORDER BY eligibility and default ordered
  `ParallelScanIterator::Read()` remain blocked until later review。

Design Review - M11-E5d-5d:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed 5d is debug/smoke-only and blocks visible ORDER BY eligibility plus
  default ordered `ParallelScanIterator::Read()` activation；
- confirmed `PQOF` / `PQWR` / `PQRM` separation is clear；
- confirmed explicit `ROW` / `EOF_REACHED` / `ERROR` shadow status is the right
  boundary for the current branch；
- confirmed the ban on `TABLE::record[0]` writes is appropriate for 5d and does
  not conflict with later migration because that step requires a separate
  reviewed task；
- accepted kill/WOULD_BLOCK residual risk because the first coding step is
  constrained to fully completed controlled groups and any blocked state must be
  explicit and tested。

Implementation summary:

- added `PQ_orderby_shadow_read_status` with `ROW`, `EOF_REACHED`, and `ERROR`
  states；
- added `Exchange_sort::read_ordered_record_shadow_shape()`；
- added `Exchange_sort::run_orderby_shadow_read_smoke()` and chained it after
  the 5a/5b/5c state-shape, allocation, and frame-loader smokes；
- the shadow helper drains already-loaded owned `m_record_groups`, chooses the
  next debug row by int64 sort-key with worker-id tie break, and copies only the
  row image into a caller-owned `std::vector<uchar>`；
- the helper does not write `TABLE::record[0]`, does not call
  `Exchange_sort::read_mq_message()`, does not consume default worker MQ, and
  does not touch `Exchange_nosort`, `PQRM`, or `PQWR`；
- the smoke verifies deterministic ROW order, EOF after all completed groups are
  drained, ERROR for empty incomplete group state, ERROR for loaded ROW without
  FINISH, ERROR for invalid sort-key shape, and cleanup reset。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`；
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5d:

- Review Agent verdict: `ACCEPT` after one required fix；
- first review found that a group with loaded ROW but no FINISH could be emitted
  because incomplete state was checked only after `next_pos >= records.size()`；
- fixed by making `read_ordered_record_shadow_shape()` reject any
  `!batch.completed` before considering rows；
- added loaded ROW without FINISH smoke coverage, in addition to empty
  incomplete group coverage；
- confirmed the helper copies only into a caller-owned buffer and does not write
  `TABLE::record[0]`；
- confirmed scope remains contained to `Exchange_sort` plus docs, with no
  `Exchange_nosort`, `Query_result_mq`, `PQWR`, `PQRM`, optimizer eligibility,
  AccessPath, handler, or InnoDB changes。

### M11-E5d-5e: User-visible ORDER BY Eligibility Gate Design

Status: design completed；Design Review Agent accepted；committed as
`2e290b3dee0`；no code edits in this subtask。

Goal:

- design the first user-visible ORDER BY eligibility gate after 5a-5d state,
  MQ-loader, and shadow-read smokes；
- avoid turning on visible ORDER BY execution in this design step；
- separate "ORDER BY shape recognized as a future candidate" from "ORDER BY
  currently executable by PQ"；
- keep all existing `HAS_ORDER_BY` negative MTRs valid until a later reviewed
  coding task wires real execution。

Current branch facts:

- `pq_check_query_block_eligible()` still rejects `query_block->is_ordered()`
  with `PQUnsuiteReason::HAS_ORDER_BY`；
- the `HAS_ORDER_BY` reject window also hosts DBUG-only saved ORDER/GROUP,
  Filesort, Sort_param, and scalar handoff smokes；
- existing `pq_commercial_order_by`, `pq_not_support`,
  `pq_explain_fallback`, `pq_saved_order_group_contract`, and
  `pq_commercial_ref_icp` tests assert ORDER BY remains serial and does not
  increase executed/workers/ranges counters；
- 5a-5d proved owned state shape, buffer allocation, controlled `PQOF` loader,
  and debug-only shadow read status, but did not implement real worker ORDER BY
  frame production, persistent `Filesort` / `Sort_param`, leader table
  materialization, default ordered `Read()`, or rowid tie-break。

Commercial reference facts:

- commercial ORDER BY eligibility is not a single gate; it is decided across
  resolver, optimizer, leader plan construction, gather creation, and
  `Exchange_sort` initialization；
- commercial supports broader cases including multi-table shapes, index-order
  paths, DESC groups, LIMIT with ORDER BY, range/ref/ICP, and some GROUP BY /
  HAVING / DISTINCT paths, but only because the full Filesort/Sort_param,
  worker producer, heap merge, rowid, leader materialization, and diagnostics
  pipeline is complete；
- current branch must not copy that broad gate before those execution pieces
  are real rather than smoke-only。

5e design decision:

- 5e remains design-only；
- the next coding step should create an explicit ORDER BY eligibility contract
  helper that can classify a query as:
  - unsupported ORDER BY shape；
  - future visible ORDER BY candidate shape；
  - executable visible ORDER BY PQ shape；
- for the first coding step, `future visible ORDER BY candidate shape` must
  still reject execution, preserve serial behavior, and keep current MTR
  counter expectations；
- `executable visible ORDER BY PQ shape` must remain unreachable until a later
  task wires real Filesort/Sort_param lifetime, worker frame production,
  `Exchange_sort` heap read, leader materialization, and default ordered
  `ParallelScanIterator::Read()`。

First visible-candidate subset:

- single query block；
- `SQLCOM_SELECT` only；
- one InnoDB base table；
- non-temporary, non-partitioned table；
- traditional optimizer only；
- `JT_ALL` clustered full table scan only；
- ORDER BY items are simple base fields already safely resolvable from the
  selected table；
- filesort path only；no ordered-index skip-sort；
- ASC-only；
- no LIMIT/OFFSET；
- WHERE predicates limited to shapes already accepted by the current full-scan
  PQ gate；
- existing cost/DOP/full-scan gates must still pass。

Must remain disabled:

- user-visible execution for every ORDER BY shape in 5e；
- multi-table join；
- DISTINCT；
- explicit GROUP BY, ROLLUP, GROUP BY with ORDER BY, and partial aggregation
  combinations；
- HAVING；
- window functions；
- UNION / INTERSECT / EXCEPT / subquery wrappers / derived tables / views /
  CTEs；
- ORDER BY expression, function, ambiguous alias, or correlated subquery；
- DESC or mixed ASC/DESC；
- LIMIT/OFFSET；
- index-order / skip-filesort ORDER BY；
- secondary index, range, ref, dependent ref, ICP, MRR, reverse scan, backward
  index scan；
- partitioned table, MVI, BLOB/record-buffer-hostile row shapes, MIN shortcut；
- non-InnoDB, temp table, locking read, SERIALIZABLE, PS/SP/trigger or
  attachable transaction restricted cases。

Proposed follow-up split:

1. M11-E5d-5e-1: eligibility contract helper and negative matrix.
   - coding task may add a helper and debug/MTR diagnostics that identify the
     first visible-candidate subset；
   - it must still return serial / disabled, not executable PQ；
   - it must preserve the existing `HAS_ORDER_BY` tests or replace them only
     with an equally strict "candidate but execution disabled" diagnostic and
     zero executed/workers/ranges deltas。

2. M11-E5d-5e-2: execution preflight blocker.
   - coding task may add a central fail-closed blocker proving all real
     prerequisites are absent or present；
   - it must keep `executable visible ORDER BY PQ` unreachable until real
     execution pieces are complete。

3. Later reviewed task: first executable ORDER BY PQ subset.
   - only after persistent Filesort/Sort_param, worker frame producer,
     `Exchange_sort` heap read, leader materialization, rowid tie-break, kill /
     detach / error diagnostics, and full MTR expected-result coverage are
     implemented。

Required tests for future coding:

- keep existing negative ORDER BY tests green；
- add/adjust a focused MTR for the first candidate subset that verifies
  recognized-but-disabled diagnostics and zero executed/workers/ranges deltas；
- retain `pq_saved_order_group_contract` DBUG smoke coverage in the
  `HAS_ORDER_BY` or replacement reject window；
- retain negative cases for ORDER BY LIMIT, DESC, GROUP BY + ORDER BY, ref,
  range, ICP, and reverse index scan。

Design review request:

- confirm 5e should remain design-only；
- confirm the first visible-candidate subset is intentionally narrower than
  commercial；
- confirm coding should first add a recognized-but-execution-disabled contract,
  not executable ORDER BY PQ；
- confirm existing ORDER BY negative MTRs must stay valid or be replaced by
  stricter disabled diagnostics with zero runtime counters。

Design Review - M11-E5d-5e:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed 5e remains design-only and does not enable visible ORDER BY
  execution；
- confirmed unsupported / future-candidate / executable-PQ classification is
  the right layering, with executable ORDER BY kept unreachable；
- confirmed the first candidate subset is conservative enough for a disabled
  recognition step；
- confirmed existing `HAS_ORDER_BY` behavior must be preserved or replaced only
  by stricter disabled diagnostics with zero runtime counters；
- confirmed the 5e-1 / 5e-2 split is executable and fail-closed；
- residual field-type, nullability, and collation details for "simple base
  fields" are deferred to 5e-1 because this design step still disables
  execution。

### M11-E5d-5e-1: ORDER BY Eligibility Contract Helper

Status: coding completed；`git diff --check`, `mysqld` build, targeted MTR,
and full `parallel_query` suite passed；Code/Doc/Test Review Agent accepted；
committed as `f54474bda65`。

Goal:

- add the first ORDER BY eligibility contract helper；
- recognize the first future visible ORDER BY candidate subset；
- keep execution disabled and preserve the existing `HAS_ORDER_BY` serial
  boundary；
- expose focused diagnostics through DBUG and status counters so the contract
  is testable without opening visible ORDER BY PQ。

Implementation summary:

- added `PQOrderByEligibilityContractStatus` and
  `PQOrderByEligibilityContract`；
- added `pq_build_orderby_eligibility_contract()`；
- the first future-candidate subset requires SELECT, traditional optimizer,
  simple query block, one table, simple ORDER BY, ASC-only, no LIMIT, no
  DISTINCT/GROUP/HAVING/window, filesort path, and `JT_ALL` full table scan；
- candidate status is
  `FUTURE_CANDIDATE_EXECUTION_DISABLED` and always keeps
  `execution_disabled=true`；
- added DBUG smoke `pq_orderby_eligibility_contract_smoke` in the existing
  `HAS_ORDER_BY` reject window；
- added status counters:
  - `Parallel_orderby_eligibility_contract_attempts`；
  - `Parallel_orderby_eligibility_contract_candidate_disabled`；
  - `Parallel_orderby_eligibility_contract_unsupported`；
- updated `pq_commercial_order_by` to verify a no-LIMIT ASC candidate is
  recognized as candidate-disabled while `EXPLAIN` still reports
  `Not parallel HAS_ORDER_BY`；
- updated `pq_stats` for the three new status variables。

Validation result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by pq_stats pq_saved_order_group_contract`；
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5e-1:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed helper is fail-closed and only the full positive predicate reaches
  `FUTURE_CANDIDATE_EXECUTION_DISABLED`；
- confirmed DBUG smoke is read-only and remains inside the existing
  `HAS_ORDER_BY` reject path before the unchanged reject return；
- confirmed status exposure/reset is complete through `PQ_global_stats`, show
  functions, and `SHOW_VAR` entries；
- confirmed MTR proves the positive candidate-disabled path and that
  `EXPLAIN` / runtime remain serial；
- residual risk: LIMIT/DESC/GROUP/window and other unsupported shapes rely on
  fail-closed predicate review plus existing negative ORDER BY tests rather
  than separate per-shape DBUG assertions。

### M11-E5d-5e-2: ORDER BY Execution Preflight Blocker

Status: coding completed；`git diff --check`, `mysqld` build, targeted MTR,
and full `parallel_query` suite passed；Code/Doc/Test Review Agent accepted；
committed as `68b27d804ca`。

Goal:

- add a central fail-closed preflight blocker between the 5e-1 future ORDER BY
  candidate contract and any user-visible ORDER BY PQ execution；
- prove all real execution prerequisites remain absent from the default path；
- keep executable visible ORDER BY PQ unreachable and preserve the existing
  `HAS_ORDER_BY` serial boundary。

Implementation summary:

- added `PQOrderByExecutionPreflightStatus` and
  `PQOrderByExecutionPreflight`；
- added `pq_build_orderby_execution_preflight()`；
- the preflight first requires the 5e-1 eligibility helper to classify the
  query as `FUTURE_CANDIDATE_EXECUTION_DISABLED`；
- the preflight then records the current prerequisite contract as not ready:
  Filesort runtime, Sort_param runtime, worker ORDER BY frame producer,
  `Exchange_sort` heap read, leader materialization, rowid tie-break, default
  ordered `Read()`, and kill/detach/error diagnostics；
- status remains `BLOCKED_EXECUTION_DISABLED` and the helper returns false；
- added DBUG smoke `pq_orderby_execution_preflight_smoke` inside the existing
  `HAS_ORDER_BY` reject window；
- added status counters:
  - `Parallel_orderby_execution_preflight_attempts`；
  - `Parallel_orderby_execution_preflight_blocked`；
  - `Parallel_orderby_execution_preflight_ready`；
- updated `pq_commercial_order_by` to verify the first no-LIMIT ASC candidate
  is recognized by 5e-1 and blocked by 5e-2 while `ready` remains zero；
- updated `pq_stats` for the three new status variables。

Scope confirmation:

- no optimizer acceptance change；
- no AccessPath, handler, InnoDB, worker MQ, `Exchange_sort` default read, or
  `ParallelScanIterator::Read()` activation；
- no `Filesort` / `Sort_param` ownership or execution change；
- no user-visible ORDER BY PQ execution；
- ordinary ORDER BY SQL still reports `Not parallel HAS_ORDER_BY` and runtime
  counters stay zero。

Validation result:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats \
  pq_saved_order_group_contract --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5e2_target --tmpdir=/tmp/pqt_m11e5d5e2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5d5e2_full --tmpdir=/tmp/pqt_m11e5d5e2_full
```

Result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，4/4 including `shutdown_report`:
  `pq_commercial_order_by pq_stats pq_saved_order_group_contract`；
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5e-2:

- Review Agent verdict: `ACCEPT` after documentation fix；
- first review found stale 5c/5d commit-id status lines in this taskbook；
- fixed 5c to `4163db43197` and 5d to `06b68f558d3`；
- confirmed `pq_build_orderby_execution_preflight()` is fail-closed and only
  runs after the 5e-1 `future_candidate_disabled()` contract；
- confirmed the `HAS_ORDER_BY` serial boundary remains unchanged and no
  optimizer acceptance, AccessPath, handler/InnoDB, worker MQ, default ordered
  `Read()`, or `Filesort` / `Sort_param` execution path was opened；
- confirmed status exposure/reset and `pq_commercial_order_by` /
  `pq_stats` MTR updates are complete；
- residual risk: unsupported ORDER BY shapes still rely on the 5e-1
  fail-closed predicate and existing serial negative tests rather than
  per-shape preflight smoke assertions。

### M11-E5d-5f: ORDER BY Runtime Prerequisite Diagnostics

Status: coding completed；`git diff --check`, `mysqld` build, targeted MTR,
and full `parallel_query` suite passed；Code/Doc/Test Review Agent accepted；
waiting for commit。

Goal:

- keep the 5e-2 execution preflight fail-closed；
- split the central blocker into per-prerequisite diagnostics so later tasks
  can turn on readiness one dependency at a time；
- keep `HAS_ORDER_BY` serial boundary, visible ORDER BY execution, default
  worker MQ consumption, and default ordered `Read()` disabled。

Implementation summary:

- kept `pq_build_orderby_execution_preflight()` returning
  `BLOCKED_EXECUTION_DISABLED` for the first future candidate；
- added per-prerequisite missing counters for:
  - filesort runtime；
  - sort_param runtime；
  - worker ORDER BY frame producer；
  - `Exchange_sort` heap read；
  - leader materialization；
  - rowid tie-break；
  - default ordered `Read()`；
  - kill/detach/error diagnostics；
- exposed short `SHOW STATUS` names under
  `Parallel_orderby_preflight_missing_*` to avoid performance_schema variable
  name truncation；
- updated `pq_commercial_order_by` to assert every missing prerequisite counter
  grows by 1 under the DBUG preflight smoke；
- updated `pq_stats` expected variable count and variable list。

Scope confirmation:

- no `Exchange_sort`, iterator, AccessPath, handler, InnoDB, worker launch, or
  worker MQ changes；
- no `Filesort` / `Sort_param` construction or default lifetime ownership；
- no optimizer acceptance change；
- no user-visible ORDER BY PQ execution；
- ordinary ORDER BY SQL still reports `Not parallel HAS_ORDER_BY`。

Validation result:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats \
  --parallel=1 --vardir=/tmp/pqv_m11e5f_target --tmpdir=/tmp/pqt_m11e5f_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5f_full --tmpdir=/tmp/pqt_m11e5f_full
```

Result:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed，3/3 including `shutdown_report`:
  `pq_commercial_order_by pq_stats`；
- full `parallel_query` suite passed，89/89 including `shutdown_report`。

Code/Doc/Test Review - M11-E5d-5f:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed missing prerequisite counters increment only in the DBUG
  `pq_orderby_execution_preflight_smoke` path and only when the preflight is
  blocked by execution disabled；
- confirmed `pq_build_orderby_execution_preflight()` remains fail-closed with
  all eight prerequisites false；
- confirmed the ORDER BY hook still returns `HAS_ORDER_BY` after smoke helpers
  and does not open visible ORDER BY execution；
- confirmed short `Parallel_orderby_preflight_missing_*` status names avoid
  performance_schema variable-name truncation and are wired through stats,
  reset, SHOW STATUS, and MTR results；
- confirmed forbidden areas were not touched: `Exchange_sort`, iterators,
  AccessPath, handler/InnoDB, worker MQ, and default ordered `Read()`；
- residual risk: turning any prerequisite ready later requires a dedicated
  default runtime owner, lifecycle, error/KILL path, MTR coverage, and review。

### M11-E5g-0: Worker ORDER BY Frame Producer and Streaming Heap Read Boundary Design

Status: design completed；Design Review Agent accepted；committed as
`9de0ae25fa8`；no code edits in this subtask。

Goal:

- define the minimum contracts required before
  `worker_order_frame_producer_ready`, `exchange_sort_heap_read_ready`, or
  `default_ordered_read_ready` can become true；
- separate worker `PQOF` ORDER BY frame production from existing `PQWR`
  worker-result frames；
- define how `Exchange_sort` moves from controlled local loader / completed
  shadow read to streaming heap read；
- keep all execution preflight readiness flags false until a later coding task
  provides the default runtime owner and MTR evidence。

Current branch facts:

- `PQOF` ORDER BY frames are decoded and loaded only by controlled local smoke
  helpers；
- existing `Query_result_mq` / `PQWR` frames are still the worker-result
  protocol and must not be overloaded；
- `Exchange_sort::read_mq_message()` remains inert and returns `FINISH`；
- `read_ordered_record_shadow_shape()` drains already-loaded completed groups
  with an `int64` debug sort key；
- default `ParallelScanIterator::Read()` consumes only the `Exchange_nosort`
  row-image path；the order-gather debug state still returns EOF；
- 5f diagnostics prove every runtime prerequisite remains missing from the
  default path。

Commercial reference facts:

- commercial ORDER BY uses a default path equivalent to:
  `ParallelScanIterator::pq_make_filesort()` ->
  `ParallelScanIterator::pq_init_record_gather()` ->
  `MQ_record_gather::mq_scan_init()` ->
  `Exchange_sort::read_mq_record()`；
- commercial worker rows are stream-consumed through per-worker MQ, cached in
  batches, compared by Filesort/Sort_param-compatible sort keys, and merged by
  heap；
- commercial row materialization writes the selected ordered row into leader
  `table->record[0]`；
- the current branch lacks the default worker producer, default streaming heap
  read, default ordered `Read()` lifecycle, and ORDER BY-specific
  kill/detach/error diagnostics。

Readiness criteria:

1. `worker_order_frame_producer_ready` may become true only after a coding task
   proves a worker-side `PQOF` producer that:
   - emits ROW / FINISH / ERROR frames distinct from `PQWR`；
   - deep-copies worker record image, rowid/ref bytes, and sort-key payload；
   - proves per-worker local order for the supported first subset；
   - does not change `PQWR` wire format or existing worker-result tests；
   - has KILL/detach/error behavior scoped to ORDER BY frames。

2. `exchange_sort_heap_read_ready` may become true only after a coding task
   proves a streaming `Exchange_sort` reader that:
   - reads controlled or default `PQOF` frames per worker without treating
     WOULD_BLOCK as EOF；
   - handles ROW / FINISH / ERROR / detach explicitly；
   - refills per-worker batches and updates/removes heap entries correctly；
   - compares by real sort key contract and stable rowid tie-break；
   - keeps DESC, NULL, unsupported charset/variable/blob shapes fail-closed
     until separately tested。

3. `leader_materialization_ready` may become true only after the streaming
   reader can copy selected row images into leader `table->record[0]` through a
   default-path-compatible owner, not only an isolated smoke。

4. `default_ordered_read_ready` may become true only after
   `ParallelScanIterator::Init()` / `Read()` own the ordered gather lifecycle,
   worker MQ wiring, cleanup, counters, and KILL/error propagation while real
   ORDER BY SQL remains guarded by an explicit reviewed gate。

Proposed follow-up split:

1. M11-E5g-1: DBUG-only worker ORDER BY `PQOF` producer smoke.
   - coding task；
   - should use controlled worker-like table/record image inputs；
   - must not start default ORDER BY SQL, must not modify `PQWR`, and must keep
     `HAS_ORDER_BY` visible boundary；
   - may add producer-specific counters, but must not mark
     `worker_order_frame_producer_ready=true` in preflight。

2. M11-E5g-2: Streaming `Exchange_sort` heap-read state machine smoke.
   - coding task after 5g-1；
   - use controlled `PQOF` handles with interleaved ROW / WOULD_BLOCK / FINISH
     / ERROR；
   - prove heap refill/remove and explicit blocked/error statuses；
   - do not route default `ParallelScanIterator::Read()` through it。

3. M11-E5g-3: Ordered leader materialization smoke from streaming reader.
   - coding task after 5g-2；
   - writes selected rows into leader `table->record[0]` inside an isolated
     smoke only；
   - verifies row image length and table shape fail-closed。

4. Later reviewed task: default ordered `ParallelScanIterator` lifecycle.
   - only after Filesort/Sort_param runtime owner, worker producer, streaming
     heap read, materialization, rowid tie-break, and diagnostics pass
     independent review。

5g-1 / 5g-2 / 5g-3 must remain DBUG/controlled smokes. They must not be
interpreted as proof that the Filesort/Sort_param default runtime owner exists
or that any ORDER BY execution preflight readiness flag may become true。

Allowed files for 5g-0 design:

- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- `Docs/pq_tasks/README.md` status only if needed。

Forbidden files/actions for 5g-0:

- no source or MTR edits；
- no preflight ready flag changes；
- no optimizer eligibility or `HAS_ORDER_BY` behavior changes；
- no `ParallelScanIterator::Read()` default path changes；
- no `PQWR` format changes；
- no AccessPath, executor, handler, or InnoDB changes；
- no claim that visible ORDER BY PQ execution is ready。

Validation:

- design review only；
- no build/MTR required because no source or test code changes。

Design review request:

- confirm worker `PQOF` producer should be designed before default heap/read
  activation；
- confirm `PQOF` and `PQWR` must remain separate protocols；
- confirm streaming heap read must handle WOULD_BLOCK / FINISH / ERROR /
  detach explicitly；
- confirm no preflight readiness flag should become true in 5g-0 or 5g-1；
- confirm the proposed 5g-1/5g-2/5g-3 split is small enough for separate code
  review。

Design Review - M11-E5g-0:

- Review Agent verdict: `ACCEPT` after documentation fixes；
- findings: none after revision；
- fixed stale commit-id/status text for 5e-1, 5e-2, 5f, and README long
  summary；
- confirmed 5g-0 is design-only with no source or MTR changes；
- confirmed preflight readiness flags must remain false；
- confirmed `PQOF` and `PQWR` remain separate protocols；
- confirmed readiness criteria cover worker producer, streaming heap read,
  leader materialization, and default ordered `Read()`；
- confirmed 5g-1 / 5g-2 / 5g-3 are DBUG/controlled smokes only and cannot be
  interpreted as Filesort/Sort_param default runtime owner readiness。

Commit:

- `9de0ae25fa8` Plan PQ M11E order by streaming boundary。

### M11-E5g-1: DBUG-only Worker ORDER BY PQOF Producer Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；build,
targeted MTR, and full `parallel_query` suite passed；committed。

Goal:

- prove a worker-like ORDER BY producer can emit `PQOF` ROW / FINISH / ERROR
  frames without using or changing the existing `PQWR` worker-result protocol；
- prove producer-side fail-closed checks for local sort-key order, finished
  worker state, non-null row image, and non-null rowid/ref bytes；
- keep default ORDER BY execution, default worker MQ consumption, and default
  ordered `ParallelScanIterator::Read()` disabled。

Implementation:

- added `PQ_orderby_worker_frame_producer_shape` as a local
  `exchange_sort.cc` smoke-only producer shape；
- added producer helpers:
  - `pq_worker_orderby_producer_emit_row()`；
  - `pq_worker_orderby_producer_finish()`；
  - `pq_worker_orderby_producer_error()`；
- added `Exchange_sort::run_orderby_worker_frame_producer_smoke()`:
  - emits ordered ROW frames for two workers；
  - emits per-worker FINISH frames；
  - emits an ERROR frame for a separate worker；
  - decodes and validates only `PQOF` frames；
  - verifies worker id through frame flags, payload lengths, row image, rowid,
    sort key, and per-worker nondecreasing sort order；
  - verifies fail-closed behavior after FINISH and on decreasing sort key；
- wired the smoke behind DBUG flag
  `pq_orderby_worker_frame_producer_smoke` in `Gather_operator::
  run_exchange_sort_smoke()`；
- added producer-specific status variables:
  - `Parallel_orderby_worker_frame_producer_smoke_attempts`；
  - `Parallel_orderby_worker_frame_producer_smoke_success`；
  - `Parallel_orderby_worker_frame_producer_smoke_unsupported`；
  - `Parallel_exchange_sort_worker_frame_smoke_rows`；
  - `Parallel_exchange_sort_worker_frame_smoke_finishes`；
  - `Parallel_exchange_sort_worker_frame_smoke_errors`；
- extended `pq_commercial_order_by_frames` to verify:
  - no-DBUG ordinary SELECT does not increment producer counters；
  - DBUG smoke increments attempts/success/rows/finishes/errors；
  - unsupported remains zero；
- updated `pq_stats` Parallel status variable count from 159 to 165。

Scope notes:

- no `Query_result_mq` or `PQWR` wire-format changes；
- no `Exchange_sort::read_mq_message()` default consumption change；
- no `ParallelScanIterator::Read()` ordered path change；
- no optimizer `HAS_ORDER_BY` eligibility relaxation；
- no AccessPath, handler, InnoDB, worker thread, real `Read()`, Filesort, or
  Sort_param execution change；
- no preflight readiness flag is set true。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g1_target --tmpdir=/tmp/pqt_m11e5g1_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g1_full --tmpdir=/tmp/pqt_m11e5g1_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed: 4/4 including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Initial Review Agent guidance applied:

- made producer smoke DBUG/controlled instead of ordinary SELECT-visible；
- added attempt/success/unsupported counters in addition to frame counts；
- kept `PQOF` and `PQWR` separate；
- kept default ORDER BY PQ execution disabled；
- added no-DBUG zero-delta MTR assertions。

Code/Doc/Test Review - M11-E5g-1:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed `PQOF` / `PQWR` separation is preserved and `Query_result_mq` /
  `PQ_WORKER_RESULT_FRAME_MAGIC` are untouched；
- confirmed producer smoke runs only under DBUG flag
  `pq_orderby_worker_frame_producer_smoke`；
- confirmed no `HAS_ORDER_BY` relaxation, no default
  `Exchange_sort::read_mq_message()` behavior change, no
  `ParallelScanIterator::Read()`, AccessPath, handler, or InnoDB change；
- confirmed producer smoke covers FINISH-after-send fail-closed, decreasing
  sort-key fail-closed, ROW / FINISH / ERROR decode, per-worker local order,
  worker id flags, and row image / rowid / sort-key length checks；
- confirmed status variables, reset path, `SHOW STATUS`, `pq_stats`, and
  `pq_commercial_order_by_frames` are consistent；
- confirmed docs match the DBUG-only controlled scope and do not claim default
  ORDER BY execution readiness。

Next recommended action:

- enter M11-E5g-2 streaming `Exchange_sort` heap-read state-machine smoke。

Commit:

- Add PQ M11E order by worker frame producer smoke。

### M11-E5g-2: Streaming Exchange_sort Heap-read State-machine Smoke

Status: coding completed；Code/Doc/Test Review Agent accepted；build,
targeted MTR, and full `parallel_query` suite passed；waiting for commit。

Goal:

- prove a controlled streaming ORDER BY reader can refill per-worker `PQOF`
  batches, maintain heap membership, and distinguish ROW / WOULD_BLOCK /
  FINISH / ERROR / DETACHED；
- prove `WOULD_BLOCK` is not treated as EOF and blocks output when a
  non-terminal worker has no comparable head row；
- keep the default `Exchange_sort::read_mq_message()`,
  `ParallelScanIterator::Read()`, optimizer eligibility, and real ORDER BY
  execution unchanged。

Implementation:

- added `PQ_orderby_stream_read_status` with ROW, EOF_REACHED, WOULD_BLOCK,
  DETACHED, and ERROR states；
- added `PQ_orderby_loader_status::DETACHED` so MQ detach is no longer folded
  into generic ERROR inside the controlled loader；
- added `Exchange_sort::read_ordered_record_stream_shape()`:
  - refills each non-terminal worker from controlled `PQOF` handles；
  - reads available ROW batches until FINISH / WOULD_BLOCK / ERROR / DETACHED；
  - returns WOULD_BLOCK before emitting any row if a non-terminal worker lacks
    a head row；
  - adds workers with row heads into the heap；
  - uses `replace_first()` when a selected batch still has rows；
  - removes heap entries when a selected batch is drained；
  - returns ERROR and DETACHED as separate observable states；
- added `Exchange_sort::run_orderby_streaming_heap_read_smoke()`:
  - builds three controlled worker queues；
  - verifies initial WOULD_BLOCK does not output a row；
  - then supplies the blocked worker row and verifies ordered output
    `100, 200, 300, 400`；
  - verifies FINISH/EOF after all workers complete；
  - verifies ERROR frame fail-closed；
  - verifies producer detach is observable as DETACHED；
- wired the smoke behind DBUG flag `pq_exchange_sort_stream_heap_smoke`；
- added status variables:
  - `Parallel_exchange_sort_stream_heap_smoke_attempts`；
  - `Parallel_exchange_sort_stream_heap_smoke_success`；
  - `Parallel_exchange_sort_stream_heap_smoke_unsupported`；
  - `Parallel_exchange_sort_stream_heap_smoke_rows`；
  - `Parallel_exchange_sort_stream_heap_smoke_would_blocks`；
  - `Parallel_exchange_sort_stream_heap_smoke_finishes`；
  - `Parallel_exchange_sort_stream_heap_smoke_errors`；
  - `Parallel_exchange_sort_stream_heap_smoke_detaches`；
  - `Parallel_exchange_sort_stream_heap_smoke_refills`；
  - `Parallel_exchange_sort_stream_heap_smoke_heap_replaces`；
  - `Parallel_exchange_sort_stream_heap_smoke_heap_removes`；
- extended `pq_commercial_order_by_frames` and `pq_stats`。

Scope notes:

- no `Exchange_sort::read_mq_message()` default behavior change；
- no default `ParallelScanIterator::Read()` ordered path；
- no optimizer `HAS_ORDER_BY` relaxation；
- no `PQWR` / `Query_result_mq` change；
- no AccessPath, handler, InnoDB, worker launch, Filesort, or Sort_param
  runtime-owner change；
- no preflight readiness flag is set true。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g2_target --tmpdir=/tmp/pqt_m11e5g2_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g2_full --tmpdir=/tmp/pqt_m11e5g2_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed: 4/4 including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Code/Doc/Test Review - M11-E5g-2:

- Review Agent first verdict: `REQUEST_CHANGES` for stale README state；
- fixed README current-phase summary so 5g-1 is marked committed and 5g-2 is
  marked completed / waiting final review；
- Review Agent final verdict: `ACCEPT`；
- confirmed 5g-2 is controlled by DBUG flag
  `pq_exchange_sort_stream_heap_smoke`；
- confirmed no optimizer eligibility / `HAS_ORDER_BY`, default
  `Exchange_sort::read_mq_message()`, `ParallelScanIterator::Read()`,
  `PQWR` / `Query_result_mq`, AccessPath, handler, or InnoDB change；
- confirmed WOULD_BLOCK handling is conservative and does not output rows when
  a non-terminal worker lacks a head row；
- confirmed ROW / FINISH / ERROR / DETACHED states are observable；
- confirmed heap refill / replace / remove have smoke and status coverage；
- confirmed status variables, reset, `SHOW STATUS`, MTR, `pq_stats`, and docs
  are aligned；
- confirmed no preflight ready path is made true。

Next recommended action:

- commit 5g-2；
- then enter M11-E5g-3 ordered leader materialization smoke from streaming
  reader。

Commit:

- `914e32e438a` Add PQ M11E order by stream heap smoke。

### M11-E5g-3: Ordered Leader Materialization Smoke from Streaming Reader

Status: coding completed；build, targeted MTR, full `parallel_query` suite, and
Code/Doc/Test Review passed。

Goal:

- prove the controlled streaming ORDER BY reader can return a record image that
  is materialized into the leader `TABLE::record[0]`；
- prove materialized rows preserve the stream reader's ORDER BY sequence；
- prove row-image length mismatch is observable and fail-closed before visible
  leader materialization；
- keep real ORDER BY execution, default worker MQ consumption, default
  `ParallelScanIterator::Read()`, optimizer eligibility, and preflight
  readiness unchanged。

Implementation:

- added `Exchange_sort::run_orderby_streaming_materialization_smoke()`；
- the smoke builds three controlled `PQOF` worker queues, sends two out-of-order
  row images with sort keys, reads them through
  `read_ordered_record_stream_shape()`, and copies each returned row image into
  the leader table record for `Field::val_int()` verification；
- the smoke preserves and restores `table->record[0]` and temporary
  read/write bitmap bits for the first field；
- unsupported leader table shapes return an unsupported counter instead of
  touching the record；
- a negative controlled frame sends a short row image and records a
  bad-length/length-mismatch counter rather than materializing it；
- wired the smoke behind DBUG flag
  `pq_exchange_sort_stream_materialized_smoke`；
- added status variables:
  - `Parallel_exchange_sort_stream_materialized_smoke_attempts`；
  - `Parallel_exchange_sort_stream_materialized_smoke_success`；
  - `Parallel_exchange_sort_stream_materialized_smoke_unsupported`；
  - `Parallel_exchange_sort_stream_materialized_smoke_rows`；
  - `Parallel_exchange_sort_stream_materialized_smoke_bad_lengths`；
- extended `pq_commercial_order_by_frames` and `pq_stats`。

Scope notes:

- no default `Exchange_sort::read_mq_message()` behavior change；
- no default ordered `ParallelScanIterator::Read()` path；
- no optimizer `HAS_ORDER_BY` relaxation；
- no `PQWR` / `Query_result_mq` change；
- no AccessPath, handler, InnoDB, worker launch, Filesort, or Sort_param
  runtime-owner change；
- no preflight readiness flag is set true。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g3_target --tmpdir=/tmp/pqt_m11e5g3_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g3_full --tmpdir=/tmp/pqt_m11e5g3_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed: 4/4 including `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Next recommended action:

- enter M11-E5g-4 default execution path boundary design, still without opening
  visible ORDER BY PQ。

Code/Doc/Test Review - M11-E5g-3:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed the new materialized stream smoke is gated by DBUG flag
  `pq_exchange_sort_stream_materialized_smoke`；
- confirmed default `HAS_ORDER_BY` rejection, preflight false flags,
  `Exchange_sort::read_mq_message()`, and `ParallelScanIterator::Read()`
  remain unchanged；
- confirmed the smoke reads row images through
  `read_ordered_record_stream_shape()` before copying into leader
  `table->record[0]`；
- confirmed status variables, reset, `SHOW STATUS`, `pq_stats`, and
  `pq_commercial_order_by_frames` are aligned；
- non-blocking review note identified a future fragility around reusing
  `PQ_orderby_cached_merge_ctx` after `m_record_groups` reallocation；
- follow-up fix applied before commit: refresh `ctx.batches`, clear row/status
  state before the negative length-mismatch phase, and unconditionally clean up
  order-gather shape on exit；
- after follow-up fix, `git diff --check`, `mysqld` build, targeted MTR, and
  full `parallel_query` suite passed again。

### M11-E5g-4: Default ORDER BY Execution Path Boundary Design

Status: design in progress；no source execution path changes allowed in this
stage。

Goal:

- define the remaining contract gap between debug-only ORDER BY streaming
  smokes and a default executable ORDER BY PQ path；
- decide which readiness flags may become true in later coding phases, and
  which must remain false until all preceding contracts are proven；
- split future coding into small reviewed stages that preserve the current
  `HAS_ORDER_BY` serial boundary until an explicit visible gate review；
- keep this stage design-only and avoid changing optimizer eligibility,
  `ParallelScanIterator::Read()`, worker launch, InnoDB, or default
  `Exchange_sort` behavior。

Current confirmed baseline:

- `pq_build_orderby_execution_preflight()` still returns
  `BLOCKED_EXECUTION_DISABLED` and keeps `execution_disabled=true`；
- `pq_build_saved_order_group_contract()` remains fail-closed for the default
  runtime handoff path, so restored ORDER state is not yet executable evidence；
- ORDER BY candidate diagnostics exist, but `pq_check_query()` still rejects
  visible ORDER BY through `PQUnsuiteReason::HAS_ORDER_BY`；
- `PQOrderByExecutionPreflight` readiness flags for
  `worker_order_frame_producer_ready`, `exchange_sort_heap_read_ready`,
  `leader_materialization_ready`, `rowid_tiebreak_ready`, and
  `default_ordered_read_ready` remain false；
- `PQTableScanIterator::Init()` can run `pq_parallel_scan_iterator_order_gather_smoke`
  only under DBUG and falls back serial otherwise；
- `Gather_operator::run_exchange_sort_smoke()` now has controlled smokes for
  frame contract, worker producer, streaming heap read, and leader
  materialization, but none is wired into default execution；
- default `Exchange_sort::read_mq_message()` and default
  `ParallelScanIterator::Read()` ordered behavior are unchanged。

Required contracts before any visible ORDER BY PQ:

1. Filesort runtime contract:
   - define exactly which `Filesort` / `Sort_param` values are copied or owned
     by PQ runtime；
   - define whether `ParallelScanIterator::Init()` or `PQTableScanIterator::Init()`
     owns allocation/cleanup, and where fallback remains legal；
   - prove `Sort_param::make_sortkey()` can run without mutating shared
     JOIN/QEP_TAB state outside a bounded owner；
   - reject nullable ORDER BY, DESC, stable output/tie-break, and non-simple
     ORDER expressions until explicit support exists。
2. Saved ORDER/GROUP executable-state contract:
   - convert the current saved/restored ORDER/GROUP contract from diagnostic
     evidence into runtime-owned state；
   - prove restored ORDER chain and table bitmap state survive optimizer to
     iterator handoff；
   - keep the path fail-closed until restored state can be consumed without
     shared JOIN mutation。
3. Worker ORDER BY producer contract:
   - convert worker record images to `PQOF` frames through a non-DBUG producer
     function with bounded ownership；
   - prove each worker stream is locally sorted before leader heap merge；
   - preserve independent worker TABLE/handler/prebuilt lifecycle；
   - report ROW / FINISH / ERROR / DETACH with observable counters。
4. Leader streaming reader contract:
   - promote `read_ordered_record_stream_shape()` from smoke helper shape into a
     default-safe reader interface；
   - define bounded wait / kill / detach behavior for WOULD_BLOCK and worker
     ERROR；
   - guarantee heap comparator ownership after any buffer cleanup/reinit。
5. Leader materialization contract:
   - materialize only full-length record images matching leader `TABLE::record[0]`；
   - preserve read/write bitmaps and original record contents on fallback/error；
   - reject unsupported table shapes and length mismatch before exposing a row。
6. Iterator integration contract:
   - define how `PQTableScanIterator::Init()` selects an ORDER BY gather path
     only after preflight readiness；
   - define how `Read()` drains ordered rows and falls back only before workers
     are started；
   - once workers are started, errors must surface or cleanly detach, not
     silently fallback mid-stream。
7. Visible eligibility contract:
   - keep `HAS_ORDER_BY` serial until all readiness flags are true；
   - when opened, limit to a narrow ASC, non-nullable, single-table, simple
     ORDER BY subset with no LIMIT pushdown, no DESC, no expressions, no BLOB,
     no GROUP BY interaction, and no ref/ICP combination。

Proposed coding split after design review:

- M11-E5g-4a: default ORDER BY execution state contract and preflight readiness
  detail design/counters only; no true flags；
- M11-E5g-4b: fail-closed ordered materialization API skeleton, default returns
  unsupported/disabled and does not connect to `Read()`；
- M11-E5g-4c: default-safe `Exchange_sort` ordered reader skeleton extracted
  from smoke helper, controlled/DBUG-only, proving status preservation；
- M11-E5g-4d: worker `PQOF` producer adapter skeleton with DBUG-only caller and
  no ordinary ORDER BY SQL entry；
- M11-E5g-4e: kill/detach/ERROR diagnostics for the ordered shadow path；
- M11-E5g-4f: final visible gate design for a minimal ASC-only ORDER BY subset；
- M11-E5g-4g: visible gate coding only if 4f review accepts all constraints。

Allowed files for M11-E5g-4 design-only:

- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- `Docs/pq_tasks/README.md`。

Allowed files for later coding phases, after separate review:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/parallel_query/pq_iterator.h`；
- `sql/parallel_query/pq_iterator.cc`；
- `sql/parallel_query/pq_optimizer.h`；
- `sql/parallel_query/pq_optimizer.cc` only for diagnostics/preflight fields
  until the visible gate phase；
- focused MTR under `mysql-test/suite/parallel_query/`。

Forbidden in M11-E5g-4 and all pre-visible-gate coding:

- changing `pq_check_query()` to stop rejecting `HAS_ORDER_BY`；
- setting `PQOrderByExecutionPreflight::execution_disabled=false`；
- setting default readiness flags true without a matching reviewed coding
  phase and MTR；
- changing default `Exchange_sort::read_mq_message()` semantics；
- changing non-DBUG `ParallelScanIterator::Read()` ordered behavior；
- changing `PQWR` / `Query_result_mq` protocol or worker-result behavior；
- changing InnoDB worker scan, handler APIs, AccessPath construction, or
  `JOIN::optimize()` hooks for ORDER BY；
- enabling DESC, nullable ORDER BY, expression ORDER BY, LIMIT pushdown,
  rowid/stable tie-break, GROUP BY + ORDER BY, ref/ICP + ORDER BY, or
  multi-table ORDER BY。

Required tests for future coding:

- preflight/status:
  - each readiness flag is visible as false until its exact coding phase；
  - `pq_commercial_order_by` continues to report `Not parallel HAS_ORDER_BY`
    before visible gate；
  - ordinary ORDER BY keeps `Parallel_queries_executed = 0`,
    `Parallel_workers_launched = 0`, and `Parallel_ranges_dispatched = 0`；
  - ordinary ORDER BY must not grow `Parallel_exchange_sort_*_smoke_*`
    counters；
  - `pq_stats` count and numeric variable checks stay aligned。
- ordered reader:
  - ASC two/three worker stream；
  - WOULD_BLOCK bounded wait；
  - ERROR frame；
  - worker detach；
  - empty worker FINISH；
  - length mismatch。
- iterator shadow:
  - DBUG-only ordered read returns sorted rows；
  - external KILL cleanup；
  - worker ERROR cleanup；
  - no mid-stream serial fallback after workers start。
- visible gate, only after review:
  - minimal ASC single-table supported query；
  - DESC/nullable/expression/LIMIT/GROUP BY/ref/ICP/multi-table negative cases；
  - result equivalence against serial baseline；
  - full `parallel_query` suite。

Hard stop conditions:

- stop if `Sort_param::make_sortkey()` needs shared JOIN/QEP_TAB mutation that
  cannot be isolated；
- stop if saved/restored ORDER/GROUP state cannot be handed to runtime as an
  executable owner-owned contract；
- stop if worker row image and leader table record layouts cannot be proven
  identical for the supported subset；
- stop if ordered reader cannot distinguish WOULD_BLOCK from EOF under bounded
  wait；
- stop if worker ERROR/DETACH cannot be surfaced without corrupting the leader
  record；
- stop if visible ORDER BY requires changing AccessPath or InnoDB scan
  contracts before the SQL-layer reader path is proven。

Validation for M11-E5g-4:

- design review only；
- no build/MTR required unless the design task accidentally changes source or
  test files。

Design Explorer - M11-E5g-4:

- Explorer recommendation: keep 5g-4 strictly `design-only / boundary-only`；
- confirmed debug-only `PQOF` producer, streaming heap smoke, and leader
  materialization smoke are not default execution path evidence；
- confirmed default ORDER BY PQ must not open in 5g-4 and ordinary ORDER BY SQL
  must continue through `HAS_ORDER_BY` serial boundary；
- highlighted missing prerequisites: default `Filesort` / `Sort_param`
  lifecycle owner, executable saved ORDER/GROUP state, worker `PQOF` default
  producer, default `Exchange_sort` MQ consumption, ordered materialization API,
  ordered iterator `Read()` state, tie-break / NULL / DESC / LIMIT policy, and
  one-by-one preflight readiness flip policy；
- confirmed forbidden scope includes relaxing `HAS_ORDER_BY`, changing default
  ordered `Read()`, making `Exchange_sort::read_mq_message()` real, changing
  `PQWR`, AccessPath, handler/InnoDB worker launch, or setting readiness flags
  true；
- suggested future coding split has been reflected in the taskbook as 4a-4g。

Design Review - M11-E5g-4:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed 5g-4 is design-only and allows only taskbook/README edits；
- confirmed default ORDER BY PQ remains closed by `HAS_ORDER_BY`, disabled
  preflight, false readiness flags, DBUG-only smokes, inert
  `Exchange_sort::read_mq_message()`, and unchanged ordered `Read()` behavior；
- confirmed hard blockers are covered: `Filesort` / `Sort_param` ownership,
  saved ORDER/GROUP executable state, worker `PQOF`, `Exchange_sort` reader,
  leader materialization, iterator `Read()`, tie-break / NULL / DESC / LIMIT,
  and readiness flip policy；
- confirmed 4a-4g split is small and ordered；
- confirmed tests cover serial ORDER BY, zero executed/workers/ranges, no smoke
  counter growth, reader states, KILL/error cleanup, and visible-gate negative
  matrix；
- non-blocking review note requested an explicit `PQWR` / `Query_result_mq`
  forbidden bullet；added before commit。

### M11-E5g-4a: ORDER BY Preflight Runtime-state Detail

Status: coding completed；build, targeted MTR, and full `parallel_query` suite
passed；waiting for Code/Doc/Test Review；default ORDER BY execution remains
disabled。

Goal:

- add a fail-closed preflight detail for saved/restored ORDER/GROUP runtime
  ownership；
- keep all ORDER BY execution readiness flags false；
- keep `execution_disabled=true` and `BLOCKED_EXECUTION_DISABLED` for future
  ORDER BY candidates；
- keep ordinary ORDER BY SQL rejected by `HAS_ORDER_BY` and out of PQ runtime。

Implementation:

- added `PQOrderByExecutionPreflight::saved_order_group_runtime_ready`；
- reset and preflight builder keep the new flag false；
- added `PQ_global_stats::orderby_execution_preflight_missing_saved_order_state`
  and `SHOW STATUS` variable
  `Parallel_orderby_preflight_missing_saved_order`；
- extended the DBUG-only `pq_orderby_execution_preflight_smoke` path to count
  the missing saved ORDER/GROUP runtime-state prerequisite；
- extended `pq_commercial_order_by` to assert the new missing prerequisite
  grows only during the DBUG preflight smoke and does not continue growing for
  subsequent ordinary ORDER BY statements；
- updated `pq_stats` expected `Parallel%` count and status variable list。

Scope notes:

- no `HAS_ORDER_BY` relaxation；
- no `execution_disabled=false`；
- no readiness flag is set true；
- no `PQ_execution_state` change；
- no `ParallelScanIterator::Read()` / `PQTableScanIterator` default ORDER BY
  behavior change；
- no `Exchange_sort::read_mq_message()` change；
- no `PQWR` / `Query_result_mq`, AccessPath, handler, InnoDB, worker launch,
  or MQ consumption change。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4a_target --tmpdir=/tmp/pqt_m11e5g4a_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4a_full --tmpdir=/tmp/pqt_m11e5g4a_full
```

Review status:

- Preflight Contract Explorer completed read-only review and agreed the minimum
  5g-4a change should be diagnostic-only；
- Explorer confirmed no new `PQOrderByExecutionPreflightStatus` is needed；
- Explorer confirmed `pq_commercial_order_by` should carry behavior assertions
  and `pq_stats` should only cover the new status variable inventory。

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed: `pq_commercial_order_by pq_stats` 3/3 including
  `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Code/Doc/Test Review - M11-E5g-4a:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed tracked file scope is limited to `pq_optimizer.h/cc`,
  `sql_parallel.h`, `mysqld.cc`, `pq_commercial_order_by`, `pq_stats`, and
  task docs；
- confirmed `saved_order_group_runtime_ready` is reset false and explicitly
  kept false in preflight build；
- confirmed no readiness flag is set true, no `execution_disabled=false`, and
  no new `PQOrderByExecutionPreflightStatus`；
- confirmed the new missing-saved-order counter increments only inside the
  DBUG-gated `pq_orderby_execution_preflight_smoke` path；
- confirmed ordinary ORDER BY still returns `HAS_ORDER_BY`, with
  executed/workers/ranges deltas staying zero and final saved-order missing
  delta still 1；
- confirmed no tracked diff touches `pq_check_query` forbidden logic,
  `PQ_execution_state`, iterators, `Exchange_sort`, `Query_result_mq` / `PQWR`,
  AccessPath, handler, or InnoDB paths；
- confirmed `pq_stats` status count/list is updated and docs accurately record
  diagnostic-only scope and validation。

### M11-E5g-4b: Fail-closed Ordered Materialization API Skeleton

Status: coding completed；`mysqld` build, targeted MTR, and full
`parallel_query` suite passed；waiting for Code/Doc/Test Review。

Goal:

- add a named `Exchange_sort` ordered materialization API boundary for future
  default ORDER BY execution wiring；
- keep the API fail-closed until default reader, worker producer, iterator
  lifecycle, and visible eligibility gates are reviewed；
- expose DBUG-only counters proving the default skeleton is reachable only from
  smoke code and returns `DISABLED` without reading MQ or writing leader record
  buffers。

Implementation:

- added `PQ_orderby_materialize_status` with explicit
  `ROW` / `EOF_REACHED` / `WOULD_BLOCK` / `DETACHED` / `UNSUPPORTED` /
  `DISABLED` / `ERROR` states；
- added
  `Exchange_sort::materialize_next_ordered_record_image_status(TABLE *,
  PQ_orderby_materialize_status *)`；
- the 4b implementation initializes status to `ERROR`, rejects null arguments
  as failure, and otherwise returns `false` with status `DISABLED`；
- added `Exchange_sort::run_orderby_materialize_api_skeleton_smoke()` to call
  the API through a DBUG-only smoke path and count disabled/unsupported/rows；
- added status variables:
  `Parallel_exchange_sort_ordered_materialize_api_attempts`,
  `Parallel_exchange_sort_ordered_materialize_api_disabled`,
  `Parallel_exchange_sort_ordered_materialize_api_rows`, and
  `Parallel_exchange_sort_ordered_materialize_api_unsupported`；
- extended `pq_commercial_order_by_frames` to assert no default-path counter
  growth without DBUG and `attempts=1`, `disabled=1`, `unsupported=0`,
  `rows=0` with `pq_exchange_sort_ordered_materialize_api_smoke`；
- updated `pq_stats` expected `Parallel%` count and status variable list。

Scope notes:

- no `HAS_ORDER_BY` relaxation；
- no `execution_disabled=false` or readiness flag change；
- no optimizer, AccessPath, handler, InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, or worker MQ consumption change；
- no `Exchange_sort::read_mq_message()` default path change；
- no `ParallelScanIterator::Read()` or `PQTableScanIterator` ordered default
  behavior change；
- no `table->record[0]` writes in the new default skeleton；`ROW` is reserved
  for later reviewed phases。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4b_target --tmpdir=/tmp/pqt_m11e5g4b_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4b_full --tmpdir=/tmp/pqt_m11e5g4b_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
- full `parallel_query` suite passed: 89/89。

Explorer result:

- API Explorer recommended a distinct materialization status enum instead of
  reusing stream-read status；
- Explorer confirmed the default API should not read MQ, heap state, or record
  buffers, and should return `DISABLED`/`UNSUPPORTED` until default execution is
  explicitly opened；
- Explorer recommended DBUG-only MTR assertions in
  `pq_commercial_order_by_frames` and `pq_stats` inventory updates。

Code/Doc/Test Review - M11-E5g-4b:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed the API is fail-closed: it initializes status to `ERROR`, rejects
  null arguments, and otherwise only returns `DISABLED`；
- confirmed the new API does not read MQ, heap state, call
  `read_ordered_record_stream_shape()`, call `Exchange_sort::read_mq_message()`,
  or write `table->record[0]`；
- confirmed the only call site is DBUG-gated by
  `pq_exchange_sort_ordered_materialize_api_smoke`；
- confirmed tracked diff does not touch optimizer, AccessPath, handler,
  InnoDB, worker launch, `PQWR` / `Query_result_mq`, or default iterator
  `Read()` paths；
- confirmed MTR covers ordinary-path no-growth for attempts/rows and DBUG-path
  `attempts=1`, `disabled=1`, `unsupported=0`, `rows=0`；
- confirmed `pq_stats` count change from 182 to 186 is consistent with exactly
  four new status variables；
- residual risk: this is still only an API skeleton；future phases must wire
  default reader lifecycle, worker producer, `ROW` materialization, ordering
  semantics, and visible eligibility gates through separate review。

### M11-E5g-4c: DBUG-only Controlled Ordered Reader Skeleton

Status: coding completed；`mysqld` build, targeted MTR, and full
`parallel_query` suite passed；waiting for Code/Doc/Test Review。

Goal:

- extract a named ordered-reader skeleton boundary that can return ordered row
  images and stream status from controlled `PQOF` input；
- keep the reader DBUG-only and smoke-only；
- keep default ORDER BY PQ closed: no default worker MQ consumption, no default
  ordered `Read()`, and no materialization into `TABLE::record[0]`。

Implementation:

- added `Exchange_sort::read_next_ordered_record_image_skeleton()` as a narrow
  wrapper around the existing controlled `read_ordered_record_stream_shape()`；
- added `Exchange_sort::run_orderby_ordered_reader_skeleton_smoke()` with a
  controlled 3-worker `PQOF` shape covering:
  - initial `WOULD_BLOCK` before worker 1 produces；
  - ordered ROW sequence from workers 0/1/2；
  - EOF after all workers finish；
  - fail-closed ERROR frame observation；
  - DETACHED observation when a producer closes；
- the new reader skeleton accepts only heap/in-heap/terminal-worker state and
  row-image/status output; it has no `TABLE *` argument and does not write
  leader record buffers；
- added DBUG flag `pq_exchange_sort_ordered_reader_skeleton_smoke`；
- added status variables:
  `Parallel_exchange_sort_ordered_reader_skeleton_attempts`,
  `..._success`, `..._unsupported`, `..._rows`, `..._would_blocks`,
  `..._finishes`, `..._errors`, `..._detaches`, `..._refills`,
  `..._heap_replaces`, and `..._heap_removes`；
- extended `pq_commercial_order_by_frames` to assert no ordinary-path counter
  growth and DBUG-path reader skeleton deltas；
- updated `pq_stats` expected `Parallel%` count and status variable list。

Scope notes:

- no `HAS_ORDER_BY` relaxation；
- no `execution_disabled=false` or readiness flag change；
- no optimizer, AccessPath, handler, InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, or default worker MQ consumption change；
- no `Exchange_sort::read_mq_message()` default path change；
- no `ParallelScanIterator::Read()` or `PQTableScanIterator` ordered default
  behavior change；
- no `table->record[0]` writes in 4c；materialization remains a separate
  reviewed boundary。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4c_target --tmpdir=/tmp/pqt_m11e5g4c_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4c_full --tmpdir=/tmp/pqt_m11e5g4c_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
- full `parallel_query` suite passed: 89/89。

Design Explorer - M11-E5g-4c:

- Explorer verdict: 4c can proceed directly to coding, no separate design-only
  commit required；
- Explorer confirmed the reader skeleton must remain controlled-input only,
  not accept `TABLE *`, not write `record[0]`, and not call default
  `Exchange_sort::read_mq_message()` or default `Read()`；
- Explorer recommended reusing `PQ_orderby_stream_read_status`, adding a
  DBUG-only smoke flag, and extending `pq_commercial_order_by_frames` plus
  `pq_stats`。

Code/Doc/Test Review - M11-E5g-4c:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed the 4c reader skeleton is a narrow wrapper over
  `read_ordered_record_stream_shape()` with no `TABLE *`, no `record[0]`, and
  no default `Read()`；
- confirmed controlled reader input flows through `PQOF` frame loader and
  `MQueue_handle::receive()`, not default `Exchange_sort::read_mq_message()`；
- confirmed 4b materialization remains fail-closed `DISABLED`；
- confirmed no tracked diff touches optimizer eligibility, AccessPath,
  handler, InnoDB, worker/PQWR, `Query_result_mq`, or iterator default read
  paths；
- confirmed DBUG entry is gated only by
  `pq_exchange_sort_ordered_reader_skeleton_smoke`；
- confirmed MTR covers ordinary no-growth for attempts/rows/errors/detaches and
  DBUG-path positive deltas；
- confirmed `pq_stats` count update from 186 to 197 is consistent with 11 new
  status variables；
- residual risk: 4c still does not prove default worker MQ, kill/wait policy,
  or visible ORDER BY iterator integration；those remain future reviewed
  boundaries。

### M11-E5g-4d: DBUG-only Worker PQOF Producer Adapter Skeleton

Status: coding completed；`mysqld` build, targeted MTR, and full
`parallel_query` suite passed；waiting for Code/Doc/Test Review。

Goal:

- extract a named worker-side `PQOF` producer adapter skeleton for future ORDER
  BY worker output；
- keep it DBUG-only and controlled-input only；
- keep default ORDER BY PQ closed: no worker thread launch, no InnoDB/handler
  scan, no `PQWR` / `Query_result_mq`, and no default ORDER BY SQL entry。

Implementation:

- added internal `PQ_orderby_worker_producer_adapter_shape`；
- added adapter helpers that emit ROW / FINISH / ERROR through existing
  `pq_send_orderby_frame()`；
- adapter validates monotonic `int64` sort keys within one worker and rejects
  row emission after FINISH / ERROR；
- added `Exchange_sort::run_orderby_worker_producer_adapter_skeleton_smoke()`
  with a controlled local `MQueue_handle`；
- smoke verifies 3 ROW frames, 2 FINISH frames, 1 ERROR frame, one sort-key
  order reject, and one after-finish reject；
- added DBUG flag `pq_orderby_worker_producer_adapter_skeleton_smoke`；
- added status variables:
  `Parallel_orderby_worker_adapter_attempts`,
  `..._success`, `..._unsupported`, `..._rows`, `..._finishes`,
  `..._errors`, `..._order_rejects`, and `..._after_finish_rejects`；
- extended `pq_commercial_order_by_frames` to assert no ordinary-path counter
  growth and DBUG-path adapter deltas；
- updated `pq_stats` expected `Parallel%` count and status variable list。

Scope notes:

- no `HAS_ORDER_BY` relaxation；
- no `execution_disabled=false` or readiness flag change；
- no optimizer, AccessPath, handler, InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, or default worker MQ consumption change；
- no `PQOF` frame header/format change；
- no `Exchange_sort::read_mq_message()` default path change；
- no `ParallelScanIterator::Read()` or `PQTableScanIterator` ordered default
  behavior change。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4d_target --tmpdir=/tmp/pqt_m11e5g4d_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4d_full --tmpdir=/tmp/pqt_m11e5g4d_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- first record run exposed MySQL status-name truncation for the long
  `after_finish_rejects` name；public 4d status names were shortened to
  `Parallel_orderby_worker_adapter_*`；
- targeted MTR passed:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
- full `parallel_query` suite passed: 89/89。

Design Explorer - M11-E5g-4d:

- Explorer verdict: 4d can proceed directly to coding, no separate design-only
  commit required；
- Explorer recommended a new worker producer adapter wrapper instead of
  treating the older 5g-1 smoke as the future adapter boundary；
- Explorer confirmed adapter arguments must remain limited to controlled
  `MQueue_handle` and row/sort-key bytes, with no `THD`, `JOIN`, `TABLE`,
  handler, `Query_result_mq`, or `PQWR` dependency。

Code/Doc/Test Review - M11-E5g-4d:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed adapter shape is internal and only stores `MQueue_handle *`,
  `worker_id`, monotonic sort-key state, and finish state；
- confirmed ROW / FINISH / ERROR emissions use existing `pq_send_orderby_frame()`
  and do not change `PQOF` header/format；
- confirmed smoke uses a local controlled `MQueue` / `MQueue_handle` and
  validates ROW, FINISH, ERROR, sort-key order reject, and after-finish reject；
- confirmed execution is gated strictly by
  `pq_orderby_worker_producer_adapter_skeleton_smoke`；
- confirmed no tracked diff touches optimizer eligibility, `HAS_ORDER_BY`,
  AccessPath, handler/InnoDB, worker launch, `PQWR`, `Query_result_mq`, default
  worker MQ consumption, or default ordered `Read()` paths；
- confirmed `pq_commercial_order_by_frames` covers ordinary no-growth and
  DBUG-path positive deltas；
- confirmed `pq_stats` count update from 197 to 205 matches 8 new public status
  variables, and shortened `Parallel_orderby_worker_adapter_*` names avoid
  status-name truncation；
- residual risk: 4d proves only controlled in-process adapter behavior；real
  worker thread wiring, InnoDB scan integration, backpressure, kill/wait
  handling, filesort key encoding, tie-breaks, DESC/mixed sort parts, and row
  materialization remain future reviewed boundaries。

### M11-E5g-4e: DBUG-only Ordered Diagnostics Consolidation

Status: coding completed；`mysqld` build, targeted MTR, and full
`parallel_query` suite passed；waiting for Code/Doc/Test Review。

Goal:

- add a minimal ordered shadow-path diagnostic boundary for the remaining kill
  prerequisite；
- avoid duplicating existing ERROR / DETACHED / WOULD_BLOCK smoke coverage；
- explicitly record that real ordered-path THD kill polling and worker-thread
  kill propagation are not wired yet。

Implementation:

- added `Exchange_sort::run_orderby_ordered_diag_skeleton_smoke()`；
- helper only returns `kill_not_wired=1` and does not read MQ, call
  `Exchange_sort::read_mq_message()`, write `TABLE::record[0]`, or call default
  `Read()`；
- added DBUG flag `pq_exchange_sort_ordered_diag_smoke`；
- added status variables:
  `Parallel_exchange_sort_ordered_diag_attempts`,
  `Parallel_exchange_sort_ordered_diag_success`, and
  `Parallel_exchange_sort_ordered_diag_kill_not_wired`；
- extended `pq_commercial_order_by_frames` to assert no ordinary-path growth and
  DBUG-path diagnostic deltas；
- updated `pq_stats` expected `Parallel%` count and status variable list。

Scope notes:

- no `HAS_ORDER_BY` relaxation；
- no `execution_disabled=false` or readiness flag change；
- no optimizer, AccessPath, handler, InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, or default worker MQ consumption change；
- no `PQOF` frame header/format change；
- no `Exchange_sort::read_mq_message()` default path change；
- no `ParallelScanIterator::Read()` or `PQTableScanIterator` ordered default
  behavior change；
- no claim that real kill handling is implemented；the public diagnostic is
  explicitly `kill_not_wired`。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by_frames \
  pq_commercial_order_by pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4e_target --tmpdir=/tmp/pqt_m11e5g4e_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5g4e_full --tmpdir=/tmp/pqt_m11e5g4e_full
```

Results:

- `git diff --check` passed；
- `mysqld` build passed；
- targeted MTR passed:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
- full `parallel_query` suite passed: 89/89。

Design Explorer - M11-E5g-4e:

- Explorer recommended coding-light diagnostics consolidation, not true kill
  wiring；
- Explorer confirmed existing stream heap and ordered reader skeleton smokes
  already cover ERROR / DETACHED / WOULD_BLOCK；
- Explorer recommended only minimal counters for attempts, success, and
  `kill_not_wired`；
- Explorer confirmed 4e must not set any ordered readiness flag true and must
  not claim real kill handling。

Code/Doc/Test Review - M11-E5g-4e:

- Review Agent first pass verdict: `REVISE`；
- code/test findings: none blocking；
- required doc fix: `Docs/pq_tasks/README.md` still had a stale
  "Next recommended action" pointing to M11-E5g-4b；
- fix applied: next recommended action now points to M11-E5g-4f minimal
  ASC-only visible ORDER BY gate design and explicitly keeps executable ORDER BY
  PQ closed unless 4f review accepts 4g coding conditions；
- Review Agent second pass verdict: `ACCEPT`；
- confirmed helper is diagnostic-only, DBUG-only, and only records
  `kill_not_wired=1`；
- confirmed no duplicated ERROR / DETACHED / WOULD_BLOCK smoke was added；
- confirmed no tracked diff touches optimizer eligibility, `HAS_ORDER_BY`,
  preflight readiness, AccessPath, handler/InnoDB, worker launch, `PQWR`,
  `Query_result_mq`, or default ordered `Read()` paths；
- confirmed ordinary no-growth and DBUG-path deltas are covered in
  `pq_commercial_order_by_frames`；
- confirmed `pq_stats` count update from 205 to 208 matches the three new
  status variables；
- residual risk: real THD kill polling, worker-thread propagation,
  backpressure, and default ORDER BY worker MQ consumption remain future
  reviewed boundaries。

### M11-E5g-4f: Minimal ASC-only Visible ORDER BY Gate Design

Status: design-only completed；Design Explorer and Code/Doc/Test Review Agent
accepted；no source changes。

Goal:

- re-check whether the branch can now relax the user-visible `HAS_ORDER_BY`
  boundary for a minimal ASC-only ORDER BY subset；
- define the exact visible candidate shape and fail-closed diagnostics that any
  later coding step must preserve；
- decide whether M11-E5g-4g may open an executable ORDER BY PQ path。

Current code facts:

- `pq_optimizer.cc` still rejects every normal `query_block->is_ordered()`
  statement through `PQUnsuiteReason::HAS_ORDER_BY`；
- `pq_build_orderby_eligibility_contract()` can identify a disabled future
  candidate only for a narrow shape: simple SELECT, non-hypergraph, single
  query block, single table, simple direct ORDER list, ASC-only, no LIMIT, no
  DISTINCT, no GROUP BY, no HAVING, no window functions, filesort required, and
  full scan；
- `pq_build_orderby_execution_preflight()` intentionally returns false for
  that candidate because all real runtime readiness flags remain false:
  saved ORDER/GROUP runtime state, Filesort runtime, Sort_param runtime, worker
  ORDER frame producer, Exchange_sort heap read, leader materialization, rowid
  tie-break, default ordered `Read()`, and kill/detach/error diagnostics；
- M11-E5g-4b/4c/4d/4e added DBUG-only skeletons and diagnostics, but none of
  them changed default optimizer eligibility, worker launch, real MQ
  consumption, or default ordered `ParallelScanIterator::Read()` behavior。

Design conclusion:

- M11-E5g-4f rejects opening an executable user-visible ORDER BY PQ gate now；
- keep `HAS_ORDER_BY` as the hard default serial boundary for normal SQL；
- the current branch may keep the disabled future-candidate contract as a
  diagnostic/design artifact, but it must not route matching ORDER BY SQL into
  PQ execution until `pq_build_orderby_execution_preflight()` has reviewed real
  runtime owners for every readiness flag；
- M11-E5g-4g should not remove the `HAS_ORDER_BY` return or set any ORDER BY
  preflight readiness flag true. If 4g is still needed, it should be limited to
  documentation or additional disabled diagnostics, not executable SQL。

Minimum future visible candidate shape, not approved for current coding:

- SQL command is simple `SELECT` in one simple query block；
- non-hypergraph optimizer path only until a separate review covers the
  hypergraph AccessPath factory；
- exactly one InnoDB base table and no partition table；
- access path is clustered full scan only；no secondary range/ref/ICP, no
  reverse range, no dependent ref, and no worker-side predicate pushdown mixed
  into ORDER BY；
- ORDER BY list contains only selected fixed-length integer fields, direct
  field references, explicit ASC direction, and no expression/subquery；
- projection is limited to record-image shapes already proven by current row
  materialization tests；
- no GROUP BY, DISTINCT, HAVING, window functions, rollup, locking read,
  subquery, derived table, view, union, generated/hidden/functional-index
  fields, BLOB/TEXT, nullable ORDER fields, DESC, or LIMIT；
- deterministic duplicate-key order must be proven by explicit ORDER BY
  tie-break columns or by reviewed rowid/ref-length semantics before LIMIT can
  be introduced。

Required prerequisites before any later executable gate:

1. `Filesort::make_sortorder()` / saved ORDER state visibility is available
   without mutating normal executor state；
2. `Sort_param` ownership is persistent for the lifetime of `Exchange_sort`；
3. default worker ORDER BY `PQOF` producer is wired from real worker rows, not
   only from smoke helpers；
4. default `Exchange_sort` heap reader consumes worker MQ frames with bounded
   wait, EOF, ERROR, DETACHED, and cleanup semantics；
5. leader materialization writes real ordered records into `table->record[0]`
   through a reviewed API；
6. stable duplicate-key tie-break semantics are implemented and tested；
7. default `ParallelScanIterator::Read()` can choose ordered gather only after
   preflight succeeds and before the PQ commit point；
8. THD kill, worker abort, MQ detach, and diagnostics are wired for the
   ordered path；
9. positive and negative MTR tests cover row order, duplicate keys, empty/one
   row tables, rejected shapes, zero-delta fallback counters, and full
   `parallel_query` suite stability。

Forbidden for M11-E5g-4g unless a new design review changes this conclusion:

- weakening or removing `PQUnsuiteReason::HAS_ORDER_BY` for ordinary SQL；
- setting `PQOrderByExecutionPreflight` readiness flags true by assertion；
- changing `execution_disabled` to false；
- changing AccessPath factory, handler/InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, or default `ParallelScanIterator::Read()` ordered path；
- claiming DBUG-only smoke counters as proof of user-visible ORDER BY PQ
  correctness。

Design Explorer - M11-E5g-4f:

- Explorer verdict: `REJECT 4g executable gate`；
- confirmed `pq_optimizer.cc` still hard-rejects all ordinary ORDER BY through
  `HAS_ORDER_BY` after running only DBUG diagnostics；
- confirmed `pq_build_orderby_eligibility_contract()` only recognizes a
  disabled future candidate and `pq_build_orderby_execution_preflight()` keeps
  every real runtime readiness flag false；
- confirmed default `Exchange_sort` and iterator paths are not executable for
  ORDER BY: real MQ read is inert, materialization returns disabled, ordered
  reader is skeleton-only, and kill diagnostics explicitly report
  `kill_not_wired`；
- confirmed `TryCreatePQTableScanIterator()` still depends on
  `join->pq_eligible`, so the `HAS_ORDER_BY` rejection prevents a normal ORDER
  BY statement from entering the PQ iterator；
- recommended that 4g, if kept, be limited to documentation, disabled
  diagnostics/status, or stronger serial-boundary MTR assertions；it must not
  remove `HAS_ORDER_BY`, set readiness flags true, change `execution_disabled`,
  or wire AccessPath/handler/InnoDB/worker launch/`PQWR`/`Query_result_mq`/
  default ordered `Read()`。

Validation for 4f:

- docs-only review；
- `git diff --check`；
- no build/MTR required because no source or test files are changed。

Code/Doc/Test Review - M11-E5g-4f:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed both taskbook and README consistently reject the current executable
  ORDER BY PQ gate and keep `HAS_ORDER_BY` as the default serial boundary；
- confirmed the taskbook forbids weakening `HAS_ORDER_BY`, setting readiness
  flags true, changing `execution_disabled`, or wiring AccessPath / worker /
  default ordered `Read()` in 4g；
- confirmed README next action says not to enter executable 4g coding if the
  review maintains the current conclusion；
- confirmed `git diff --check` passed；
- no build/MTR run because 4f only changes docs。

## M11-E5h: ORDER BY Runtime Sort-State Owner Contract

Status: design-only completed；Design Review Agent accepted；no source changes。

Decision from M11-E5g-4f:

- do not enter executable M11-E5g-4g；
- disabled ORDER BY diagnostics are already covered by the existing
  eligibility/preflight counters and `pq_commercial_order_by` assertions；
- the next useful work is the first real runtime readiness gap: runtime
  ownership for saved ORDER state, `Filesort`, and `Sort_param`。

Goal:

- define how a future ordered PQ path can hold runtime sort state without
  mutating normal executor state；
- decide the minimal owner object and lifetime boundaries for `Filesort` /
  `Sort_param` before any default `Exchange_sort` read path is enabled；
- produce a coding split that can be reviewed incrementally and still keeps
  `HAS_ORDER_BY` as the user-visible serial boundary。

Scope:

- design only in M11-E5h；
- target runtime prerequisites from 4f:
  1. `Filesort::make_sortorder()` / saved ORDER state visibility；
  2. persistent `Sort_param` ownership for `Exchange_sort` lifetime；
- do not solve worker `PQOF` production, default heap reader, leader
  materialization, rowid tie-break, default ordered `Read()`, or real kill
  propagation in this design。

Current evidence:

- M11-E5d-S1/S2/S3 and E5d-2a/2b/2c/2d already built sidecar/owned ORDER chain
  contracts, but they are DBUG/contract oriented and do not make runtime
  `Filesort` / `Sort_param` ownership ready；
- M11-E5d-3a/3c and E5d-4a/4b/4c introduced restored ORDER Filesort and
  scalar handoff smokes, but the default preflight still records Filesort and
  Sort_param runtime as missing；
- M11-E5d-5a/5b introduced `Exchange_sort` real-init state and allocation
  smokes, but no default SQL path owns a real sort state；
- `pq_build_orderby_execution_preflight()` keeps
  `filesort_runtime_ready=false` and `sort_param_runtime_ready=false` by
  design。

Proposed owner model:

- introduce a future `PQ_orderby_runtime_sort_state` style owner, preferably
  private to `exchange_sort` / ORDER BY path implementation, that contains:
  - copied/restored ORDER chain metadata owned outside normal `JOIN` mutation；
  - `Filesort` pointer or value owner with explicit construction/destruction
    rules；
  - `Sort_param` owner whose storage outlives `Exchange_sort` heap reads；
  - table/field layout references validated as leader-only and non-mutating；
  - diagnostic flags mapping to preflight readiness, but not setting readiness
    true until code review proves ownership；
- keep construction DBUG-only or helper-only until a later reviewed coding step
  proves it can be called before the PQ commit point and cleaned up on fallback。

Proposed coding split after this design review:

1. M11-E5h-1 sort-state owner shape:
   - add a fail-closed owner struct/helper that can be default-constructed,
     reset, and report unsupported；
   - no real `Filesort` construction, no readiness flag true；
   - task prompt must restate `THR_MALLOC` / `thd->mem_root`,
     `JOIN::filesorts_to_cleanup`, QEP, and AccessPath non-attach checks。
2. M11-E5h-2 runtime saved ORDER attach smoke:
   - DBUG-only helper copies/restores ORDER metadata into the owner and proves
     normal `JOIN` order state is unchanged after cleanup；
   - still no default SQL execution。
3. M11-E5h-3 `Sort_param` lifetime smoke:
   - DBUG-only helper initializes owner-managed scalar `Sort_param` state and
     validates cleanup / repeated use；
   - task prompt must restate `Filesort::m_sort_param`,
     `Sort_param::init_for_filesort()` read-set timing, and owner lifetime
     checks；
   - still no default worker MQ read or visible materialization。
4. M11-E5h-4 preflight evidence update:
   - only after 5h-1/2/3 review, consider adding clearer diagnostics that
     distinguish "owner shape exists" from "runtime ready"；
   - do not set `filesort_runtime_ready` or `sort_param_runtime_ready` true
     unless the helper is actually reachable from a reviewed default preflight
     owner path。

Allowed files for M11-E5h design:

- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- `Docs/pq_tasks/README.md`。

Potential coding files after design review:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/mysqld.cc` only for reviewed status variables；
- `sql/parallel_query/pq_optimizer.h` / `.cc` only for fail-closed
  preflight/diagnostics and never for eligibility opening；
- `mysql-test/suite/parallel_query/t/pq_commercial_order_by*.test` and matching
  result files；
- `mysql-test/suite/parallel_query/r/pq_stats.result` if status variables are
  added。

Forbidden:

- weakening or deleting `PQUnsuiteReason::HAS_ORDER_BY`；
- setting any ORDER BY execution preflight readiness flag true by assertion；
- changing `execution_disabled` to false；
- modifying AccessPath factory, handler/InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, default worker MQ consumption, or default ordered
  `ParallelScanIterator::Read()`；
- treating DBUG-only smokes as proof of user-visible ORDER BY PQ correctness；
- mixing secondary range/ref/ICP or GROUP BY work into ORDER BY sort-state
  ownership。

Validation:

Design-only M11-E5h:

```bash
git diff --check
```

Later coding steps:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by \
  pq_commercial_order_by_frames pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5h_target --tmpdir=/tmp/pqt_m11e5h_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5h_full --tmpdir=/tmp/pqt_m11e5h_full
```

Design Review - M11-E5h:

- Review Agent verdict: `ACCEPT`；
- findings: none blocking；
- confirmed skipping executable M11-E5g-4g is reasonable because 4f rejected
  the visible gate and E5h keeps `HAS_ORDER_BY` as the serial boundary；
- confirmed h-1/h-2/h-3/h-4 are executable as a conservative split:
  fail-closed owner shape, DBUG-only saved ORDER attach, DBUG-only
  `Sort_param` lifetime smoke, then optional preflight diagnostics；
- confirmed allowed/forbidden files and validation are sufficient for
  design-only and later coding；
- review note applied: h-1/h-3 task prompts must explicitly restate
  `THR_MALLOC` / `thd->mem_root`, `Filesort::m_sort_param`,
  `Sort_param::init_for_filesort()` read-set timing,
  `JOIN::filesorts_to_cleanup`, QEP, AccessPath non-attach, and owner lifetime
  checks。

### M11-E5h-1: ORDER BY Runtime Sort-state Owner Shape

Status: coding completed locally；waiting for Code/Doc/Test Review。

Goal:

- add a fail-closed runtime sort-state owner shell for future ORDER BY PQ；
- prove the owner shell can initialize/reset without constructing `Filesort`,
  initializing `Sort_param`, mutating `JOIN`, attaching to
  `JOIN::filesorts_to_cleanup`, QEP, or AccessPath, or setting runtime
  readiness；
- keep user-visible ORDER BY serial through `HAS_ORDER_BY`。

Completion Report - M11-E5h-1 Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_runtime_sort_state_owner_shape` with dimensional fields
    plus explicit false-by-default flags for `runtime_ready`,
    `filesort_constructed`, `sort_param_initialized`, `join_state_mutated`,
    `filesorts_cleanup_attached`, `qep_attached`, and
    `access_path_attached`；
  - added `Exchange_sort::init_runtime_sort_state_owner_shape()` and
    `cleanup_runtime_sort_state_owner_shape()`；
  - added `Exchange_sort::run_orderby_runtime_sort_state_owner_shape_smoke()`
    and chained it into existing DBUG-only
    `pq_exchange_sort_state_shape_smoke` coverage；
  - no new public status variables were added; existing
    `Parallel_exchange_sort_state_shape_smoke_*` counters remain the
    observable smoke boundary。
- scope notes:
  - no `Filesort` construction；
  - no `Sort_param` initialization；
  - no `THR_MALLOC` / `thd->mem_root` allocation change；
  - no `JOIN::filesorts_to_cleanup`, QEP, or AccessPath attach；
  - no `HAS_ORDER_BY` relaxation；
  - no preflight readiness flag or `execution_disabled` change；
  - no worker launch, `PQWR` / `Query_result_mq`, handler/InnoDB, default MQ
    consumption, or default ordered `ParallelScanIterator::Read()` change。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats` 4/4；
  - full `parallel_query` suite passed: 89/89。

Code/Doc/Test Review - M11-E5h-1:

- Review Agent verdict: `ACCEPT`；
- findings: one minor doc issue, fixed by adding `Docs/pq_tasks/README.md` to
  the changed-files list；
- confirmed the new owner shape is fail-closed and only records dimensions plus
  false-by-default runtime side-effect flags；
- confirmed there is no `Filesort` construction, `Sort_param` initialization,
  `THR_MALLOC` / `thd->mem_root` change, `JOIN::filesorts_to_cleanup`, QEP, or
  AccessPath attach；
- confirmed there is no `HAS_ORDER_BY`, preflight readiness,
  `execution_disabled`, worker/MQ, or default `Read()` execution-path change；
- confirmed reusing `pq_exchange_sort_state_shape_smoke` and existing
  `Parallel_exchange_sort_state_shape_smoke_*` counters is sufficient and no
  new status variables are needed；
- confirmed build, targeted MTR, full `parallel_query` suite, and
  `git diff --check` passed。

### M11-E5h-2: Runtime Saved ORDER Attach Smoke

Status: coding completed locally；waiting for Code/Doc/Test Review。

Goal:

- prove the optimizer-side saved ORDER metadata can be copied/restored/cloned
  into an owner-local sidecar without mutating `JOIN::order`；
- keep `Exchange_sort` independent from `JOIN` / `ORDER` internals；
- keep user-visible ORDER BY serial through `HAS_ORDER_BY`。

Design Explorer - M11-E5h-2:

- Explorer verdict: `ACCEPT`；
- recommended placing the smoke in `pq_optimizer.cc` by reusing
  `PQ_owned_order_chain_sidecar` and existing saved-order-chain smokes；
- recommended not passing `JOIN *`, `ORDER *`, or `ORDER_with_src *` into
  `Exchange_sort`；
- recommended reusing existing DBUG flags/counters and avoiding new status
  variables unless a later review finds an observability gap。

Completion Report - M11-E5h-2 Coding:

- changed files:
  - `sql/parallel_query/pq_optimizer.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_order_chain_identity_snapshot`；
  - added `pq_capture_order_chain_identity()` and
    `pq_order_chain_identity_matches()` to snapshot `JOIN::order` head, source,
    const-optimized flag, and node addresses；
  - added `pq_runtime_saved_order_attach_smoke()` that creates owner-local
    sidecar copies/restores/clones inside a local scope, then verifies the
    original `JOIN::order` identity is unchanged after the owner-local data is
    destroyed；
  - extended existing `pq_saved_order_chain_clone_copy_smoke` success condition
    to include the runtime saved ORDER attach smoke。
- scope notes:
  - no `Exchange_sort` dependency on `JOIN`, `ORDER`, or `ORDER_with_src`；
  - no `Filesort` construction；
  - no `Sort_param` initialization；
  - no `JOIN::filesorts_to_cleanup`, QEP, or AccessPath attach；
  - no `HAS_ORDER_BY` relaxation；
  - no preflight readiness flag or `execution_disabled` change；
  - no new status variables or MTR files。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_saved_order_group_contract pq_stats` 4/4；
  - full `parallel_query` suite passed: 89/89。

Code/Doc/Test Review - M11-E5h-2:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `pq_runtime_saved_order_attach_smoke()` only runs through the
  existing `pq_saved_order_chain_clone_copy_smoke` DBUG flag inside the
  optimizer-side `HAS_ORDER_BY` rejection branch；
- confirmed owner-local sidecars are local to `pq_optimizer.cc` and no
  `JOIN` / `ORDER` dependency is introduced into `Exchange_sort`；
- confirmed the identity snapshot covers the intended E5h-2 contract:
  `JOIN::order` head, source, const-optimized flag, and node-address chain；
- confirmed the patch does not claim full ORDER node field immutability beyond
  that identity contract；
- confirmed no `HAS_ORDER_BY`, preflight readiness, `execution_disabled`,
  `Filesort` / `Sort_param`, `JOIN::filesorts_to_cleanup`, QEP / AccessPath,
  worker / MQ, or default `Read()` path is changed；
- confirmed no new status variables or MTR files are required。

### M11-E5h-3: Runtime Sort_param Scalar Lifetime Smoke

Status: coding completed locally；waiting for Code/Doc/Test Review。

Goal:

- validate owner-managed scalar `Sort_param` metadata lifetime in
  `Exchange_sort` without persisting a real `Sort_param` object；
- prove scalar metadata can initialize, cleanup, and initialize again with new
  dimensions；
- keep user-visible ORDER BY serial through `HAS_ORDER_BY`。

Design Explorer - M11-E5h-3:

- Explorer verdict: `ACCEPT`；
- recommended not expanding real stack `Sort_param::init_for_filesort()` reuse
  because it has no reviewed reset contract；
- recommended putting E5h-3 in the `Exchange_sort` owner shape as scalar
  metadata only；
- recommended reusing existing `pq_exchange_sort_state_shape_smoke` and
  `Parallel_exchange_sort_state_shape_smoke_*` counters。

Completion Report - M11-E5h-3 Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added scalar metadata fields to
    `PQ_orderby_runtime_sort_state_owner_shape`:
    `sort_param_order_length`, `sort_param_max_record_length`, and
    `sort_param_ref_length`；
  - added `Exchange_sort::init_runtime_sort_param_scalar_shape()`；
  - added `Exchange_sort::run_orderby_runtime_sort_param_lifetime_smoke()`
    under the existing `pq_exchange_sort_state_shape_smoke` path；
  - the smoke validates first initialization, cleanup-to-zero, and a second
    initialization with different dimensions。
- scope notes:
  - no real `Sort_param` object is constructed or persisted；
  - no `Filesort` construction；
  - no `Sort_param::init_for_filesort()` call；
  - no `HAS_ORDER_BY` relaxation；
  - no `sort_param_runtime_ready` or other preflight readiness change；
  - no `execution_disabled=false`；
  - no worker launch, `PQWR` / `Query_result_mq`, handler/InnoDB, default MQ
    consumption, or default ordered `ParallelScanIterator::Read()` change；
  - no new status variables or MTR files。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats` 4/4；
  - full `parallel_query` suite passed: 89/89。

Code/Doc/Test Review - M11-E5h-3:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed only scalar `uint32` metadata fields were added and no real
  `Sort_param` is held；
- confirmed `init_runtime_sort_param_scalar_shape()` only validates dimensions,
  stores scalar metadata, and sets `sort_param_initialized`；
- confirmed there is no `Sort_param::init_for_filesort()`, `Filesort`
  construction, or `Filesort` call in the diff；
- confirmed cleanup resets the entire owner shape and the smoke covers first
  init, cleanup-to-zero, second init with different dimensions, and final
  cleanup-to-zero；
- confirmed no `HAS_ORDER_BY`, preflight readiness, `execution_disabled`,
  worker/MQ, or default `Read()` path is changed；
- confirmed no new status variables or MTR files are needed。

### M11-E5h-4: Preflight Evidence Update Decision

Status: docs-only completed；Docs Review Agent accepted；no source changes。

Goal:

- decide whether E5h-1/2/3 evidence should be reflected in
  `pq_build_orderby_execution_preflight()`；
- keep the distinction between DBUG evidence and default runtime readiness
  explicit。

Design Explorer - M11-E5h-4:

- Explorer verdict: `REVISE` against coding now；
- recommended docs-only closure instead of adding owner-evidence fields or new
  diagnostics；
- confirmed E5h-1/2/3 prove DBUG smoke / owner-shape existence, not a reviewed
  default preflight owner path；
- confirmed existing `pq_saved_order_chain_clone_copy_smoke` and
  `pq_exchange_sort_state_shape_smoke` counters are sufficient evidence for
  this stage。

Decision:

- do not change `pq_build_orderby_execution_preflight()` in E5h-4；
- do not add `owner_evidence` flags or public status variables；
- keep `saved_order_group_runtime_ready=false`,
  `filesort_runtime_ready=false`, and `sort_param_runtime_ready=false`；
- keep `execution_disabled=true` and `BLOCKED_EXECUTION_DISABLED`；
- continue treating E5h-1/2/3 as DBUG-only evidence, not runtime readiness。

Reasoning:

- E5h-1 owner shell does not construct `Filesort`, initialize `Sort_param`, or
  attach to JOIN/QEP/AccessPath；
- E5h-2 validates optimizer-side saved ORDER attach identity but still runs
  only under DBUG before the `HAS_ORDER_BY` rejection；
- E5h-3 stores scalar `Sort_param` metadata only and does not persist a real
  `Sort_param` object；
- default ORDER BY PQ still lacks a reviewed runtime path that can create these
  owners before the PQ commit point and clean them up on fallback。

Allowed future coding, only if a later review finds an observability gap:

- `sql/parallel_query/pq_optimizer.h` / `.cc` for fail-closed evidence fields
  that are not readiness and not eligibility；
- `sql/parallel_query/sql_parallel.h` / `.cc`, `sql/mysqld.cc`,
  `pq_commercial_order_by*.test/result`, and `pq_stats.result` only if a new
  public status variable is explicitly justified。

Forbidden:

- weakening `HAS_ORDER_BY`；
- setting any ORDER BY preflight readiness flag true；
- changing `execution_disabled` to false；
- changing AccessPath, handler/InnoDB, worker launch, `PQWR` /
  `Query_result_mq`, default MQ consumption, or default ordered
  `ParallelScanIterator::Read()`；
- constructing real `Filesort` or persisting real `Sort_param`；
- treating DBUG-only smoke counters as user-visible ORDER BY PQ correctness。

Validation:

- docs-only；
- `git diff --check`。

Docs Review - M11-E5h-4:

- verdict: `ACCEPT`；
- confirmed the taskbook does not change
  `pq_build_orderby_execution_preflight()`；
- confirmed readiness flags remain false and `execution_disabled` remains true；
- confirmed E5h-1/2/3 are documented as DBUG evidence, not runtime readiness；
- confirmed the next action correctly points to the default worker `PQOF`
  producer / `Exchange_sort` default heap-reader boundary。

### M11-E5i: Worker PQOF Producer Runtime Boundary

Status: design accepted；ready for M11-E5i-1 serial coding。

Goal:

- choose the next smallest ORDER BY runtime gap after E5h；
- define a fail-closed boundary for moving from controlled `PQOF` smoke frames
  toward a worker-local producer contract；
- do not open user-visible ORDER BY PQ and do not change preflight readiness。

Design Explorer - M11-E5i:

- verdict: `ACCEPT A`；choose worker-local controlled `PQOF` producer
  boundary before `Exchange_sort` default heap-reader；
- rejected immediate `Exchange_sort` default heap-reader work because it would
  touch default Exchange selection, leader reader state machine,
  materialization, wait/kill/error semantics, and default `Read()` behavior in
  one step；
- rejected more design-only inventory as the primary next action because
  E5g-4f and E5h-4 already closed the visible-gate and preflight-evidence
  decisions；
- recommended serial coding, with only design/review agents running in
  parallel。

Decision:

- next implementation unit is M11-E5i-1；
- M11-E5i-1 must be an incremental consolidation of the existing E5g-4d
  producer adapter skeleton, not a new producer family；
- M11-E5i-1 may rename, reuse, document, or minimally enhance the existing
  adapter skeleton so it represents one worker-local controlled `PQOF`
  producer-owner contract；
- M11-E5i-1 must remain DBUG-only or controlled-smoke-only；
- M11-E5i-1 must not wire the helper into default worker execution, default
  `Gather_operator::init()`, default `ParallelScanIterator::Read()`, or
  visible ORDER BY eligibility。

Existing baseline / Delta from M11-E5g-4d:

- E5g-4d already added `PQ_orderby_worker_producer_adapter_shape`；
- E5g-4d already added `pq_orderby_worker_producer_adapter_emit_row()`,
  `..._finish()`, and `..._error()` helpers using existing `PQOF` frames；
- E5g-4d already added
  `Exchange_sort::run_orderby_worker_producer_adapter_skeleton_smoke()`；
- E5g-4d already validates ROW / FINISH / ERROR, per-worker monotonic scalar
  sort-key reject, and after-FINISH reject；
- E5g-4d already added DBUG flag
  `pq_orderby_worker_producer_adapter_skeleton_smoke` and public counters
  `Parallel_orderby_worker_adapter_*`；
- source still also contains older
  `PQ_orderby_worker_frame_producer_shape` /
  `run_orderby_worker_frame_producer_smoke()` from the pre-adapter smoke path；
- therefore E5i-1 must not add a third parallel `PQOF` producer shape or
  another status-variable family。

M11-E5i-1 Coding Task: Consolidate worker-local PQOF producer owner contract

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.h`；
- `sql/parallel_query/sql_parallel.cc`；
- `sql/mysqld.cc` only if a new status variable is strictly needed；
- `mysql-test/suite/parallel_query/t/pq_commercial_order_by*.test`；
- matching `mysql-test/suite/parallel_query/r/*.result` files；
- `mysql-test/suite/parallel_query/r/pq_stats.result` only if status
  variables change；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden files and behavior:

- no `sql/parallel_query/pq_optimizer.*` changes, except a later separately
  reviewed fail-closed diagnostic that does not set readiness true；
- no `sql/join_optimizer/access_path.*` changes；
- no `sql/sql_executor.*`, `sql/sql_select.*`, `sql/handler.*`, or
  `storage/innobase/**` changes；
- no `sql/parallel_query/pq_clone*` changes；
- no `sql/parallel_query/query_result_mq.*` changes and no PQWR wire-format
  changes；
- no default `Gather_operator::init()` `Exchange_nosort` replacement；
- no default `ParallelScanIterator::Read()` ORDER BY path；
- no optimizer eligibility / `HAS_ORDER_BY` relaxation；
- no user-visible ORDER BY PQ execution。

Required fail-closed conditions:

- `pq_build_orderby_execution_preflight()` continues to return false；
- `worker_order_frame_producer_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- `default_ordered_read_ready=false`；
- `execution_disabled=true`；
- `HAS_ORDER_BY` remains the normal SQL serial boundary；
- DBUG smoke counters must not be interpreted as runtime readiness；
- ordinary ORDER BY SQL must not increase `Parallel_queries_executed`,
  worker-count, or range-count counters。

Implementation expectations for M11-E5i-1:

- keep `PQOF` separate from `PQWR` / `PQRM` protocols；
- reuse or consolidate the existing E5g-4d adapter skeleton into the single
  named worker-local producer-owner contract；
- do not introduce a third `PQOF` producer shape, helper family, DBUG flag, or
  status-variable family；
- if code changes are needed, prefer:
  - making the adapter skeleton naming and ownership comments explicit；
  - removing ambiguity between the older frame-producer smoke and the adapter
    owner contract without deleting tested coverage；
  - adding focused smoke assertions only for missing ownership/lifetime
    behavior；
- keep existing validation of after-FINISH reject and per-worker key-order
  reject behavior；
- keep the first sort-key contract scalar and limited to controlled ASC smoke；
- document that real `Filesort` key generation, DESC, NULL ordering, rowid
  tie-break, and default worker integration remain future work。

New acceptance criteria for M11-E5i-1:

- the task report must explain why the resulting code has exactly one intended
  adapter-owner contract for future worker-local `PQOF` production；
- any remaining older producer smoke must be documented as legacy/contract
  coverage and not as a second runtime owner；
- no new `PQOF` producer counter family may be added unless a review identifies
  a concrete observability gap；
- ordinary ORDER BY SQL must continue to show zero ordinary-path growth for the
  existing worker adapter counters。

Validation for M11-E5i-1:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by \
  pq_commercial_order_by_frames pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5i_target --tmpdir=/tmp/pqt_m11e5i_target
```

- full MTR:

```bash
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5i_full --tmpdir=/tmp/pqt_m11e5i_full
```

Review requirements:

- before coding: Design Review Agent must accept this taskbook；
- after coding: Code/Doc/Test Review Agent must verify source scope,
  fail-closed preflight behavior, ordinary ORDER BY negative counters, and MTR
  evidence；
- if review asks to touch default execution, split the task and stop before
  opening a visible path。

Design Review - M11-E5i:

- verdict: `REVISE` before coding；
- no critical findings；
- important finding: original E5i-1 wording overlapped with completed
  M11-E5g-4d adapter skeleton and could cause a coding agent to create a third
  producer family；
- required fix applied here:
  - added this E5g-4d baseline/delta section；
  - changed wording from default worker producer boundary to worker-local
    controlled `PQOF` producer boundary；
  - changed E5i-1 from "add a producer" to "consolidate the existing adapter
    skeleton into a single owner contract"；
  - explicitly forbade adding a third producer shape / DBUG flag /
    status-variable family。

Design Re-review - M11-E5i:

- verdict: `ACCEPT`；
- confirmed E5i-1 is now scoped as consolidation/enhancement of the existing
  E5g-4d adapter skeleton, not a new producer；
- confirmed third `PQOF` producer shape/helper/DBUG/status family is explicitly
  forbidden；
- confirmed fail-closed behavior, preflight readiness false state, and
  `HAS_ORDER_BY` serial boundary remain preserved；
- approved entering M11-E5i-1 serial coding。

### M11-E5i-1: Consolidate Worker-local PQOF Producer Owner Contract

Status: completed；Code/Doc/Test Review accepted。

Completion Report:

- changed files:
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - removed the older internal `PQ_orderby_worker_frame_producer_shape` owner
    and its dedicated helper family；
  - kept the existing public/DBUG legacy smoke entry
    `Exchange_sort::run_orderby_worker_frame_producer_smoke()` for coverage；
  - changed that legacy smoke to use
    `PQ_orderby_worker_producer_adapter_shape` and
    `pq_orderby_worker_producer_adapter_*()` helpers；
  - documented `PQ_orderby_worker_producer_adapter_shape` as the single
    controlled owner contract for future worker-local `PQOF` production；
  - did not add a new `PQOF` producer shape, helper family, DBUG flag, status
    variable, MTR file, or result file。
- fail-closed scope:
  - no `pq_optimizer.*` changes；
  - no preflight readiness flag changes；
  - no `HAS_ORDER_BY` relaxation；
  - no default worker execution, default `Gather_operator::init()`, default
    `Exchange_sort` selection, `Query_result_mq`, handler/InnoDB, or default
    `ParallelScanIterator::Read()` changes；
  - ordinary ORDER BY SQL remains protected by existing negative MTR coverage。
- validation:
  - `git diff --check -- sql/parallel_query/exchange_sort.cc` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - the legacy smoke API and counters still exist for compatibility, but now
    reuse the single adapter owner contract；
  - real worker thread integration, real Filesort key generation, DESC / NULL
    ordering, rowid tie-break, default MQ consumption, and default ordered
    `Read()` remain future reviewed work。

Code/Doc/Test Review - M11-E5i-1:

- verdict: `ACCEPT`；
- confirmed removing the old internal `PQ_orderby_worker_frame_producer_shape`
  and helper family is reasonable because it duplicated adapter semantics；
- confirmed the legacy `run_orderby_worker_frame_producer_smoke()` now reuses
  the single adapter owner while preserving ROW / FINISH / ERROR, after-FINISH
  reject, and descending-key reject coverage；
- confirmed no tracked diff touches `pq_optimizer.*`, `HAS_ORDER_BY`,
  `Gather_operator::init`, `Query_result_mq`, handler/InnoDB, or default
  `ParallelScanIterator::Read`；
- confirmed documentation accurately records scope, validation, and residual
  risks。

### M11-E5j: Exchange_sort Default Heap-reader State Owner Shape

Status: completed；Code/Doc/Test Review accepted。

Goal:

- add a default-path-compatible heap-reader state owner shape inside
  `Exchange_sort`；
- own heap / in-heap / terminal-worker vectors and reader counters through an
  explicit init/cleanup lifecycle；
- drive it only from controlled / DBUG `PQOF` smoke；
- do not enter default SQL execution and do not open user-visible ORDER BY PQ。

Design Explorer - M11-E5j:

- verdict: `ACCEPT with scope revision`；
- recommended coding the state owner shape directly after writing this
  taskbook；
- selected option A: `Exchange_sort` default heap-reader state owner shape；
- deferred option B: do not change `Exchange_sort::read_mq_message()` default
  semantics yet；
- rejected option C: keep
  `materialize_next_ordered_record_image_status()` returning `DISABLED`；
- recommended serial coding with only review agents in parallel。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `sql/parallel_query/sql_parallel.cc` only for DBUG smoke dispatch and summary；
- `sql/parallel_query/sql_parallel.h` / `sql/mysqld.cc` only if a minimal new
  counter is required；
- focused `pq_commercial_order_by*.test/result` and `pq_stats.result` only if
  counters change；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `sql/parallel_query/pq_optimizer.*`；
- `sql/sql_optimizer.*`；
- `sql/join_optimizer/access_path.*`；
- `sql/sql_executor.*` / `sql/sql_select.*`；
- `sql/handler.*`；
- `storage/innobase/**`；
- `sql/parallel_query/pq_clone*`；
- `sql/parallel_query/query_result_mq.*`；
- `Gather_operator::init()` default `Exchange_nosort` replacement；
- default `ParallelScanIterator::Read()` / `PQTableScanIterator::Read()`
  ordered path；
- `Exchange_sort::read_mq_message()` real MQ consumption；
- `materialize_next_ordered_record_image_status()` non-`DISABLED` behavior。

Required fail-closed conditions:

- `HAS_ORDER_BY` remains the ordinary SQL serial boundary；
- `pq_build_orderby_execution_preflight()` continues to return false；
- `worker_order_frame_producer_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- `default_ordered_read_ready=false`；
- `execution_disabled=true`；
- ordinary ORDER BY SQL must not increase `Parallel_queries_executed`,
  workers, or ranges；
- controlled ASC scalar sort-key smoke must not be treated as real Filesort
  key, DESC, NULL ordering, rowid tie-break, or runtime readiness。

Implementation expectations:

- add an owned reader state shape that can be initialized with worker count and
  existing record groups；
- the state shape should own `binary_heap`, `in_heap`, `terminal_workers`, and
  reader counters or equivalent tracked fields；
- cleanup must reset heap/vector/counter ownership and be idempotent；
- add or adapt one controlled smoke that proves:
  - initial no-row state returns WOULD_BLOCK without leaking ownership；
  - ROW / EOF path works with controlled `PQOF` frames；
  - ERROR / DETACHED path resets or reports through owned state；
  - cleanup after success and error returns the state to uninitialized；
- do not add a new public status family unless existing counters cannot prove
  the smoke。

Validation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 16
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query pq_commercial_order_by \
  pq_commercial_order_by_frames pq_stats --parallel=1 \
  --vardir=/tmp/pqv_m11e5j_target --tmpdir=/tmp/pqt_m11e5j_target
TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl \
  --suite=parallel_query --parallel=1 \
  --vardir=/tmp/pqv_m11e5j_full --tmpdir=/tmp/pqt_m11e5j_full
```

Review requirements:

- Code/Doc/Test Review Agent must verify no default execution path is opened；
- Review must confirm `read_mq_message()` remains inert and materializer remains
  `DISABLED`；
- Review must confirm heap owner cleanup is explicit and does not leave stale
  heap/vector state。

Completion Report - M11-E5j:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - moved `PQ_orderby_cached_merge_ctx` to the header so `Exchange_sort` can own
    a heap-reader context；
  - added `PQ_orderby_heap_reader_state_shape` and
    `PQ_orderby_heap_reader_counters`；
  - added owned state fields for heap reader context, in-heap flags, terminal
    workers, counters, and heap lifecycle；
  - added `init_orderby_heap_reader_state_shape()`,
    `read_next_ordered_record_image_owned_shape()`, and
    `cleanup_orderby_heap_reader_state_shape()`；
  - adapted existing `run_orderby_ordered_reader_skeleton_smoke()` to exercise
    the owned heap-reader state instead of local heap/vector variables；
  - reused existing DBUG flag/counters for ordered reader skeleton smoke and
    added no new public status variables。
- fail-closed scope:
  - no `pq_optimizer.*` changes；
  - no readiness flag changes；
  - no `HAS_ORDER_BY` relaxation；
  - no `Gather_operator::init()` Exchange selection change；
  - no `Exchange_sort::read_mq_message()` real MQ consumption；
  - no materializer behavior change; it remains `DISABLED`；
  - no default `ParallelScanIterator::Read()` ordered path；
  - no `Query_result_mq`, handler/InnoDB, AccessPath, or clone changes。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - owner state is still controlled-smoke-only and uses ASC scalar sort-key
    frames；
  - `read_mq_message()`, materialization, wait/kill policy, default worker MQ
    consumption, real Filesort key generation, DESC / NULL ordering, rowid
    tie-break, and visible ORDER BY eligibility remain future reviewed tasks。

Code/Doc/Test Review - M11-E5j:

- verdict: `ACCEPT`；
- confirmed moving `PQ_orderby_cached_merge_ctx` to the header is reasonable
  because `Exchange_sort` now owns it as a member；
- confirmed owned counters preserve existing smoke status semantics and remain
  wired through the existing DBUG-only path；
- confirmed `Exchange_sort::read_mq_message()` remains inert and the
  materializer still returns `DISABLED`；
- confirmed no tracked diff touches `pq_optimizer.*`, readiness flags,
  `HAS_ORDER_BY`, `Gather_operator::init()`, iterators, MQ result,
  handler/InnoDB, or AccessPath；
- non-blocking lifecycle notes for future phases:
  - `m_order_heap` is explicit lifecycle rather than RAII; current controlled
    paths clean it idempotently, but future hardening may use RAII；
  - `m_heap_reader_ctx.batches` points into `m_record_groups.data()`; future
    code must keep heap-reader cleanup before clearing/resizing record groups。

### M11-E5k: Exchange_sort Typed PQOF Read Helper Smoke

Status: completed；Code/Doc/Test Review accepted。

Goal:

- extract a typed helper that reads exactly one worker `PQOF` frame from a
  provided `MQueue_handle` and maps it to explicit loader status；
- reuse the helper from existing record-group loader code；
- validate ROW / FINISH / ERROR / WOULD_BLOCK / DETACHED / malformed cases
  through controlled smoke coverage；
- keep `Exchange_sort::read_mq_message()` inert and keep all default ORDER BY
  execution paths closed。

Design Explorer - M11-E5k:

- verdict: `ACCEPT WITH SCOPE REVISION`；
- recommended helper + DBUG smoke only, not a default `read_mq_message()`
  branch；
- required `read_mq_message()` to keep current non-DBUG behavior:
  `type=FINISH`, `datap=nullptr`, `data_len=0`, `return false`；
- required `materialize_next_ordered_record_image_status()` to remain
  `DISABLED`；
- rejected changing default Exchange selection, default iterator `Read()`, or
  preflight readiness。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `Exchange_sort::read_mq_message()` default semantic changes；
- `materialize_next_ordered_record_image_status()` behavior changes；
- `pq_optimizer.*`, readiness flags, or `HAS_ORDER_BY` eligibility changes；
- `Gather_operator::init()` default `Exchange_nosort` replacement；
- default `ParallelScanIterator::Read()` / `PQTableScanIterator::Read()`
  ORDER BY branch；
- `Query_result_mq`, handler/InnoDB, AccessPath, clone, or optimizer/executor
  path changes；
- new `PQOF` decoder family or mixing `PQOF` with `PQWR` / `PQRM`。

Implementation:

- added private helper
  `Exchange_sort::read_orderby_frame_from_worker_shape()`；
- helper calls `MQueue_handle::receive()` directly, maps:
  - `MQ_WOULD_BLOCK` -> `PQ_orderby_loader_status::WOULD_BLOCK`；
  - `MQ_DETACHED` -> `PQ_orderby_loader_status::DETACHED`；
  - valid `PQOF` ROW / FINISH -> corresponding loader status；
  - valid `PQOF` ERROR or malformed frame -> `ERROR` status；
- changed `load_orderby_frame_to_record_group()` to reuse this helper before
  writing into record groups；
- extended existing `run_orderby_frame_contract_smoke()` to validate helper
  status mapping for ROW / FINISH / ERROR / WOULD_BLOCK / DETACHED / malformed；
- did not change existing frame smoke output counters and did not add public
  status variables。

Fail-closed scope:

- no `read_mq_message()` default behavior change；
- no materializer behavior change；it remains `DISABLED`；
- no `pq_optimizer.*`, preflight readiness, or `HAS_ORDER_BY` changes；
- no default worker MQ consumption or default ordered iterator path；
- ordinary ORDER BY SQL remains protected by existing negative MTR coverage。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
  plus `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Residual risk:

- helper is still controlled-smoke-only and is not a reviewed default
  `read_mq_message()` branch；
- future typed `read_mq_message()` work must separately prove default Exchange
  selection cannot half-open ORDER BY PQ；
- real wait/kill policy, materialization, Filesort key generation, DESC / NULL
  ordering, rowid tie-break, and visible ORDER BY eligibility remain future
  reviewed tasks。

Code/Doc/Test Review - M11-E5k:

- verdict: `ACCEPT`；
- confirmed helper status mapping is reasonable for `MQ_WOULD_BLOCK`,
  `MQ_DETACHED`, valid ROW / FINISH, valid ERROR, and malformed frames；
- confirmed `load_orderby_frame_to_record_group()` preserves decode / copy /
  finish behavior and treats malformed frames as non-transport loader errors；
- confirmed smoke coverage is sufficient and existing `rows` / `finishes` /
  `errors` output counters are unchanged；
- confirmed no default path was opened: `read_mq_message()` remains inert and
  materialization remains `DISABLED`；
- review noted a minor side-effect difference: valid `PQOF` ERROR returned
  before resetting `batch.compare_state`；fixed by resetting compare state after
  helper return for ROW / FINISH / ERROR statuses, before status handling；
- post-review validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`。

### M11-E5l: Controlled Exchange_sort::read_mq_message Typed PQOF Branch Smoke

Status: committed as `72b4234fd62`；Code/Doc/Test Review accepted。

Goal:

- add an explicit controlled branch inside `Exchange_sort::read_mq_message()`
  for typed `PQOF` frames；
- keep default behavior inert unless a private smoke-only flag is enabled；
- validate controlled ROW / FINISH / ERROR / WOULD_BLOCK / DETACHED /
  malformed mapping；
- do not connect this branch to default SQL execution, materialization, or
  iterator `Read()`。

Design Explorer - M11-E5l:

- verdict: `ACCEPT WITH SCOPE REVISION`；
- allowed coding only with a private mode flag that defaults false and is
  enabled only by smoke helper；
- required disabled behavior to remain:
  `type=FINISH`, `datap=nullptr`, `data_len=0`, `return false`；
- required `materialize_next_ordered_record_image_status()` to remain
  `DISABLED`；
- required `Gather_operator::init()` to keep default `Exchange_nosort` and
  ordinary ORDER BY to remain rejected by `HAS_ORDER_BY`。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- default `Exchange_sort::read_mq_message()` behavior change when flag is false；
- `materialize_next_ordered_record_image_status()` behavior change；
- `pq_optimizer.*`, readiness flags, or `HAS_ORDER_BY` eligibility changes；
- `Gather_operator::init()` default `Exchange_nosort` replacement；
- default `ParallelScanIterator::Read()` / `PQTableScanIterator::Read()`
  ORDER BY branch；
- `Query_result_mq`, handler/InnoDB, AccessPath, clone, optimizer/executor path
  changes；
- any user-visible sysvar or public readiness counter for this smoke-only flag。

Implementation expectations:

- add a private `Exchange_sort` flag for controlled `PQOF` read mode；
- add a private or public smoke helper that enables the flag and drives local
  `PQOF` frames through `read_mq_message()`；
- when disabled, `read_mq_message()` must be byte-for-byte equivalent in
  externally visible behavior to the current inert stub；
- when enabled, it may read one controlled worker queue using the existing typed
  `PQOF` helper；
- ROW may expose the MQ-owned record-image pointer only for immediate smoke
  validation；do not persist it beyond the call；
- WOULD_BLOCK / DETACHED / malformed must not be treated as EOF success in
  future default code; this smoke can map them to controlled FINISH/ERROR
  behavior only as documented。

Validation:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`；
- full `parallel_query` suite。

Completion Report - M11-E5l:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added private `m_orderby_read_mq_shape_enabled` flag, default false；
  - added private `enable_orderby_read_mq_shape_for_smoke()`；
  - added controlled branch in `Exchange_sort::read_mq_message()` that is only
    active when the flag is true and MQ handles are initialized；
  - disabled branch keeps inert behavior:
    `type=FINISH`, `datap=nullptr`, `data_len=0`, `return false`；
  - controlled branch uses existing `read_orderby_frame_from_worker_shape()` on
    worker 0 only；
  - ROW returns `MQMessageType::ROW` with the MQ-owned record-image pointer for
    immediate smoke validation；
  - ERROR / malformed return `MQMessageType::ERROR` and fail closed；
  - FINISH / WOULD_BLOCK / DETACHED return false with the existing default
    FINISH/no-data shape；
  - `cleanup_order_gather_shape()` disables the smoke flag；
  - existing `run_orderby_frame_contract_smoke()` now calls
    `run_orderby_read_mq_message_controlled_smoke()`；
  - no new public counters, sysvars, or MTR files were added。
- fail-closed scope:
  - no `pq_optimizer.*` changes；
  - no readiness flag changes；
  - no `HAS_ORDER_BY` relaxation；
  - no `Gather_operator::init()` Exchange selection change；
  - no materializer behavior change；it remains `DISABLED`；
  - no default `ParallelScanIterator::Read()` ordered path；
  - no `Query_result_mq`, handler/InnoDB, AccessPath, clone, or executor path
    changes。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - `read_mq_message()` still cannot represent WOULD_BLOCK / DETACHED distinctly
    through its public interface；future default integration needs a richer
    status API or a separate materialization layer；
  - this smoke does not prove default worker MQ consumption, wait/kill policy,
    materialization, or visible ORDER BY readiness。

Code/Doc/Test Review - M11-E5l:

- Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed disabled `read_mq_message()` remains inert:
  `type=FINISH`, `datap=nullptr`, `data_len=0`, `return false`；
- confirmed typed `PQOF` branch is guarded by the private smoke flag and does
  not alter `Gather_operator::init()`, optimizer readiness, materializer, or
  iterator default paths；
- confirmed ROW data pointer is MQ-owned and only validated immediately by the
  controlled smoke；
- confirmed FINISH / WOULD_BLOCK / DETACHED remain folded into existing
  FINISH/no-data/false shape and ERROR / malformed fail closed；
- confirmed docs record the residual risk that public `read_mq_message()` cannot
  express WOULD_BLOCK / DETACHED distinctly。

### M11-E5m: Exchange_sort Ordered Read Rich Status API

Status: committed as `2bc1b98c81e`；Code/Doc/Test Review accepted。

Goal:

- add a controlled internal richer status API for ordered reads；
- explicitly represent `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `DETACHED`,
  `UNSUPPORTED`, `DISABLED`, and `ERROR`；
- keep public `read_mq_message()` default behavior inert；
- keep `materialize_next_ordered_record_image_status()` fail-closed as
  `DISABLED`；
- do not connect this API to default SQL execution or user-visible ORDER BY PQ。

Design Explorer - M11-E5m:

- verdict: `ACCEPT WITH SCOPE REVISION`；
- next step should prioritize richer status API before ordered materializer
  owner shape；
- reason: the public bool / FINISH shape cannot distinguish temporary
  WOULD_BLOCK, worker DETACHED, EOF, and ERROR；materialization before this
  boundary risks treating transient states as EOF；
- follow-up task after E5m should be M11-E5n ordered materializer owner shape。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `pq_optimizer.*`, `sql_optimizer.*`, `sql_executor.*`, `access_path.*`,
  `sql_parallel.*`, `pq_iterators.*`, `Query_result_mq.*`, handler/InnoDB, or
  storage-engine changes；
- relaxing `HAS_ORDER_BY`；
- changing default `Exchange_sort::read_mq_message()` behavior when the private
  E5l flag is false；
- changing `materialize_next_ordered_record_image_status()` non-`DISABLED`
  behavior；
- adding public readiness counters, sysvars, or user-visible ORDER BY
  eligibility。

Implementation plan:

- add private `PQ_orderby_ordered_read_status` as the richer internal boundary；
- add private `read_ordered_record_rich_status_shape()`；
- add a private smoke-only enable flag, default false；
- map disabled state to `DISABLED`；
- map enabled-but-not-ready owned reader state to `UNSUPPORTED`；
- map owned heap-reader states one-for-one:
  `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `DETACHED`, and `ERROR`；
- add private `run_orderby_rich_status_api_smoke()` and call it from the
  existing ordered-reader skeleton smoke, without adding public counters or new
  MTR files。

Validation:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`；
- full `parallel_query` suite；
- confirm ORDER BY SQL remains serial through `HAS_ORDER_BY` and PQ executed /
  worker / range counters do not grow for visible ORDER BY。

Completion Report - M11-E5m:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added private richer status enum
    `PQ_orderby_ordered_read_status`；
  - added private `read_ordered_record_rich_status_shape()`；
  - added private `m_orderby_rich_status_shape_enabled`, default false and
    cleared by `cleanup_order_gather_shape()`；
  - added private `run_orderby_rich_status_api_smoke()`；
  - smoke covers disabled, unsupported, would-block, row, EOF, error, and
    detached mappings；
  - wired the smoke through existing
    `run_orderby_ordered_reader_skeleton_smoke()`；
  - no default `read_mq_message()`, materializer, optimizer, iterator,
    handler/InnoDB, sysvar, or public counter changes。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - this is still an internal status boundary；it does not write
    `TABLE::record[0]` and does not prove visible ORDER BY readiness；
  - a later materializer owner must consume this richer status and must not
    collapse `WOULD_BLOCK` / `DETACHED` into EOF。

Code/Doc/Test Review - M11-E5m:

- Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed richer status API is private to `Exchange_sort` and has no external
  SQL / optimizer / iterator / handler / InnoDB call sites；
- confirmed `DISABLED` and `UNSUPPORTED` mappings are explicit；
- confirmed `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `DETACHED`, and `ERROR` map
  one-for-one from owned heap-reader status；
- confirmed `WOULD_BLOCK` and `DETACHED` are not folded into EOF；
- confirmed `m_orderby_rich_status_shape_enabled` defaults false and cleanup
  clears it；
- confirmed public `read_mq_message()` remains inert by default and
  `materialize_next_ordered_record_image_status()` remains `DISABLED`；
- confirmed `HAS_ORDER_BY` and execution preflight remain fail-closed。

### M11-E5n: Ordered Materializer Owner Shape

Status: committed as `3c1d273ae0d`；Code/Doc/Test Review accepted。

Goal:

- add a controlled ordered materializer owner shape inside `Exchange_sort`；
- let `materialize_next_ordered_record_image_status()` consume E5m rich status
  only when a private smoke flag is enabled；
- materialize only full-length row images into `TABLE::record[0]`；
- preserve original record and bitmap state through owner cleanup；
- keep default `materialize_next_ordered_record_image_status()` behavior
  `DISABLED` and keep user-visible ORDER BY PQ fail-closed。

Design Explorer - M11-E5n:

- verdict: coding allowed with narrow DBUG-only / controlled smoke scope；
- default SQL path must remain disabled；
- materializer must preserve E5m status distinctions and must not fold
  `WOULD_BLOCK` or `DETACHED` into EOF；
- controlled smoke may write `TABLE::record[0]` only with full record-length
  checks, bitmap protection, original-record restore, and idempotent cleanup。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- optimizer, executor, AccessPath, iterator, `Query_result_mq`, handler/InnoDB,
  or storage-engine changes；
- `Gather_operator::init()` Exchange selection changes；
- default `ParallelScanIterator::Read()` / `PQTableScanIterator::Read()` ORDER
  BY branch；
- user-visible sysvar, eligibility, readiness flag, or public counter changes；
- relaxing `HAS_ORDER_BY` or execution preflight blockers。

Implementation plan:

- add `PQ_orderby_materializer_owner_shape` and owner-local original record
  storage；
- add private `m_orderby_materializer_shape_enabled`, default false；
- add `init_orderby_materializer_owner_shape()` to save original
  `TABLE::record[0]` and first-field bitmap state；
- add `materialize_ordered_record_owner_shape()` to copy only full-length row
  images；
- add `cleanup_orderby_materializer_owner_shape()` and call it from
  `cleanup_order_gather_shape()`；
- extend `materialize_next_ordered_record_image_status()` so the private flag
  path maps rich status to materializer status:
  `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `DETACHED`, `UNSUPPORTED`, `DISABLED`,
  and `ERROR`；
- add private `run_orderby_materializer_owner_shape_smoke()` under the existing
  materialize API skeleton smoke。

Completion Report - M11-E5n:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_materializer_owner_shape`；
  - added owner-local original record storage；
  - added private materializer smoke flag, default false；
  - added owner init/copy/cleanup helpers；
  - controlled materializer path consumes E5m rich status only when the private
    flag is enabled；
  - default materializer path still returns `DISABLED`；
  - smoke covers `WOULD_BLOCK`, ordered `ROW` materialization, `EOF_REACHED`,
    short-record length error, worker `ERROR`, worker `DETACHED`, original
    record restore, and bitmap restore；
  - no optimizer, executor, iterator, handler/InnoDB, sysvar, readiness, or
    public counter changes。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - this is still a controlled smoke-only materializer owner；
  - it does not prove worker/leader TABLE layout compatibility for default
    execution；
  - it does not connect `Gather_operator`, iterator `Read()`, real Filesort, or
    visible ORDER BY eligibility。

Code/Doc/Test Review - M11-E5n:

- Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed default materializer remains fail-closed as `DISABLED` when the
  private smoke flag is false；
- confirmed owner shape saves and restores `TABLE::record[0]` and read/write
  bitmap state；
- confirmed `cleanup_order_gather_shape()` cleans materializer owner state and
  smoke flags；
- confirmed rich status mapping preserves `WOULD_BLOCK` and `DETACHED` as
  non-EOF states；
- confirmed ROW copy requires full `leader_table->s->reclength` match and short
  record path fails closed with restore；
- confirmed no optimizer, executor, AccessPath, iterator, `Query_result_mq`,
  handler/InnoDB, default Gather, eligibility, sysvar, readiness, or public
  counter changes。

### M11-E5o: Filesort / Sort_param Real Runtime Owner Readiness

Status: committed as `7a5d9b2ea27`；Code/Doc/Test Review accepted。

Goal:

- close the next ORDER BY runtime-owner gap before any visible gate work；
- strengthen the optimizer-side `Filesort` / `Sort_param` smoke into a
  runtime owner readiness proof；
- prove construction, Sort_param initialization, repeated Exchange_sort
  handoff, and cleanup do not pollute `JOIN` / QEP / AccessPath-visible state；
- keep execution preflight fail-closed and keep user-visible ORDER BY PQ
  disabled。

Design Explorer - M11-E5o:

- verdict: coding allowed, but not default ordered read or visible gate；
- next step must address `Filesort` / `Sort_param` runtime owner readiness；
- default Exchange_sort selection, `ParallelScanIterator::Read()` ordered hook,
  and visible eligibility remain too early；
- readiness evidence may be recorded through controlled smoke only and must not
  set runtime readiness flags true。

Allowed files:

- `sql/parallel_query/pq_optimizer.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `HAS_ORDER_BY` rejection changes；
- `pq_build_orderby_execution_preflight()` readiness true flags；
- AccessPath, executor, iterator, `Gather_operator::init()`, `Query_result_mq`,
  handler/InnoDB, storage-engine, sysvar, or public counter changes；
- default worker launch or default ordered read integration。

Implementation:

- enhanced existing `pq_run_orderby_sort_state_handoff_smoke()`；
- snapshot `JOIN::filesorts_to_cleanup`, `JOIN::order`, `best_ref`, `qep_tab`,
  and `sort_by_table` before real Filesort / Sort_param construction；
- keep using restored ORDER sidecar and local `Filesort` / stack `Sort_param`；
- verify Sort_param has a local sort order and nonzero max record length；
- run `Exchange_sort::run_orderby_sort_state_shape_handoff_smoke()` twice to
  prove repeat init / cleanup shape；
- require the JOIN/QEP pointers and cleanup list size to remain unchanged；
- keep existing DBUG trigger and counters:
  `pq_orderby_sort_state_handoff_smoke` and
  `Parallel_orderby_sort_state_handoff_smoke_*`。

Validation:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `pq_commercial_order_by pq_commercial_order_by_frames
  pq_saved_order_group_contract pq_stats`；
- full `parallel_query` suite。

Completion Report - M11-E5o:

- changed files:
  - `sql/parallel_query/pq_optimizer.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - strengthened existing optimizer-side sort-state handoff smoke；
  - added no-pollution snapshots for `JOIN::filesorts_to_cleanup`,
    `JOIN::order`, `best_ref`, `qep_tab`, and `sort_by_table`；
  - added repeat Exchange_sort handoff verification；
  - did not add counters, sysvars, readiness flags, default Exchange selection,
    iterator hooks, or visible eligibility。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by pq_commercial_order_by_frames
    pq_saved_order_group_contract pq_stats` plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - this is still DBUG-only runtime owner evidence；
  - it does not make real default ORDER BY execution ready；
  - worker ORDER BY `PQOF` producer, default heap reader, rowid tie-break,
    kill/detach/error wait policy, and default ordered `Read()` remain blocked。

Code/Doc/Test Review - M11-E5o:

- Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed the enhanced sort-state handoff smoke remains DBUG-only；
- confirmed default SQL execution and `HAS_ORDER_BY` rejection are unchanged；
- confirmed no-pollution snapshot covers `filesorts_to_cleanup`, `JOIN::order`,
  `best_ref`, `qep_tab`, and `sort_by_table`；
- confirmed local `Filesort` / stack `Sort_param` construction does not push to
  `JOIN::filesorts_to_cleanup`；
- confirmed repeated `Exchange_sort` handoff proves repeat init / cleanup shape
  without setting runtime readiness true；
- confirmed preflight readiness flags remain false and `execution_disabled`
  remains true；
- confirmed no AccessPath, executor, iterator, `Gather_operator`,
  `Query_result_mq`, handler/InnoDB, sysvar, or public counter changes。

### M11-E5p: Worker ORDER BY PQOF Default Producer Owner Contract

Status: committed as `2c84c61a19c`；Code/Doc/Test Review accepted。

Goal:

- promote the worker `PQOF` producer shape from a DBUG-only adapter skeleton to
  a default-path-owned API / owner contract；
- keep all callers DBUG / controlled-smoke only；
- prove producer lifecycle for ROW, FINISH, ERROR, DETACH, after-finish reject,
  local order reject, and idempotent cleanup；
- keep `PQOF` separate from `PQWR` / `Query_result_mq`；
- keep preflight `worker_order_frame_producer_ready` false and keep visible
  ORDER BY PQ disabled。

Design Explorer - M11-E5p:

- verdict: coding allowed, but only fail-closed contract coding；
- priority blocker is worker ORDER BY `PQOF` producer ownership；
- default heap reader, rowid tie-break, kill/detach/error policy, and visible
  gate must wait for this owner boundary；
- producer evidence may grow via existing smoke counters, but readiness flags
  must remain false。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `HAS_ORDER_BY` rejection changes；
- preflight readiness true flags；
- AccessPath, handler, InnoDB, worker launch, iterator `Read()`,
  `Query_result_mq`, default `Exchange_sort::read_mq_message()`, sysvar, or
  public counter changes；
- merging `PQOF` into `PQWR`；
- DESC / nullable / expression ORDER BY or visible ORDER BY support。

Implementation:

- added `PQ_orderby_worker_frame_producer_owner` to the public exchange_sort
  contract header；
- replaced the anonymous producer adapter shape with the owner contract；
- kept existing DBUG smoke entry points and counters；
- extended producer owner lifecycle helpers to reject detached producers；
- added detach and cleanup helpers；
- extended worker frame producer smoke to validate MQ DETACHED observation,
  post-detach emit rejection, cleanup flag, null handle, and idempotent cleanup；
- did not alter frame format, preflight readiness, optimizer eligibility, or
  default execution paths。

Validation:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`；
- full `parallel_query` suite。

Completion Report - M11-E5p:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_worker_frame_producer_owner` to `exchange_sort.h`；
  - replaced the anonymous `.cc` producer adapter shape with the owner contract；
  - kept existing DBUG smoke entry points and counters；
  - added owner detach and cleanup helpers；
  - extended ROW / FINISH / ERROR helpers to reject detached owners；
  - extended worker frame producer smoke to validate MQ DETACHED observation,
    post-detach emit rejection, cleanup flag, null handle, and idempotent
    cleanup；
  - did not alter `PQOF` frame format, `PQWR`, preflight readiness, optimizer
    eligibility, or default execution paths。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed: 89/89。
- residual risk:
  - this is still a controlled producer owner contract；
  - real worker row production, real sort-key generation, default worker launch,
    default heap reader, rowid tie-break, wait/kill/detach policy, and visible
    ORDER BY eligibility remain blocked。

Code/Doc/Test Review - M11-E5p:

- Review Agent verdict: `ACCEPT`；
- no blocking findings；
- confirmed `PQ_orderby_worker_frame_producer_owner` is now the shared owner
  shape in `exchange_sort.h`；
- confirmed ROW / FINISH / ERROR helpers reject null, finished, and detached
  owners, and decreasing key rejection remains；
- confirmed DETACH / CLEANUP smoke covers `MQ_DETACHED`, post-detach reject,
  null handle, cleanup flag, and idempotent cleanup；
- confirmed `PQOF` frame format is unchanged and remains separate from `PQWR` /
  `Query_result_mq`；
- confirmed `read_mq_message()`, `HAS_ORDER_BY`, and preflight readiness remain
  fail-closed；
- confirmed no public readiness counter, sysvar, optimizer, iterator, handler,
  InnoDB, or default execution path changes。

### M11-E5q: Default Exchange_sort Heap Reader Integration Contract

Status: committed as `c909a0efd19`；Code/Doc/Test Review accepted。

Goal:

- add a default-path-shaped heap reader helper contract inside `Exchange_sort`；
- keep the helper disabled by default；
- in controlled smoke, compose worker `PQOF` producer owner, typed loader, owned
  heap reader, and E5m rich status；
- prove `ROW`, `EOF_REACHED`, `WOULD_BLOCK`, `ERROR`, `DETACHED`, and cleanup
  semantics without opening visible ORDER BY PQ；
- keep preflight `exchange_sort_heap_read_ready` and
  `default_ordered_read_ready` false。

Design Explorer - M11-E5q:

- verdict: coding allowed, but only fail-closed contract coding；
- helper must be default-path compatible but disabled unless a private smoke
  flag is enabled；
- controlled/DBUG smoke may use scalar sort keys and `PQOF` producer owner；
- real Filesort sort-key generation, rowid tie-break, wait/kill policy,
  materializer default path, and iterator `Read()` remain blocked。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- this taskbook and `Docs/pq_tasks/README.md`。

Forbidden:

- `pq_optimizer.*` readiness, `HAS_ORDER_BY`, or `execution_disabled` changes；
- AccessPath, executor, iterator `Read()`, `Gather_operator::init()`,
  `Query_result_mq`, `PQWR`, handler/InnoDB, worker launch, sysvar, public
  counter, or visible ORDER BY changes；
- claiming scalar / byte-vector sort keys are equivalent to real Filesort keys。

Implementation:

- added private `m_orderby_default_heap_reader_shape_enabled`, default false；
- added private `read_default_ordered_record_heap_shape()`；
- disabled helper returns `DISABLED`；
- enabled helper requires E5m rich status flag, otherwise returns
  `UNSUPPORTED`；
- controlled path delegates to E5m rich status and cleans up owned gather state
  on `EOF_REACHED`, `ERROR`, and `DETACHED`；
- added `run_orderby_default_heap_reader_contract_smoke()`；
- smoke uses E5p worker producer owner to emit worker `PQOF` frames and covers
  disabled, unsupported, would-block, ordered rows, EOF cleanup, ERROR cleanup,
  DETACHED cleanup, and repeat post-cleanup disabled behavior；
- wired the smoke through existing ordered-reader skeleton smoke；
- did not add counters or MTR files。

Validation:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR:
  `pq_commercial_order_by pq_commercial_order_by_frames pq_stats`
  plus `shutdown_report` passed；
- full `parallel_query` suite passed: 89/89。

Completion Report - M11-E5q:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added private `m_orderby_default_heap_reader_shape_enabled`；
  - added `read_default_ordered_record_heap_shape()`；
  - disabled helper returns `DISABLED`；
  - enabled helper requires E5m rich status and otherwise returns
    `UNSUPPORTED`；
  - terminal `EOF_REACHED`, `ERROR`, and `DETACHED` statuses clean up owned
    gather state before returning the observed status；
  - added `run_orderby_default_heap_reader_contract_smoke()`；
  - smoke composes E5p producer owner, typed `PQOF` loader, owned heap reader,
    and E5m rich status；
  - smoke covers disabled, unsupported, would-block, ordered ROWs, EOF cleanup,
    ERROR cleanup, DETACHED cleanup, and post-cleanup disabled behavior；
  - no optimizer, preflight readiness, iterator, `Gather_operator`,
    `Query_result_mq`, handler/InnoDB, sysvar, or public counter changes。
- residual risk:
  - this is still controlled heap-reader evidence；
  - real default iterator integration, rowid/ref tie-break, wait/kill policy,
    real Filesort key generation, and visible ORDER BY eligibility remain
    blocked。

Code/Doc/Test Review - M11-E5q:

- Review Agent verdict: `ACCEPT`；
- no findings；
- confirmed changed-file scope is limited to `exchange_sort.*` plus task docs；
- confirmed the helper is default disabled and returns `DISABLED` unless the
  private smoke flag is enabled；
- confirmed rich-status-disabled path returns `UNSUPPORTED`；
- confirmed `EOF_REACHED`, `ERROR`, and `DETACHED` preserve the observed
  terminal status while cleaning owned gather state；
- confirmed `WOULD_BLOCK` does not clean cached rows or turn into EOF；
- confirmed controlled smoke composes E5p producer owner, typed loader, owned
  heap reader, and E5m rich status；
- confirmed no optimizer, preflight readiness, `HAS_ORDER_BY`, iterator
  `Read()`, `Gather_operator`, `Query_result_mq`, `PQWR`, handler/InnoDB,
  sysvar, public counter, or visible ORDER BY changes；
- confirmed materializer default remains unchanged and visible ORDER BY PQ
  remains blocked。

### M11-E5r: Rowid Duplicate-key Tie-break Inventory

Status: committed as `7f6eeda9038`；Docs/Source Review accepted。

Goal:

- decide whether ORDER BY equal-key tie-break is ready for fail-closed coding；
- compare the current controlled `row_id` byte-vector rule with the commercial
  handler `ref` / `cmp_ref()` contract；
- document the exact gaps before any `rowid_tiebreak_ready` work；
- keep visible ORDER BY PQ blocked。

Design/Source Review input:

- independent Review Agent verdict: `REVISE` for direct coding；
- recommendation: split E5r into design-only/read-only inventory first；
- reason: current branch has rowid/ref/tie-break shapes, but not a real
  executable handler `ref` contract。

Current branch inventory:

- `PQ_orderby_cached_record` stores `row_id` as an owned byte-vector；
- `PQ_orderby_frame_header` carries `row_id_len` and `sort_key_len`；
- `PQ_orderby_sort_state_shape` and
  `PQ_orderby_real_init_state_shape` carry `ref_length`,
  `stable_output`, and `rowid_required`；
- cached/synthetic comparator currently compares:
  - byte-vector `sort_key`；
  - byte-vector `row_id` when sort keys are equal；
  - `worker_id` as final deterministic fallback；
- controlled smokes cover duplicate scalar keys and expected rowid order；
- controlled heap reader still requires `sort_key.size() == sizeof(int64)`；
- `pq_build_orderby_execution_preflight()` keeps
  `rowid_tiebreak_ready=false`,
  `exchange_sort_heap_read_ready=false`, and
  `default_ordered_read_ready=false`；
- existing `HAS_ORDER_BY` serial boundary remains the user-visible gate。

Commercial reference inventory:

- commercial `Exchange_sort` stores MQ records carrying `m_row_id` and
  `m_sort_key`, and owns `Sort_param *`, `handler *m_file`, `Filesort *`,
  and heap state；
- commercial `Exchange_sort::init()` builds real Filesort sort order and
  `Sort_param`；
- stable mode validates `ref_length == m_file->ref_length`；
- commercial `heap_compare_records()` compares real Filesort keys first；
- when keys are equal and stable output is required, commercial code uses
  `handler::cmp_ref(row_id_0, row_id_1) < 0`；
- commercial worker rowid source is handler `file->ref` populated after
  `file->position(record)` in worker scan；
- commercial MQ sends handler ref bytes using `file->ref_length` for stable
  output。

Decision:

- do not code rowid tie-break in E5r；
- byte-vector `row_id` ordering is not sufficient evidence for handler
  `cmp_ref()` semantics；
- scalar sort-key smokes are useful test guards, but cannot be claimed as real
  Filesort key or real rowid/ref ordering；
- keep E5r as a design checkpoint before any handler-ref comparator coding。

Required next split before coding:

- E5r-1: handler `ref` ownership contract design；
  - worker-side `position(record)` call point；
  - rowid/ref lifetime in MQ frame；
  - leader-side handler object used for `cmp_ref()`；
  - `ref_length` source and validation。
- E5r-2: controlled `cmp_ref()` comparator contract, only if E5r-1 is
  accepted；
  - private flag only；
  - no preflight readiness changes；
  - no default ORDER BY SQL path；
  - no claim that byte-vector order equals handler ref order。

Allowed files for E5r documentation:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Allowed files for a later reviewed E5r-2 coding task:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- the same task docs；
- optional focused MTR only after design review accepts the contract。

Forbidden until later review:

- `sql/parallel_query/pq_optimizer.*` readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- AccessPath, executor, iterator `Read()`, `Gather_operator`, worker launch,
  `Query_result_mq`, `PQWR`, handler/InnoDB, sysvar, or public counter changes；
- default `ParallelScanIterator::Read()` ORDER BY path；
- treating scalar sort-key or byte-vector rowid order as equivalent to real
  Filesort key + handler `cmp_ref()`。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- `default_ordered_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- normal ORDER BY SQL must not increase executed / worker / range counters。

Minimum validation for future E5r-2 coding:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_order_by_frames pq_commercial_order_by pq_stats --parallel=1 --vardir=/tmp/pqv_m11e5r --tmpdir=/tmp/pqt_m11e5r`。

Completion Report - M11-E5r design inventory:

- changed files:
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - no source code changes；
  - captured independent Review Agent `REVISE` conclusion for direct coding；
  - documented current controlled byte-vector rowid behavior；
  - documented commercial handler `ref` / `cmp_ref()` behavior；
  - split next work into E5r-1 handler ref ownership design and possible
    E5r-2 controlled comparator contract。
- validation:
  - docs-only; `git diff --check` required before commit；
  - no build/MTR required unless source or test files are changed。
- residual risk:
  - real handler `cmp_ref()` tie-break remains unimplemented in current branch；
  - visible ORDER BY PQ remains blocked by design。

Docs/Source Review - M11-E5r:

- Review Agent verdict: `ACCEPT`；
- findings: no critical or important issues；
- minor wording fix applied: commercial `m_row_id` / `m_sort_key` are carried
  by MQ records managed by `Exchange_sort`, not direct `Exchange_sort` scalar
  members；
- confirmed current branch remains controlled byte-vector `row_id` / scalar
  `sort_key` only and does not claim equivalence to handler `cmp_ref()`；
- confirmed commercial reference uses real Filesort / `Sort_param` key compare
  and `handler::cmp_ref()` for equal-key stable output；
- confirmed worker handler ref source depends on `position(record)` and
  `file->ref` / `file->ref_length`；
- confirmed hard gates remain documented:
  `rowid_tiebreak_ready=false`, `exchange_sort_heap_read_ready=false`,
  `default_ordered_read_ready=false`, and `HAS_ORDER_BY` serial rejection；
- confirmed docs-only scope is safe to commit。

### M11-E5s: Wait / Kill / Detach / Error Policy Inventory

Status: committed as `84a387f4674`；Code/Doc/Test Review accepted。

Goal:

- define the ORDER BY `Exchange_sort` wait/kill/detach/error policy before
  any default ordered read path；
- compare current controlled reader states with the commercial blocking wait
  and kill/error behavior；
- decide whether the next coding step can be limited to private diagnostics
  and controlled smoke；
- keep visible ORDER BY PQ blocked。

Current branch inventory:

- `read_ordered_record_stream_shape()` distinguishes:
  - `ROW`；
  - `EOF_REACHED`；
  - `WOULD_BLOCK`；
  - `DETACHED`；
  - `ERROR`。
- `run_orderby_streaming_heap_read_smoke()` and
  `run_orderby_ordered_reader_skeleton_smoke()` already cover:
  - `WOULD_BLOCK` without producing a row or EOF；
  - ordered rows after the missing worker produces data；
  - `EOF_REACHED` after all workers finish；
  - `ERROR` frame propagation；
  - producer detach propagation；
  - per-state counters for finishes, would-blocks, errors, detaches, refills,
    heap replace, and heap remove。
- `run_orderby_ordered_diag_skeleton_smoke()` only reports
  `kill_not_wired=1`；
- existing `Parallel_exchange_sort_ordered_diag_kill_not_wired` status is a
  negative diagnostic, not readiness evidence；
- `pq_build_orderby_execution_preflight()` keeps
  `kill_detach_error_diagnostics_ready=false`；
- visible ORDER BY SQL remains blocked by `HAS_ORDER_BY` and by the central
  execution preflight blocker。

Commercial reference inventory:

- commercial MQ event wait loops check `thd->is_killed()` while spinning and
  waiting；
- commercial `Exchange_nosort::read_next()` loops while
  `!thd->is_killed() && !thd->pq_error`；
- commercial reader waits on receiver event after a full round with no data；
- commercial MQ send/receive loops return `MQ_DETACHED` on PQ error or detached
  status；
- commercial `Exchange_sort::load_group_record()` treats `MQ_DETACHED` as a
  completed worker, `MQ_WOULD_BLOCK` as temporary no data, and otherwise loads
  the message；
- commercial `Exchange_sort::get_min_record()` can block while building the
  initial heap to ensure every active worker has an initial record or terminal
  state。

Decision draft:

- do not open default ORDER BY `Read()` in E5s；
- do not set `kill_detach_error_diagnostics_ready=true`；
- do not wire real THD kill polling into ORDER BY default path yet；
- the next coding task, if accepted by review, should be limited to private
  diagnostics describing the current missing kill/wait pieces and proving that
  `WOULD_BLOCK`, `DETACHED`, and `ERROR` stay distinguishable through the
  default-heap-reader-shaped helper；
- if Review finds the current diagnostics already sufficient, E5s should remain
  docs-only and the next coding split should be handler-ref or real wait-owner
  design。

Allowed files for E5s design:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Allowed files for a later reviewed private-diagnostic coding step:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- the same task docs；
- focused `pq_commercial_order_by_frames` MTR only if a new private smoke
  counter is added。

Forbidden:

- optimizer preflight readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- default `ParallelScanIterator::Read()` ORDER BY path；
- `Gather_operator::init()` default `Exchange_sort` selection；
- handler/InnoDB, worker launch, PQWR, `Query_result_mq`, AccessPath, executor,
  sysvar, or public visible SQL behavior changes；
- treating `kill_not_wired` as readiness。

Hard gates:

- `kill_detach_error_diagnostics_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- `default_ordered_read_ready=false`；
- `rowid_tiebreak_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- normal ORDER BY SQL must not increase executed / worker / range counters。

Minimum validation for any E5s private-diagnostic coding:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  `TMPDIR=/tmp perl build-ninja/mysql-test/mysql-test-run.pl --suite=parallel_query pq_commercial_order_by_frames pq_commercial_order_by pq_stats --parallel=1 --vardir=/tmp/pqv_m11e5s --tmpdir=/tmp/pqt_m11e5s`；
- full `parallel_query` suite before commit if source changes touch
  `exchange_sort.*`。

Review questions:

- should E5s remain design-only because current negative diagnostics already
  cover `kill_not_wired`；
- or should E5s add a private diagnostic contract that reports:
  `would_block_distinct_from_eof`, `detach_cleans_up`, `error_cleans_up`,
  `kill_polling_not_wired`；
- whether such a diagnostic belongs in source smoke only or should add MTR
  counters。

Design/Source Review - M11-E5s:

- Review Agent verdict: `ACCEPT` for extremely narrow fail-closed coding；
- allowed only private diagnostic state / private DBUG smoke / optional smoke
  counters；
- forbidden to implement real wait/kill or set any ORDER BY default-path
  readiness flag；
- confirmed current branch already distinguishes `WOULD_BLOCK`, `DETACHED`,
  and `ERROR` in private ORDER BY reader statuses；
- confirmed kill remains explicit `kill_not_wired` and must not be reported as
  ready；
- confirmed commercial wait/kill/error policy spans MQ event wait,
  `thd->is_killed()`, `pq_error`, worker ERROR, detach, and iterator `Read()`
  checks, so it cannot be implemented inside `Exchange_sort` alone。

Implementation - M11-E5s:

- added private `PQ_orderby_ordered_diag_contract_shape`；
- added private `run_orderby_ordered_diag_contract_smoke()`；
- the private diagnostic contract verifies:
  - `WOULD_BLOCK` is distinct from EOF and preserves the default heap reader
    state；
  - `ERROR` returns terminal ERROR and then cleanup makes the default helper
    return `DISABLED`；
  - `DETACHED` returns terminal DETACHED and then cleanup makes the default
    helper return `DISABLED`；
  - kill polling is still not wired；
  - default ordered read is still not ready；
  - diagnostics readiness remains false。
- `run_orderby_ordered_diag_skeleton_smoke()` now calls the private diagnostic
  contract and still returns only `kill_not_wired=1` to the existing DBUG smoke
  counter path；
- no new public counters, sysvars, MTR files, optimizer changes, preflight
  readiness changes, `HAS_ORDER_BY` changes, default `read_mq_message()`
  changes, iterator changes, handler/InnoDB changes, PQWR changes, or
  `Query_result_mq` changes。

Validation - M11-E5s:

- `git diff --check` passed；
- `cmake --build build-ninja --target mysqld -j 16` passed；
- targeted MTR passed:
  `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`
  plus `shutdown_report`；
- full `parallel_query` suite passed: 89/89。

Completion Report - M11-E5s:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added private diagnostic shape and smoke；
  - strengthened existing ordered diagnostic smoke without widening public
    observability；
  - preserved all fail-closed default gates。
- residual risk:
  - real THD kill polling, blocking wait policy, worker-thread error
    propagation, and default ordered `Read()` remain unimplemented；
  - `kill_detach_error_diagnostics_ready` remains false by design。

Code/Doc/Test Review - M11-E5s:

- Review Agent verdict: `ACCEPT`；
- findings: none；
- confirmed `PQ_orderby_ordered_diag_contract_shape` and
  `run_orderby_ordered_diag_contract_smoke()` are private smoke-only helpers；
- confirmed the diagnostic contract covers `WOULD_BLOCK` distinct from EOF,
  `ERROR` cleanup to `DISABLED`, `DETACHED` cleanup to `DISABLED`, and keeps
  kill/default-read/diagnostics readiness false；
- confirmed `run_orderby_ordered_diag_skeleton_smoke()` still only exposes
  `kill_not_wired=1` through the existing DBUG diagnostic path；
- confirmed no `pq_optimizer.*`, `HAS_ORDER_BY`, default `read_mq_message()`,
  iterator `Read()`, `Gather_operator`, `Query_result_mq`, PQWR,
  handler/InnoDB, sysvar, or public counter changes；
- confirmed fail-closed preflight remains intact:
  `default_ordered_read_ready=false` and
  `kill_detach_error_diagnostics_ready=false`；
- full `parallel_query` suite passed after review: 89/89。

### M11-E5r-1: Handler Ref Ownership Contract Inventory

Status: committed as `af2ae4f9d10`；Design/Source Review accepted。

Goal:

- document the ownership contract for handler `ref` bytes before any
  `cmp_ref()` tie-break coding；
- compare current controlled `PQOF` row_id bytes with commercial worker
  `file->position(record)` / `file->ref` semantics；
- identify whether the current branch has a safe leader handler object for
  `handler::cmp_ref()`；
- keep `rowid_tiebreak_ready=false` and visible ORDER BY PQ blocked。

Current branch inventory:

- `PQ_orderby_frame_header` carries `row_id_len` but does not encode whether
  bytes are handler ref bytes or synthetic smoke row ids；
- `PQ_orderby_cached_record::row_id` is an owned byte-vector copied from
  `PQOF` frames；
- `pq_send_orderby_frame()` accepts caller-provided `row_id` / `row_id_len`
  and deep-copies the payload into MQ；
- `pq_orderby_worker_frame_producer_owner_emit_row()` emits caller-provided
  rowid bytes and only enforces producer-local sort-key monotonicity；
- current ORDER BY smokes pass small `uint32` row ids, not handler
  `file->ref` bytes；
- current `PQblockScanIterator::Read()` is still a skeleton returning `1` and
  does not call `table()->file->position(record)`；
- `PQblockScanIterator` stores `m_need_rowid`, but there is no current
  ORDER BY worker path that converts it into handler ref bytes；
- current `Query_result_mq` has stable-output shape, but its current
  `send_data()` path does not send `file->ref` / `file->ref_length` for ORDER
  BY stable output；
- default `Gather_operator` still creates `Exchange_nosort`; ORDER BY
  `Exchange_sort` evidence comes from local smoke objects and controlled
  helpers；
- current `Exchange_sort` comparator still orders duplicate scalar keys by
  byte-vector row_id and worker id, not `handler::cmp_ref()`；
- current runtime-owner shapes track `ref_length`, `stable_output`, and
  `rowid_required`, but do not bind a real handler pointer for comparator use。

Commercial reference inventory:

- commercial `PQblockScanIterator::Read()` calls
  `table()->file->position(m_record)` when `m_need_rowid` is true；
- commercial `Query_result_mq` sends `file->ref` with length
  `file->ref_length` for stable output；
- commercial `Exchange_sort::init()` validates stable output with
  `ref_length == m_file->ref_length`；
- commercial `Exchange_sort` stores MQ records carrying handler ref bytes in
  `m_row_id`；
- commercial `heap_compare_records()` compares real Filesort keys first and
  uses `m_file->cmp_ref(row_id_0, row_id_1) < 0` for equal-key stable output；
- InnoDB `position(record)` writes either generated row id bytes or primary-key
  bytes into `handler::ref` depending on table shape；
- InnoDB `cmp_ref()` is type-aware for primary-key refs and byte-comparison for
  generated row ids。

Risk conclusions:

- synthetic `uint32` row ids are not interchangeable with handler ref bytes；
- byte-vector row-id order is not equivalent to `handler::cmp_ref()` for
  primary-key refs；
- the leader-side comparator must use a handler associated with the same table
  shape and `ref_length` as the worker-produced refs；
- worker and leader handler instances may differ, so the contract must be
  expressed in terms of stable table metadata and `cmp_ref()` compatibility,
  not pointer identity；
- handler ref bytes must be deep-copied before the worker advances or reuses
  handler buffers；
- partitioned InnoDB refs include partition bytes, so `ref_length` validation
  is mandatory before any comparator coding。

Decision draft:

- keep M11-E5r-1 design-only/read-only；
- do not add a `cmp_ref()` comparator in E5r-1；
- do not attempt fail-closed comparator coding until worker `position(record)`,
  MQ ref deep-copy format, leader handler ownership, `ref_length` validation,
  and handler instance compatibility are separately reviewed；
- do not set `rowid_tiebreak_ready=true`；
- require a later E5r-2 coding task to use a private comparator contract only,
  with explicit handler/ref-length inputs and no default ORDER BY path。

Allowed files for E5r-1:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Allowed files for a later reviewed E5r-2 coding task:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- docs；
- optional focused MTR only if a DBUG smoke counter or visible test guard is
  added after review。

Forbidden:

- `pq_optimizer.*` readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- default `Exchange_sort::read_mq_message()` changes；
- default ordered `ParallelScanIterator::Read()` changes；
- worker launch, `Query_result_mq`, PQWR, handler/InnoDB, AccessPath,
  executor, sysvar, or public counter changes；
- claiming current `uint32` rowid smokes prove real handler ref ordering。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- normal ORDER BY SQL must not increase executed / worker / range counters。

Minimum validation for E5r-1:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Design/Source Review - M11-E5r-1:

- Review Agent verdict: `ACCEPT` for design-only/read-only inventory；
- Review Agent verdict: `BLOCKED` for any current fail-closed comparator
  coding；
- confirmed current `Exchange_sort` has no handler ownership field and only
  stores controlled `PQOF` row_id byte-vectors；
- confirmed current `Query_result_mq` does not send handler `file->ref` /
  `file->ref_length` for ORDER BY stable output；
- confirmed current worker ORDER BY producer smokes use synthetic `uint32`
  rowids；
- confirmed current `PQblockScanIterator::Read()` does not call
  `position(record)`；
- confirmed default Gather still uses `Exchange_nosort`, not default
  `Exchange_sort`；
- confirmed leader `TABLE::file` / InnoDB `cmp_ref()` exist, but the branch has
  not proven worker ref bytes come from the same table/ref-length contract；
- required next split before `cmp_ref()` coding:
  - worker `position(record)` call point；
  - MQ ref deep-copy format；
  - leader handler ownership；
  - `ref_length` validation；
  - handler instance compatibility。

### M11-E5r-2-pre: Worker position(record) and MQ Handler Ref Format Contract

Status: committed as `3c6dbfd8cd8`；Docs/Source Review accepted。

Goal:

- define the preconditions for moving from synthetic `PQOF` row_id bytes to
  handler `ref` bytes；
- identify the future worker row-production hook where `position(record)` may
  be called；
- define a safe MQ handler-ref deep-copy contract；
- keep `cmp_ref()` comparator coding blocked until this contract is reviewed；
- keep visible ORDER BY PQ blocked。

Design/Source Review input:

- Review Agent verdict: `ACCEPT` for design-only pre-task；
- direct `cmp_ref()` comparator remains forbidden；
- if coding is considered later, only private metadata / validation helper is
  acceptable, and only after a separate review。

Current branch future-hook inventory:

- `PQ_row_sink` documents that SQL sink implementations must deep-copy worker
  row image before `send_row()` returns；
- `PQ_limited_mq_row_sink::send_row()` is the closest SQL-layer callback row
  production point；it currently sends only row image through
  `Exchange_nosort::enqueue_record_image()`；
- `Exchange_nosort::enqueue_record_image()` deep-copies record image to MQ and
  is a useful no-ref contrast point；
- `PQ_orderby_frame_header` already has `row_id_len` and payload layout for
  ORDER BY `PQOF` frames；
- `pq_send_orderby_frame()` deep-copies `record_image`, `row_id`, and
  `sort_key` into the MQ message, but does not prove `row_id` is handler ref；
- `PQblockScanIterator::Read()` remains a skeleton and must not be modified in
  this pre-task；
- `Query_result_mq::send_data()` currently does not send `file->ref` /
  `file->ref_length` in this branch and must not be modified in this pre-task；
- `Gather_operator::init()` still defaults to `Exchange_nosort` and must not
  select `Exchange_sort` for normal SQL；
- `ParallelScanIterator::Read()` remains the nosort materialization path and
  must not be connected to ORDER BY。

Commercial reference chain:

1. Worker `PQblockScanIterator::Read()` successfully reads a worker record；
2. if `m_need_rowid` is true, worker calls
   `table()->file->position(m_record)`；
3. InnoDB `position(record)` writes either primary-key bytes or generated
   row-id bytes into handler-owned `file->ref`；
4. commercial `Query_result_mq::send_data()` includes `file->ref` with length
   `file->ref_length` when stable output is required；
5. commercial `Exchange_sort::store_mq_record()` deep-copies MQ ref bytes into
   cached `m_row_id`；
6. commercial `heap_compare_records()` compares real Filesort key first, then
   calls `file->cmp_ref(row_id_0, row_id_1) < 0` for equal-key stable output。

Contract requirements:

- worker may call `position(record)` only after a successful row read；
- `record` must be the worker handler's current row image, matching
  `TABLE::record[0]` / iterator-owned record state；
- handler ref bytes must be deep-copied before the worker handler advances,
  detaches, or reuses handler buffers；
- ORDER BY frame metadata must distinguish handler ref bytes from synthetic
  smoke row ids；
- frame metadata must carry or validate expected `ref_length`；
- stable-output requirement must be explicit；
- leader comparator handler must be compatible with the worker-produced ref
  table shape and ref format；
- partitioned handler refs require mandatory `ref_length` validation；
- InnoDB primary-key refs are type-aware under `cmp_ref()`, so byte-vector
  lexical order must not be claimed equivalent。

Recommended next split:

- E5r-2-pre remains docs-only；
- a later reviewed E5r-2a may add private frame metadata shape and validation
  helper only；
- a later reviewed E5r-2b may add a private controlled validation smoke using
  synthetic bytes only to verify fail-closed metadata checks；
- actual worker `position(record)`, `Query_result_mq` wire-format changes,
  and `cmp_ref()` comparator must remain blocked until their own reviews。

Allowed files for E5r-2-pre:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Allowed files for a later reviewed private metadata task:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- docs；
- optional focused MTR only if a new DBUG-visible guard is added。

Forbidden:

- `cmp_ref()` comparator；
- `file->position(record)` calls；
- `Query_result_mq::send_data()` wire-format changes；
- `PQblockScanIterator::Read()` changes；
- `ParallelScanIterator::Read()` changes；
- `Gather_operator::init()` default `Exchange_sort` selection；
- `pq_optimizer.*` readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- handler/InnoDB, worker launch, AccessPath, executor, sysvar, or public
  counter changes；
- claiming synthetic row_id bytes are handler refs。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- `HAS_ORDER_BY` remains the ordinary SQL serial boundary；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- normal ORDER BY SQL must not increase executed / worker / range counters。

Validation for E5r-2-pre:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Design/Source Review - M11-E5r-2-pre:

- Review Agent verdict: `ACCEPT` for a design-only pre-task；
- confirmed future hook candidates:
  - `PQ_row_sink`；
  - `PQ_limited_mq_row_sink::send_row()`；
  - `Exchange_nosort::enqueue_record_image()` as no-ref contrast；
  - `PQOF` frame header and `pq_send_orderby_frame()` as private metadata
    shape candidates。
- confirmed still-forbidden current points:
  - `PQblockScanIterator::Read()`；
  - `Query_result_mq::send_data()`；
  - `Gather_operator::init()`；
  - `ParallelScanIterator::Read()`。
- confirmed the commercial chain from worker `position(record)` to
  `file->ref`, MQ ref deep-copy, `Exchange_sort::m_row_id`, and
  `handler::cmp_ref()`；
- recommended only design-only now；
- if later coding is approved, limit it to private frame metadata shape or
  validation helper；
- confirmed `Query_result_mq` and `pq_iterators` must remain forbidden in this
  pre-task。

Docs/Source Review - M11-E5r-2-pre:

- Review Agent verdict: `ACCEPT`；
- findings: no critical, important, or minor issues；
- confirmed the taskbook accurately records future hooks:
  - `PQ_row_sink`；
  - `PQ_limited_mq_row_sink::send_row()`；
  - `Exchange_nosort::enqueue_record_image()`；
  - `PQOF` header and `pq_send_orderby_frame()`。
- confirmed the taskbook keeps current forbidden points intact:
  - `PQblockScanIterator::Read()`；
  - `Query_result_mq::send_data()`；
  - `Gather_operator::init()`；
  - `ParallelScanIterator::Read()`。
- confirmed the commercial chain is documented as worker `Read()` to
  `position(record)`, handler `file->ref/ref_length`, MQ deep-copy,
  `Exchange_sort::m_row_id`, and `cmp_ref()` tie-break；
- confirmed the docs do not over-commit coding and keep future implementation
  limited to private metadata / validation helper after a separate review；
- confirmed hard gates remain explicit:
  - `rowid_tiebreak_ready=false`；
  - `default_ordered_read_ready=false`；
  - `exchange_sort_heap_read_ready=false`；
  - ordinary SQL ORDER BY remains blocked by the `HAS_ORDER_BY` serial boundary。
- scope note: tracked diff is docs-only；build/MTR is not required for this
  task。

### M11-E5r-2a: Private Row-id Metadata Validation Helper

Status: committed as `f8f27df3bc0`；Design/Source Review and Code/Doc/Test
Review accepted；full `parallel_query` suite passed。

Goal:

- add a private ORDER BY row-id metadata validation shape for `PQOF` decoded
  frames；
- keep synthetic smoke row ids distinct from future handler refs by requiring
  an explicit metadata contract；
- validate expected handler `ref_length` only as a private helper；
- keep `cmp_ref()`, worker `position(record)`, MQ wire-format changes, and
  visible ORDER BY PQ blocked。

Implementation shape:

- add `PQ_orderby_row_id_source` with:
  - `NONE`；
  - `SYNTHETIC_SMOKE`；
  - `HANDLER_REF`。
- add `PQ_orderby_row_id_contract` containing:
  - row-id source；
  - expected handler `ref_length`；
  - whether stable output requires row-id bytes。
- add `pq_validate_orderby_row_id_contract()` for decoded ROW frames only；
- wire the helper into existing controlled
  `Exchange_sort::run_orderby_frame_contract_smoke()`；
- verify negative metadata cases without extending the current `PQOF` wire
  header or adding public counters。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- existing `pq_commercial_order_by_frames` test/result only if output changes。

Forbidden:

- `cmp_ref()` comparator；
- `file->position(record)` calls；
- `Query_result_mq::send_data()` wire-format changes；
- `PQblockScanIterator::Read()`；
- `ParallelScanIterator::Read()`；
- `Gather_operator::init()` default `Exchange_sort` selection；
- `pq_optimizer.*` readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- handler/InnoDB, worker launch, AccessPath, executor, sysvar, or public
  counter changes。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- synthetic row-id bytes must not be described as handler refs。

Validation plan:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  - `pq_commercial_order_by_frames`；
  - `pq_commercial_order_by`；
  - `pq_stats`。
- full `parallel_query` suite if the source patch survives review。

Design/Source Review - M11-E5r-2a:

- Review Agent verdict: `ACCEPT`；
- confirmed E5r-2a may be coded only as a fail-closed private
  metadata/validation shape inside `exchange_sort.*`；
- confirmed source semantics must remain outside the `PQOF` wire header and
  must be passed as an explicit contract；
- confirmed `flags` must not be repurposed from worker id to source metadata；
- confirmed the task must not enter comparator, worker, MQ default chain, or
  visible ORDER BY execution。

Completion Report - M11-E5r-2a Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_row_id_source` with `NONE`, `SYNTHETIC_SMOKE`, and
    `HANDLER_REF`；
  - added `PQ_orderby_row_id_contract` carrying source, expected
    `ref_length`, and stable-output requirement；
  - added `pq_validate_orderby_row_id_contract()` with fail-closed semantics
    for null/non-ROW frames, missing metadata, synthetic/ref-length mismatch,
    and handler-ref length mismatch；
  - wired the helper into the existing controlled
    `run_orderby_frame_contract_smoke()`；
  - kept `PQOF` header, wire format, and `flags` semantics unchanged；
  - did not add public counters or visible SQL behavior。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed 4/4:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed 89/89。
- scope notes:
  - no `cmp_ref()` comparator；
  - no `file->position(record)` call；
  - no `Query_result_mq` wire-format change；
  - no `PQblockScanIterator::Read()` / `ParallelScanIterator::Read()` /
    `Gather_operator::init()` default path change；
  - no optimizer readiness or `HAS_ORDER_BY` boundary change；
  - synthetic row-id bytes remain smoke-only and are not described as handler
    refs。

Code/Doc/Test Review - M11-E5r-2a:

- Review Agent verdict: `ACCEPT`；
- findings: no critical, important, or minor issues；
- confirmed diff only touches the allowed 4 files；
- confirmed helper uses the existing MySQL/PQ `true == error/fail` convention
  and remains fail-closed；
- confirmed no `PQOF` wire/header/flags change；
- confirmed synthetic smoke row ids are not connected to handler ref,
  `file->ref`, `cmp_ref()`, or the default ORDER BY path；
- requested full `parallel_query` suite before commit；this was completed and
  passed 89/89。

### M11-E5r-2b: Controlled Row-id Metadata Validation Smoke

Status: committed as `976885fb58c`；Code/Doc/Test Review accepted；full
`parallel_query` suite passed。

Goal:

- make the E5r-2a row-id metadata contract evidence explicit in a controlled
  private smoke；
- verify fail-closed behavior for `NONE`, `SYNTHETIC_SMOKE`, `HANDLER_REF`,
  ref-length mismatch, missing row-id, null frame, and non-ROW frame shapes；
- keep the smoke disconnected from real handler ref production, `cmp_ref()`,
  and visible ORDER BY PQ。

Implementation shape:

- add a private `pq_orderby_row_id_contract_controlled_smoke()` helper in
  `exchange_sort.cc`；
- call it from the existing
  `Exchange_sort::run_orderby_frame_contract_smoke()` path；
- reuse existing frame-smoke counters and MTR assertions；
- do not extend the `PQOF` wire header；
- do not add public status variables or counters；
- do not claim the handler-ref length-shape smoke proves real handler
  `file->ref` ordering。

Allowed files:

- `sql/parallel_query/exchange_sort.cc`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- `cmp_ref()` comparator；
- `file->position(record)` calls；
- `Query_result_mq::send_data()` wire-format changes；
- `PQblockScanIterator::Read()`；
- `ParallelScanIterator::Read()`；
- `Gather_operator::init()` default `Exchange_sort` selection；
- `pq_optimizer.*` readiness changes；
- `HAS_ORDER_BY` serial boundary changes；
- handler/InnoDB, worker launch, AccessPath, executor, sysvar, or public
  counter changes。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`；
- synthetic row-id bytes must not be described as handler refs。

Validation plan:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  - `pq_commercial_order_by_frames`；
  - `pq_commercial_order_by`；
  - `pq_stats`。
- full `parallel_query` suite if the source patch survives review。

Planning/Source Review - M11-E5r-2b:

- Review Agent verdict: `ACCEPT`；
- recommended E5r-2b as the next smallest safe task；
- confirmed it should be a controlled validation smoke, not E5r-3 worker
  `position(record)` design and not real handler ref production/comparator；
- confirmed coding is allowed only in `exchange_sort.*` and docs, with optional
  focused MTR update only if output changes；
- confirmed no public counter is needed；
- confirmed E5r-3 should remain a later docs/source design task。

Completion Report - M11-E5r-2b Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added private `pq_orderby_row_id_contract_controlled_smoke()`；
  - covered null frame and non-ROW frame fail-closed behavior；
  - covered stable output with `NONE` source and missing row-id fail-closed
    behavior；
  - covered synthetic smoke row-id success only when no handler `ref_length` is
    claimed；
  - covered handler-ref length-shape success and length mismatch failures；
  - reused existing `run_orderby_frame_contract_smoke()` and existing MTR
    counter assertions；
  - did not change `PQOF` header, wire format, or `flags` semantics；
  - did not add public counters/status variables。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed 4/4:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`
    plus `shutdown_report`；
  - full `parallel_query` suite passed 89/89。
- scope notes:
  - no `cmp_ref()` comparator；
  - no `file->position(record)` call；
  - no `Query_result_mq` wire-format change；
  - no `PQblockScanIterator::Read()` / `ParallelScanIterator::Read()` /
    `Gather_operator::init()` default path change；
  - no optimizer readiness or `HAS_ORDER_BY` boundary change；
  - no real handler ref ordering claim。

Code/Doc/Test Review - M11-E5r-2b:

- Review Agent verdict: `ACCEPT`；
- findings: no critical or important issues；
- minor finding: README and taskbook status were stale before final validation；
  fixed by this completion update；
- confirmed the patch stays within the allowed three files；
- confirmed the smoke is private and controlled；
- confirmed `PQOF` header/wire/flags are unchanged；
- confirmed no public counter/status is added；
- confirmed forbidden paths are not touched；
- requested full `parallel_query` suite before commit；this was completed and
  passed 89/89。

### M11-E5r-3a: Worker position(record) and MQ Ref Lifetime Design

Status: committed as `c191ac8d505`；Design/Source Review accepted。

Goal:

- document the current branch gaps before any worker `position(record)` coding；
- define the required worker-side preconditions for producing handler refs；
- define the MQ deep-copy and lifetime contract for future handler refs；
- define leader handler compatibility requirements before any future
  `cmp_ref()` tie-break；
- keep all real execution-path changes blocked。

Current branch findings:

- `PQblockScanIterator::Read()` remains a fail-closed skeleton and does not
  call `file->position(record)`；
- `Query_result_mq::send_data()` emits existing `PQWR` field-value frames and
  does not carry handler `file->ref` / `file->ref_length`；
- current `Exchange_sort` row-id handling is private and controlled through
  `PQOF` frames that deep-copy caller-provided bytes；
- current controlled compare still falls back to byte-vector row-id / worker-id
  ordering and does not call `handler::cmp_ref()`；
- commercial code confirms the missing stable-output chain is multi-step:
  worker read success, `position(record)`, MQ ref deep-copy, and leader
  `cmp_ref()` equal-key tie-break。

Worker `position(record)` preconditions:

- call only after a successful worker row read；
- `record` must be the worker handler's current row image；
- worker `TABLE`, handler, prebuilt state, read view, and iterator ownership
  must still be valid；
- do not call on detach, kill, EOF, or error paths；
- stable-output requirement must be explicit before producing refs；
- handler `ref_length` must be known and nonzero before any ref bytes are sent。

Handler ref lifetime contract:

- `file->ref` is handler-owned mutable storage；
- bytes are valid only until the handler advances, reuses buffers, detaches, or
  is destroyed；
- any future MQ path must deep-copy exactly `file->ref_length` bytes before the
  worker continues；
- partitioned or engine-specific refs require mandatory `ref_length`
  validation；
- synthetic smoke row ids must remain distinguishable from handler refs。

Future MQ contract requirements:

- do not overload existing `PQWR` field-value frames without a separate wire
  review；
- do not overload `PQOF::flags`, which currently carries worker id in the
  controlled ORDER BY frame path；
- future metadata must explicitly distinguish handler refs from synthetic
  smoke row ids；
- future metadata must carry or validate expected `ref_length`；
- worker ref deep-copy must happen before worker handler state advances。

Leader handler compatibility requirements:

- comparator must use a handler compatible by engine, table shape, ref format,
  and `ref_length`；
- compatibility is not established by pointer identity alone；
- InnoDB `cmp_ref()` is type-aware for primary-key refs and byte-comparison only
  for generated row-id refs；
- byte-vector row-id ordering must not be claimed equivalent to
  `handler::cmp_ref()`；
- any future comparator task must prove leader handler lifetime and table shape
  ownership independently。

Recommended next split:

- E5r-3a remains docs-only；
- E5r-3b may later add a private non-default validation/shape helper only after
  separate review；
- real `position(record)`, `Query_result_mq` wire-format ref deep-copy, and
  `cmp_ref()` comparator tasks remain blocked until their own design and review
  gates。

Allowed files for E5r-3a:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- `PQblockScanIterator::Read()` real path changes；
- `Query_result_mq::send_data()` wire-format changes；
- `Exchange_sort` `cmp_ref()` comparator；
- `file->position(record)` calls；
- `Gather_operator::init()` default `Exchange_sort` selection；
- `ParallelScanIterator::Read()` ORDER BY path；
- `pq_optimizer.*` readiness or `HAS_ORDER_BY` serial-boundary relaxation；
- handler/InnoDB, AccessPath, worker launch, executor, sysvar, or public
  counter changes。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`。

Validation for E5r-3a:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Design/Source Review - M11-E5r-3a:

- Review Agent verdict: `ACCEPT` for design-only；
- confirmed current `PQblockScanIterator::Read()` has no `position(record)`
  call；
- confirmed current `Query_result_mq::send_data()` does not carry handler refs；
- confirmed current `Exchange_sort` row-id handling remains private/controlled
  and does not call `cmp_ref()`；
- confirmed E5r-3b, if any, must remain private non-default validation/shape
  only；
- confirmed real execution/comparator coding remains blocked。

### M11-E5r-3b: Private Handler-ref Lifetime Validation Shape

Status: committed as `ed0bfc591b5`；focused MTR passed；Code/Doc/Test
Re-review accepted。

Goal:

- encode the E5r-3a handler-ref lifetime requirements as a private validation
  shape；
- require handler-ref metadata to prove source, expected `ref_length`,
  deep-copy, current worker record, and no detach/advance risk；
- keep the helper disconnected from real worker `position(record)`, MQ ref
  wire, `cmp_ref()`, and visible ORDER BY PQ。

Implementation shape:

- add `PQ_orderby_handler_ref_lifetime_contract`；
- add `pq_validate_orderby_handler_ref_lifetime_contract()`；
- reuse `PQ_orderby_row_id_contract` and
  `pq_validate_orderby_row_id_contract()`；
- validate the shape through existing private controlled smoke；
- do not read handler `file->ref`；
- do not call `file->position(record)`；
- do not change `PQOF` or `PQWR` wire headers；
- do not add public status variables or counters。

Allowed files:

- `sql/parallel_query/exchange_sort.h`；
- `sql/parallel_query/exchange_sort.cc`；
- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- worker `file->position(record)` calls；
- `Query_result_mq::send_data()` carrying refs or changing `PQWR` wire；
- `Exchange_sort` `handler::cmp_ref()` comparator；
- `PQblockScanIterator::Read()` / `ParallelScanIterator::Read()` ORDER BY real
  paths；
- `Gather_operator::init()` default `Exchange_sort` selection；
- `pq_optimizer.*` readiness or `HAS_ORDER_BY` serial-boundary relaxation；
- handler/InnoDB, AccessPath, worker launch, executor, sysvar, or public
  counter changes；
- user-visible ORDER BY PQ。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`。

Validation plan:

- `git diff --check`；
- `cmake --build build-ninja --target mysqld -j 16`；
- targeted MTR:
  - `pq_commercial_order_by_frames`；
  - `pq_commercial_order_by`；
  - `pq_stats`。
- full `parallel_query` suite only if Code/Doc/Test Review requests it。

Planning/Source Review - M11-E5r-3b:

- Review Agent verdict: `ACCEPT`；
- allowed a very narrow private non-default validation/shape helper；
- recommended implementation only in `exchange_sort.*` and docs；
- recommended not touching `query_result_mq.*` or `pq_iterators.*`；
- confirmed compile and focused MTR are enough if the helper is wired only into
  existing controlled smoke；
- confirmed real handler-ref production, MQ ref wire, `cmp_ref()`, and visible
  ORDER BY PQ remain blocked。

Completion Report - M11-E5r-3b Coding:

- changed files:
  - `sql/parallel_query/exchange_sort.h`；
  - `sql/parallel_query/exchange_sort.cc`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- implementation:
  - added `PQ_orderby_handler_ref_lifetime_contract`；
  - added `pq_validate_orderby_handler_ref_lifetime_contract()`；
  - required `HANDLER_REF`, nonzero expected `ref_length`, stable output,
    verified length, deep copy, current worker record, no handler advance, and
    no worker detach；
  - reused `pq_validate_orderby_row_id_contract()`；
  - extended only the existing private controlled smoke；
  - did not read handler `file->ref`；
  - did not call `file->position(record)`；
  - did not change `PQOF` or `PQWR` wire headers；
  - did not add public status variables or counters。
- validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - targeted MTR passed 4/4:
    `pq_commercial_order_by_frames pq_commercial_order_by pq_stats`
    plus `shutdown_report`。
- scope notes:
  - no `cmp_ref()` comparator；
  - no `Query_result_mq` ref wire-format change；
  - no `PQblockScanIterator::Read()` / `ParallelScanIterator::Read()` /
    `Gather_operator::init()` default path change；
  - no optimizer readiness or `HAS_ORDER_BY` boundary change；
  - no real handler ref ordering claim。

Code/Doc/Test Review - M11-E5r-3b:

- first Review Agent verdict: `REVISE`；
- findings: no critical or important issues；
- minor finding: README and taskbook status were stale after build/focused MTR；
  fixed by this completion update；
- review confirmed helper is fail-closed；
- review confirmed the helper does not read `file->ref` or call
  `position(record)`；
- review confirmed no MQ wire/header/flags, public counter/status,
  `Query_result_mq`, iterator, Gather, optimizer, or `HAS_ORDER_BY` changes；
- review confirmed focused validation is sufficient and full `parallel_query`
  suite is not required for this private controlled-smoke-only helper。

Code/Doc/Test Re-review - M11-E5r-3b:

- Review Agent verdict: `ACCEPT`；
- findings: no critical, important, or minor issues；
- confirmed stale status was fixed；
- confirmed technical boundary still holds；
- confirmed no source changes to `file->ref` reads, `position(record)`,
  `PQOF` / `PQWR` wire headers, `Query_result_mq`, iterators, Gather,
  optimizer, `HAS_ORDER_BY`, or public counters/status。

### M11-E5r-4: cmp_ref() Comparator Compatibility Design

Status: committed as `f9635804b40`；design-only taskbook accepted。

Goal:

- document why comparator coding remains blocked；
- define the compatibility requirements before any future
  `handler::cmp_ref()` use；
- prevent synthetic row ids or byte-vector ordering from being mistaken for
  handler-ref stable ordering；
- keep visible ORDER BY PQ and all real comparator/execution changes blocked。

Current branch findings:

- `rowid_tiebreak_ready=false` and `default_ordered_read_ready=false` remain
  false in optimizer preflight；
- ordinary SQL ORDER BY still reaches the `HAS_ORDER_BY` serial rejection；
- current `Exchange_sort` comparator uses byte-vector `sort_key`, byte-vector
  `row_id`, and worker id in controlled smoke paths；
- current branch has no real worker `position(record)` production；
- current branch has no `Query_result_mq` handler-ref deep-copy wire；
- E5r-3b validates only metadata/lifetime shape and does not prove refs are
  comparable by a leader handler。

Comparator compatibility requirements:

- comparator inputs must be deep-copied handler refs, not synthetic row ids；
- both refs must have validated `ref_length` matching the comparator handler；
- leader handler must be compatible by engine, table shape, ref format, and
  partition semantics；
- handler lifetime and table ownership must outlive the comparator use；
- partitioned refs require explicit design before comparison；
- InnoDB primary-key refs are type-aware under `cmp_ref()`；
- generated row-id refs may use byte comparison inside the handler, but that
  does not make generic byte-vector ordering equivalent；
- comparator failure must remain fail-closed and must not fall back to a
  synthetic total order for visible SQL。

Future private helper prerequisites:

- real handler-ref production design reviewed；
- MQ ref deep-copy wire reviewed；
- leader handler ownership / compatibility reviewed；
- metadata can prove `HANDLER_REF`, stable output, expected `ref_length`, deep
  copy, and no detach/advance risk；
- helper remains private/non-default until visible ORDER BY execution is
  separately reviewed。

Allowed files for E5r-4:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- `exchange_sort.*` comparator coding；
- `query_result_mq.*` changes；
- `pq_iterators.*` changes；
- `Gather_operator` changes；
- `pq_optimizer.*` readiness or `HAS_ORDER_BY` serial-boundary relaxation；
- handler/InnoDB, AccessPath, executor, sysvar, or public counter changes；
- any visible ORDER BY PQ path。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`。

Validation for E5r-4:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Planning/Source Review - M11-E5r-4:

- Review Agent verdict: `ACCEPT` for docs-only design；
- recommended E5r-4 docs-only as the next smallest safe task；
- explicitly rejected E5r-4a private comparator shape helper for now；
- confirmed direct comparator helper coding may create false evidence before
  worker ref production, MQ ref wire, and leader handler compatibility are
  proven；
- confirmed real `cmp_ref()` comparator and visible ORDER BY PQ remain blocked。

### M11-E5r-5: MQ Handler-ref Wire Contract Design

Status: committed as `88713346078`；design-only taskbook accepted。

Goal:

- document the MQ handler-ref wire contract before any `Query_result_mq`
  coding；
- define how future ORDER BY handler-ref bytes must be separated from existing
  `PQWR` field-value frames and controlled `PQOF` smoke frames；
- keep current `Query_result_mq`, worker production, comparator, and visible
  ORDER BY PQ unchanged。

Current branch findings:

- `Query_result_mq::send_data()` currently encodes field-value `PQWR` payloads
  and does not carry handler `file->ref` / `file->ref_length`；
- `PQblockScanIterator::Read()` remains fail-closed and does not produce
  handler refs；
- `Exchange_sort` has private row-id and lifetime shape validation only；
- no current path proves real handler refs are in MQ, deep-copied, or safe for
  `cmp_ref()`；
- E5r-4 blocked comparator helper work until real worker ref production, MQ ref
  deep-copy wire, and leader handler compatibility are designed。

Future MQ wire contract requirements:

- do not overload existing `PQWR` field-value frames without a dedicated wire
  review；
- do not overload `PQOF::flags`, which is already used as worker id in
  controlled ORDER BY frame smokes；
- define a distinct ORDER BY handler-ref record frame or explicit metadata
  extension before coding；
- frame metadata must distinguish:
  - no row id；
  - synthetic smoke row id；
  - handler ref；
  - handler-ref length-shape validation failure。
- frame must carry or validate expected `ref_length`；
- frame producer must deep-copy handler ref bytes before worker handler state
  advances；
- frame must record stable-output requirement explicitly；
- ERROR / FINISH semantics must remain distinguishable from ROW payloads；
- ref payload length mismatch must fail closed and not be treated as synthetic
  ordering。

Compatibility with existing frames:

- existing `PQWR` worker-result field-value frames remain unchanged；
- existing controlled `PQOF` frame smokes remain private and non-default；
- future handler-ref wire must not change existing no-ORDER-BY worker-result
  tests；
- future visible ORDER BY path must remain gated until worker ref production,
  MQ ref wire, leader handler compatibility, and comparator readiness are all
  reviewed。

Recommended next split:

- E5r-5 remains docs-only；
- E5r-5a is not recommended yet because private helper shapes already exist
  and more shape code would create false evidence before the wire contract is
  implemented；
- E5r-6 may later cover worker `position(record)` production design after the
  MQ wire contract is accepted；
- any source changes to `Query_result_mq` must be a separate reviewed phase。

Allowed files for E5r-5:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/exchange_sort.*`；
- `Gather_operator` / `exchange.*`；
- `pq_optimizer.*`, `HAS_ORDER_BY` gate, AccessPath, handler/InnoDB, executor,
  sysvar, or public counter changes；
- visible ORDER BY PQ, `cmp_ref()` comparator, or `file->position(record)`
  calls。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`。

Validation for E5r-5:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Planning/Source Review - M11-E5r-5:

- Review Agent verdict: `ACCEPT` for docs-only design；
- recommended E5r-5 as the next smallest safe task；
- confirmed `Query_result_mq::send_data()` currently has no handler-ref wire；
- confirmed `PQblockScanIterator::Read()` remains fail-closed；
- confirmed E5r-5a is not useful before the MQ wire design is accepted；
- confirmed E5r-6 worker production design is riskier and should follow the MQ
  wire contract。

### M11-E5r-6: Worker position(record) Production Design

Status: committed as `19f4899ebf7`；design-only taskbook accepted。

Goal:

- document the worker-side preconditions before any real
  `file->position(record)` coding；
- define when a worker may produce handler ref bytes；
- define failure paths that must not produce refs；
- keep `PQblockScanIterator::Read()`, `Query_result_mq`, comparator, and
  visible ORDER BY PQ unchanged。

Current branch findings:

- `PQblockScanIterator::Init()` / `Read()` still fail closed and do not call
  `file->position(record)`；
- `Query_result_mq::send_data()` still encodes only field-value `PQWR` ROW
  payloads and has no handler-ref wire；
- E5r-5 accepted the MQ handler-ref wire contract only as docs；
- no current path combines worker row read, handler ref production, MQ
  deep-copy, and leader comparator compatibility。

Worker production preconditions:

- worker must have successfully read a row；
- `record` must be the worker handler's current row image；
- worker `TABLE`, handler, handler prebuilt state, read view, and iterator
  ownership must remain valid；
- stable-output requirement must be explicit before requesting refs；
- handler `ref_length` must be known, nonzero, and validated；
- worker must not be detached, killed, at EOF, or in an error path；
- worker must not allow handler state to advance before MQ deep-copies the ref。

Forbidden worker paths:

- no `position(record)` on init failure；
- no `position(record)` on read failure；
- no `position(record)` on EOF；
- no `position(record)` after detach or kill is observed；
- no ref emission when `ref_length` is zero or mismatched；
- no fallback to synthetic row ids for visible stable ORDER BY。

Future coding prerequisites:

- MQ handler-ref wire contract must have a reviewed source implementation；
- worker row-production ownership must be proven for the specific iterator；
- handler ref lifetime must be connected to the MQ deep-copy point；
- leader handler compatibility must be separately reviewed；
- comparator readiness must remain false until all prerequisites are linked and
  tested。

Allowed files for E5r-6:

- `Docs/pq_tasks/README.md`；
- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。

Forbidden:

- `sql/parallel_query/pq_iterators.*`；
- `sql/parallel_query/query_result_mq.*`；
- `sql/parallel_query/exchange_sort.*`；
- `Gather_operator` / `exchange.*`；
- `pq_optimizer.*`, `HAS_ORDER_BY` gate, AccessPath, handler/InnoDB, executor,
  sysvar, or public counter changes；
- visible ORDER BY PQ, `cmp_ref()` comparator, or `file->position(record)`
  calls。

Hard gates:

- `rowid_tiebreak_ready=false`；
- `default_ordered_read_ready=false`；
- `exchange_sort_heap_read_ready=false`；
- visible ORDER BY SQL remains `Not parallel HAS_ORDER_BY`。

Validation for E5r-6:

- docs-only；
- `git diff --check` before commit；
- no build/MTR required unless source or test files change。

Planning/Source Review - M11-E5r-6:

- Review Agent verdict: `ACCEPT` for docs-only design；
- recommended E5r-6 as the next smallest safe task after E5r-5；
- confirmed current `PQblockScanIterator::Read()` still fails closed；
- confirmed current `Query_result_mq::send_data()` still has no handler-ref
  wire；
- confirmed direct worker `file->position(record)` coding is still blocked；
- confirmed no build/MTR is required for docs-only E5r-6。

### M11-E5r Closure

Status: design/validation complete；real execution blocked。

Closure summary:

- E5r-1 documented handler ref ownership inventory；
- E5r-2-pre documented worker `position(record)` / MQ handler-ref format
  contract；
- E5r-2a and E5r-2b added private row-id metadata validation and controlled
  smoke；
- E5r-3a and E5r-3b documented/refined worker ref lifetime shape；
- E5r-4 documented `cmp_ref()` comparator compatibility requirements；
- E5r-5 documented MQ handler-ref wire contract；
- E5r-6 documented worker `position(record)` production preconditions。

Closure Review:

- Review Agent verdict: `ACCEPT`；
- E5r should close as `design/validation complete, real execution blocked`；
- no E5r-7 readiness summary is needed；
- current source remains fail-closed:
  - `PQblockScanIterator::Read()` does not call `position(record)`；
  - `Query_result_mq::send_data()` sends `PQWR` field-value frames only；
  - ORDER BY preflight keeps `rowid_tiebreak_ready=false`,
    `default_ordered_read_ready=false`, and
    `exchange_sort_heap_read_ready=false`；
  - ordinary ORDER BY remains rejected by `HAS_ORDER_BY`。
- no safe E5r source coding remains at this point；
- real `position(record)`, `Query_result_mq` handler-ref wire,
  `cmp_ref()` comparator, optimizer readiness, and visible ORDER BY PQ all
  remain blocked until separate reviewed phases。

### M11-E Post-E5r Handoff

Status: docs-only handoff accepted；source work stopped。

Handoff decision:

- no safe M11-E source task remains after E5r closure；
- E5h-E5q/E5s non-real-path contracts and smokes are already committed；
- E5r rowid/tie-break chain is design/validation complete but real execution
  remains blocked；
- earlier historical task-prompt sections may retain original planning wording,
  but this handoff is the current authoritative M11-E status；
- do not continue with `position(record)`, `Query_result_mq` handler-ref wire,
  `cmp_ref()` comparator, optimizer readiness, or visible ORDER BY PQ inside
  the current M11-E workstream；
- any future work touching those paths must start as a new reviewed phase with
  fresh design/source review and explicit file boundaries。

Allowed next actions:

- turn to another non-ORDER-BY commercial-port workstream；
- create a new reviewed phase for one of the blocked real ORDER BY paths；
- run broader verification if integration readiness is being assessed。

Forbidden without a new reviewed phase:

- `sql/**` or `storage/**` source changes for M11-E；
- `mysql-test/**` changes that imply visible ORDER BY PQ readiness；
- relaxing `HAS_ORDER_BY` or setting ORDER BY readiness flags true。

### M11-E6: ORDER BY Positive-path Phase Selection

Status: docs-only design in progress。

Goal:

- 从 E5r closure 后的新 reviewed phase 重新选择真实 ORDER BY path 的下一步；
- 对齐当前分支、商用参考实现和现有 MTR 护栏；
- 明确下一步只打开一个最小 positive contract，不打开用户可见 ORDER BY PQ。

Explorer conclusions:

- Current-branch Explorer 确认当前仍 fail-closed：
  `pq_check_query_block_eligible()` 继续以 `HAS_ORDER_BY` reject 普通
  ORDER BY；`PQblockScanIterator::Init()` / `Read()` 仍返回 failure/error
  path，不调用 `position(record)`；`Query_result_mq` 只发送当前 `PQWR`
  field-value frame；`Exchange_sort` 仍未接默认 gather path，rowid
  tie-break 只存在私有 shape/smoke；
- Commercial-reference Explorer 确认商用正路径顺序是：
  worker 读到当前 row 后调用 `handler::position(record)` 生成
  `handler::ref`，worker MQ 深拷贝该 ref，leader `Exchange_sort` 再用
  divided-table handler 的 `cmp_ref()` 作为 stable tie-break；
- Test/Docs Explorer 确认当前 `pq_commercial_order_by`、
  `pq_saved_order_group_contract` 和 `pq_commercial_order_by_frames` 仍是
  负向/DBUG-only 护栏；`README.md` 和本文件顶部状态已按 E5r closure
  刷新。

Phase decision:

- 下一编码阶段命名为 M11-E6a Worker Handler-ref Positive Contract；
- E6a 只验证 private positive contract：受控 worker/block-scan 路径在读到
  当前 row 后调用 `position(record)`，深拷贝 `handler::ref/ref_length`，
  并在 leader/private helper 中证明该 ref 可被同一 opened divided-table
  handler 的 `cmp_ref()` 消费；
- M11-E6b 再做 MQ handler-ref wire，把 E6a 生成的 ref 接入私有 MQ frame；
- M11-E6c 再做 `Exchange_sort` private `cmp_ref()` comparator；
- optimizer readiness、default ordered `Read()` 和 visible ORDER BY PQ 必须排在
  E6a/E6b/E6c 之后，且需要单独 reviewed gate。

Allowed files for E6 docs-only:

- `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
- `Docs/pq_tasks/README.md`。

Allowed files for later E6a code taskbook, pending review:

- `sql/parallel_query/pq_iterators.{h,cc}`，仅限受控/private
  `position(record)` contract；
- `sql/parallel_query/query_result_mq.{h,cc}`，E6a 仅允许 explicit no-wire
  assertion；实际 handler-ref MQ copy / wire 必须留到 E6b；
- `sql/parallel_query/exchange_sort.{h,cc}`，仅限 private handler-ref
  validation / `cmp_ref()` smoke helper；
- focused MTR under `mysql-test/suite/parallel_query/`，必须保持普通 ORDER BY
  负向断言。

Forbidden for E6/E6a unless a later reviewed phase explicitly allows:

- relaxing `HAS_ORDER_BY`；
- setting `rowid_tiebreak_ready`、`default_ordered_read_ready` 或
  `exchange_sort_heap_read_ready` true；
- changing default `Gather_operator` to use `Exchange_sort`；
- changing visible `ParallelScanIterator::Read()` ORDER BY behavior；
- adding naked `memcmp(ref)` as a substitute for handler `cmp_ref()`；
- touching `storage/innobase/**` or `sql/handler.*` without a separate
  handler/InnoDB boundary review；
- updating result files to imply visible ORDER BY PQ readiness。

Validation:

- E6 itself is docs-only：run `git diff --check`；
- no build/MTR required unless source or test files change；
- before E6a source coding, write the E6a taskbook and request Code / Docs /
  Test Review。

### M11-E6a: Worker Handler-ref Positive Contract

Status: committed；Code / Docs / Test Review accepted。

Goal:

- 建立商用 ORDER BY stable tie-break 的第一个最小正路径契约；
- 在 private/debug-only 路径中证明 worker 当前 row 可以调用
  `handler::position(record)` 生成真实 `handler::ref`；
- 证明该 ref 会被深拷贝并且可由同一 opened divided-table handler 的
  `cmp_ref()` 消费；
- 保持 visible ORDER BY PQ 关闭。

Non-goals:

- 不实现 `Query_result_mq` handler-ref wire；
- 不改变 `PQWR` field-value frame；
- 不接 `Exchange_sort` default heap reader；
- 不设置 `rowid_tiebreak_ready=true`；
- 不放开 `HAS_ORDER_BY`；
- 不迁移商用 Filesort/Sort_param/default ordered `Read()`。

Proposed private contract:

1. 在受控 DBUG-only smoke 中准备一个真实 InnoDB worker/opened table
   window，读取一条当前 row；
2. 调用 worker handler `position(record)`，要求 `file->ref_length > 0`；
3. 将 `file->ref` 按 `ref_length` 深拷贝到 private owner buffer；
4. 允许 worker handler 前进或结束，owner buffer 必须仍保持有效；
5. 在 leader/private validation helper 中用相同表形态的 opened handler
   调用 `cmp_ref(copied_ref, copied_ref)`，期望返回 `0`；
6. negative windows 必须覆盖：无当前 row、`ref_length == 0`、未深拷贝、
   handler detached / failed scan 时不计 success。

Allowed files for E6a code:

- `sql/parallel_query/pq_iterators.{h,cc}`：
  - private DBUG-only smoke entry；
  - worker current-row `position(record)` probe；
  - no visible `Read()` behavior change；
- `sql/parallel_query/exchange_sort.{h,cc}`：
  - private handler-ref owner / validation helper；
  - `cmp_ref(ref, ref) == 0` smoke only；
  - no default comparator replacement；
- `sql/parallel_query/query_result_mq.{h,cc}`：
  - E6a only permits explicit no-wire assertion / helper comment；
  - no handler-ref payload copy or frame format change；
- `sql/parallel_query/sql_parallel.{h,cc}` and `sql/mysqld.cc`：
  - only if a new status counter or DBUG smoke dispatcher is required；
- focused MTR under `mysql-test/suite/parallel_query/`。

Forbidden files / behavior:

- `sql/parallel_query/pq_optimizer.*` visible eligibility changes；
- `sql/sql_optimizer.*`、`sql/sql_executor.*`、`sql/join_optimizer/**`；
- `sql/handler.*` and `storage/innobase/**` unless a separate reviewed
  handler/InnoDB boundary task is opened；
- `Query_result_mq` handler-ref wire；
- default `Gather_operator` `Exchange_sort` selection；
- visible ORDER BY result changes；
- naked `memcmp(ref)` comparator。

TDD plan:

- RED: add a DBUG-only MTR window that requests the E6a worker handler-ref
  positive contract and expects:
  - attempts delta >= 1；
  - success delta >= 1；
  - copied ref length > 0；
  - `cmp_ref(ref, ref) == 0` validation success；
  - visible ORDER BY still reports `Not parallel HAS_ORDER_BY`；
  - `Parallel_queries_executed` / `Parallel_workers_launched` /
    `Parallel_ranges_dispatched` deltas remain 0 outside the private smoke。
- Before implementation this RED should fail on missing/zero E6a success；
- GREEN: implement the private contract only；
- replay the focused MTR and then full `parallel_query` suite。

Required review after E6a coding:

- Code Review: confirm no visible ORDER BY gate opened and no MQ wire slipped in；
- Docs Review: confirm E6a/E6b/E6c boundaries remain separate；
- Test Review: confirm no-DBUG and visible ORDER BY negative windows remain。

Completion Report - M11-E6a:

- Status: implementation completed locally；Code / Docs / Test Review accepted；
- Changed files:
  - `sql/parallel_query/sql_parallel.{h,cc}`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- Implementation:
  - added E6a status counters:
    `Parallel_orderby_worker_handler_ref_attempts`,
    `Parallel_orderby_worker_handler_ref_success`,
    `Parallel_orderby_worker_handler_ref_unsupported`,
    `Parallel_orderby_worker_handler_ref_bytes`,
    `Parallel_orderby_worker_handler_ref_cmp_equal`；
  - added DBUG-only `pq_orderby_worker_handler_ref_positive_smoke` inside
    `Gather_operator::run_worker_attach_contract_smoke()` after successful
    worker `pq_worker_scan_init()`；
  - the smoke uses existing callback conversion to materialize one current
    worker record, calls `handler::position(record)`, deep-copies
    `handler::ref/ref_length`, and verifies `cmp_ref(copied_ref,
    copied_ref) == 0`；
  - no `Query_result_mq` frame format or handler-ref wire was changed；
  - no optimizer eligibility, `HAS_ORDER_BY`, readiness flag, or default
    `Exchange_sort` behavior was changed。
- TDD evidence:
  - RED: before implementation, `pq_worker_attach_contract_smoke` failed
    because handler-ref attempts / success / bytes / cmp-equal deltas stayed
    `0`；
  - GREEN: after implementation, `pq_worker_attach_contract_smoke` passed。
- Validation:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `pq_worker_attach_contract_smoke` replay passed；
  - `pq_stats` replay passed after syncing the new 224-variable result；
  - full `parallel_query` suite passed 89/89；
  - `git diff --check` passed。
- Review:
  - Review Agent verdict: `ACCEPT`；
  - confirmed the E6a logic is reachable only through DBUG-gated worker attach
    smoke；
  - confirmed no `Query_result_mq` wire, MQ frame definition, `Exchange_sort`,
    optimizer eligibility, `HAS_ORDER_BY`, readiness flag, or default gather
    selection changed；
  - confirmed `position(record)` runs after callback-converted worker current
    record, `handler::ref` is deep-copied, and `cmp_ref(copied_ref,
    copied_ref) == 0` is sufficient for this first private contract。
- Remaining boundary:
  - E6a proves only private worker handler-ref production and local
    `cmp_ref(ref, ref)` consumption；
  - E6b is still required for MQ handler-ref wire；
  - E6c is still required for `Exchange_sort` comparator integration；
  - visible ORDER BY PQ remains blocked。

### M11-E6b: MQ Handler-ref Wire Contract

Status: taskbook draft in progress；no source changes yet。

Goal:

- 在 E6a 已证明 worker 可以生成真实 handler ref 后，验证该 ref 可以通过
  private ORDER BY MQ frame 传输；
- 只使用 DBUG-only / private `PQOF` ORDER BY frame contract；
- 保持通用 `PQWR` worker-result field-value frame 不变；
- 保持 visible ORDER BY PQ 关闭。

Design decision:

- E6b 不直接平移商用 `Query_result_mq::send_data()` 的 stable-output 字段
  布局到当前 `PQWR` frame；
- 当前分支已有 `PQ_orderby_frame_header` / `pq_send_orderby_frame()` /
  `pq_decode_orderby_frame()`，其中 `row_id` payload 已能承载 ORDER BY
  tie-break bytes；
- E6b 的最小安全迁移是：把 E6a 深拷贝得到的真实
  `handler::ref/ref_length` 放入 private `PQOF` frame 的 `row_id` 区段，
  再由 leader/private decode helper 验证：
  - frame type is `ROW`；
  - `row_id_len == ref_length`；
  - row-id contract source is `HANDLER_REF`；
  - decoded bytes equal the E6a copied ref；
  - decoded bytes are deep-copied before worker handler/table cleanup。

Non-goals:

- 不修改 `PQ_worker_result_frame_header`；
- 不修改 `PQ_orderby_frame_header` 或 `PQOF` flags 语义；
- `PQ_orderby_row_id_source::HANDLER_REF` 只是 private validation 输入，
  不是新的 wire metadata；
- 不改变 `Query_result_mq::send_data()` 默认 behavior；
- 不把 handler ref 接入 visible worker result path；
- 不接 `Exchange_sort` heap comparator；
- 不设置 `rowid_tiebreak_ready=true`；
- 不放开 `HAS_ORDER_BY`。

Allowed files for E6b code:

- `sql/parallel_query/exchange_sort.{h,cc}`：
  - private helper/smoke for handler-ref `PQOF` encode/decode；
  - validation that `PQ_orderby_row_id_source::HANDLER_REF` with
    `expected_ref_length` succeeds；
- `sql/parallel_query/sql_parallel.{h,cc}`：
  - only if the E6b smoke reuses the E6a worker attach window to obtain a real
    copied handler ref；
- `sql/mysqld.cc`：
  - only for new E6b status counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- `Docs/pq_tasks/README.md` and this taskbook。

Forbidden files / behavior:

- `sql/parallel_query/query_result_mq.{h,cc}` frame-format changes；
- `PQOF` header / flags format changes；
- `sql/parallel_query/pq_optimizer.*` and visible eligibility changes；
- `sql/sql_optimizer.*`、`sql/sql_executor.*`、`sql/join_optimizer/**`；
- `sql/handler.*` and `storage/innobase/**`；
- default `Gather_operator` `Exchange_sort` selection；
- visible ORDER BY result changes；
- `cmp_ref()` comparator integration beyond `cmp_ref(ref, ref)` local
  validation already done in E6a。

TDD plan:

- RED: add DBUG-only MTR assertions for E6b private MQ wire counters:
  - attempts delta >= 1；
  - success delta >= 1；
  - decoded bytes delta > 0；
  - decoded handler-ref contract success delta >= 1；
  - decoded row-id bytes equal the E6a copied handler ref；
  - unsupported delta == 0；
  - no-DBUG/default-path E6b counters stay 0；
  - visible ORDER BY still reports `Not parallel HAS_ORDER_BY` in existing
    ORDER BY guards；
  - no default `Query_result_mq` / `PQWR` behavior changes。
- Before implementation, this RED must fail on E6b success/bytes counters
  staying `0`；
- GREEN: implement only the private `PQOF` handler-ref encode/decode smoke；
- replay focused MTR, `pq_stats`, and full `parallel_query` suite。

Required review after E6b coding:

- Code Review: confirm `PQWR` and `Query_result_mq::send_data()` are unchanged；
- Docs Review: confirm E6b does not claim visible ORDER BY readiness；
- Test Review: confirm E6b is DBUG-only and existing visible ORDER BY negative
  guards remain。

Completion Report - M11-E6b:

- Status: implementation completed locally；Code / Docs / Test Review accepted；
- Changed files:
  - `sql/parallel_query/exchange_sort.{h,cc}`；
  - `sql/parallel_query/sql_parallel.{h,cc}`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- Implementation:
  - added E6b status counters:
    `Parallel_orderby_handler_ref_wire_attempts`,
    `Parallel_orderby_handler_ref_wire_success`,
    `Parallel_orderby_handler_ref_wire_unsupported`,
    `Parallel_orderby_handler_ref_wire_bytes`,
    `Parallel_orderby_handler_ref_wire_contract_success`；
  - added private helper `pq_run_orderby_handler_ref_wire_smoke()` to encode
    one real copied handler ref into a `PQOF` ORDER BY ROW frame `row_id`
    payload, receive and decode the frame, verify `row_id_len == ref_length`,
    verify decoded bytes equal the copied handler ref, and deep-copy decoded
    bytes before reporting success；
  - reused the E6a worker attach smoke window under the additional
    `pq_orderby_handler_ref_wire_smoke` DBUG flag；
  - did not modify `PQ_worker_result_frame_header`, `Query_result_mq`,
    `PQOF` header / flags format, optimizer eligibility, `HAS_ORDER_BY`,
    readiness flags, or default `Exchange_sort` selection。
- TDD evidence:
  - RED: after adding E6b counters and MTR assertions but before implementation,
    `pq_worker_attach_contract_smoke` failed because E6b attempts / success /
    bytes / contract deltas stayed `0`；
  - GREEN: after implementation, `pq_worker_attach_contract_smoke` passed。
- Validation:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `pq_worker_attach_contract_smoke` passed；
  - `pq_stats --record` SQL completed and hit the known final copy errno 1；
    generated result log was copied manually；
  - `pq_stats` replay passed；
  - full `parallel_query` suite passed 89/89；
  - `git diff --check` passed。
- Review:
  - Review Agent verdict: `ACCEPT`；
  - confirmed no changes to `Query_result_mq`,
    `PQ_worker_result_frame_header`, optimizer eligibility, `HAS_ORDER_BY`,
    readiness flags, default Gather / `Exchange_sort` selection, or PQWR
    frame path；
  - confirmed E6b uses the E6a real handler ref source, encodes it into
    private `PQOF` `row_id`, validates length / bytes / `HANDLER_REF`
    contract, and deep-copies decoded bytes inside the helper；
  - non-blocking follow-up: E6c / real comparator work should add stronger
    post-cleanup ownership checks if decoded refs are retained beyond the
    helper。
- Remaining boundary:
  - E6b proves only private `PQOF` handler-ref wire encode/decode；
  - E6c is still required for `Exchange_sort` `cmp_ref()` comparator
    integration；
  - visible ORDER BY PQ remains blocked。

### M11-E6c: Private cmp_ref Comparator Contract

Status: implementation completed locally；Code / Docs / Test Review accepted。

Goal:

- 在 E6a 已证明 real worker `position(record)` / deep-copied handler ref、
  E6b 已证明 private `PQOF` handler-ref wire 后，验证 equal sort-key 下的
  stable tie-break 可以调用 handler `cmp_ref()`；
- 只做 private / DBUG-only comparator contract；
- 保持当前 `Exchange_sort` 默认 cached comparator、heap reader、visible ORDER
  BY PQ 和 `HAS_ORDER_BY` serial boundary 不变。

Commercial reference:

- worker row path：`PQblockScanIterator::Read()` 在需要 stable rowid 时调用
  `table()->file->position(m_record)`，生成 `file->ref`；
- worker MQ path：`Query_result_mq::send_data()` 在商用实现中把
  `file->ref_length` 字节作为 stable rowid/ref 传输；
- leader ORDER BY path：`Exchange_sort` 在 sort-key 相等时调用
  `file->cmp_ref(row_id_0, row_id_1) < 0`，而不是裸 `memcmp(row_id)`。

Current-branch findings:

- `pq_orderby_cached_compare_records()` 当前在 sort-key 相等时比较
  `std::vector<uchar> row_id`，这是 controlled-smoke 语义，不等价于
  handler `cmp_ref()`；
- `PQ_orderby_cached_record::row_id` 是 owning `std::vector<uchar>`；
  `pq_copy_decoded_orderby_frame()` 已 deep-copy decoded `row_id`；
- E6b 的 decoded bytes deep-copy 证明仍局限在 helper 内；E6c 若要表达
  comparator ownership，必须显式验证 copied refs 在 worker cleanup 后仍可由
  still-open handler `cmp_ref()` 消费。

Allowed files for E6c code:

- `sql/parallel_query/exchange_sort.{h,cc}`：
  - private comparator helper/smoke only；
  - may add a sibling comparator helper that uses handler `cmp_ref()` for
    equal sort-key rows；
  - must not replace the default cached comparator or heap reader comparator；
- `sql/parallel_query/sql_parallel.{h,cc}`：
  - may reuse the E6a/E6b worker attach window to obtain real copied handler
    refs and call the private comparator helper；
- `sql/mysqld.cc`：
  - only for new E6c status counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- `Docs/pq_tasks/README.md` and this taskbook。

Forbidden files / behavior:

- `sql/parallel_query/query_result_mq.{h,cc}`；
- `PQ_worker_result_frame_header` / `PQWR` changes；
- `PQOF` header / flags format changes；
- optimizer eligibility, `HAS_ORDER_BY`, readiness flags, visible ORDER BY PQ；
- default `Gather_operator` `Exchange_sort` selection；
- default cached comparator replacement；
- `sql/handler.*` and `storage/innobase/**`。

TDD plan:

- RED: add DBUG-only MTR assertions for E6c private comparator counters:
  - attempts delta >= 1；
  - success delta >= 1；
  - cmp_equal delta >= 1；
  - post-cleanup cmp_ref success delta >= 1；
  - decoded/deep-copied ref bytes delta > 0；
  - unsupported delta == 0；
  - no-DBUG/default-path E6c counters stay 0；
  - visible ORDER BY guards continue to report `Not parallel HAS_ORDER_BY`。
- Before implementation, RED must fail because E6c counters stay `0`；
- GREEN: implement only private `cmp_ref()` comparator smoke；
- Acceptance: the smoke must copy refs into owned buffers before worker scan /
  worker table cleanup, run cleanup, then call `cmp_ref()` through a still-open
  handler after that cleanup boundary. A comparator success before cleanup is
  insufficient for E6c；
- replay focused MTR, `pq_stats`, and full `parallel_query` suite。

Required review after E6c coding:

- Code Review: confirm default comparator / heap reader / visible ORDER BY are
  unchanged；
- Docs Review: confirm E6a/E6b/E6c boundaries remain separate；
- Test Review: confirm E6c is DBUG-only and existing ORDER BY negative guards
  remain。

Completion Report - M11-E6c:

- Status: implementation completed locally；Code / Docs / Test Review accepted；
- Changed files:
  - `sql/parallel_query/sql_parallel.{h,cc}`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- Implementation:
  - added E6c status counters:
    `Parallel_orderby_handler_ref_cmp_attempts`,
    `Parallel_orderby_handler_ref_cmp_success`,
    `Parallel_orderby_handler_ref_cmp_unsupported`,
    `Parallel_orderby_handler_ref_cmp_bytes`,
    `Parallel_orderby_handler_ref_cmp_equal`,
    `Parallel_orderby_handler_ref_cmp_post_cleanup_success`；
  - added DBUG-only `pq_orderby_handler_ref_comparator_smoke` inside the
    existing E6a worker handler-ref positive window；
  - the smoke copies the E6a real handler ref into an owned vector before
    worker scan / worker table cleanup, runs the existing cleanup, then calls
    `leader_table->file->cmp_ref(copied_ref, copied_ref)` through the still-open
    leader handler after the cleanup boundary；
  - no default cached comparator, heap reader, `Query_result_mq`, `PQWR`,
    `PQOF` header / flags, optimizer eligibility, `HAS_ORDER_BY`, readiness
    flag, or visible ORDER BY behavior was changed。
- TDD evidence:
  - RED: after adding E6c counters and MTR assertions but before implementation,
    `pq_worker_attach_contract_smoke` failed because E6c attempts / success /
    bytes / cmp-equal / post-cleanup deltas stayed `0`；
  - GREEN: after implementation, `pq_worker_attach_contract_smoke` passed。
- Validation:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `pq_worker_attach_contract_smoke` passed；
  - `pq_stats --record` SQL completed and hit the known final copy errno 1；
    generated result log was copied manually；
  - `pq_stats` replay passed；
  - full `parallel_query` suite passed 89/89；
  - `git diff --check` passed。
- Review:
  - Review Agent verdict: `ACCEPT`；
  - confirmed `pq_orderby_handler_ref_comparator_smoke` is scoped to the
    existing DBUG-only E6a worker handler-ref window；
  - confirmed copied ref ownership crosses worker cleanup before leader
    `cmp_ref(ref, ref)`；
  - confirmed no diff in `Query_result_mq`, `PQWR` / `PQOF` frame headers,
    optimizer eligibility, `HAS_ORDER_BY`, readiness flags, heap reader,
    default comparator, handler, or InnoDB paths；
  - confirmed docs do not claim real two-row ORDER BY comparator or visible
    readiness。
- Remaining boundary:
  - E6c proves only private post-cleanup `cmp_ref(ref, ref)` consumption of an
    owned copied handler ref；
  - it does not replace the default byte-vector cached comparator and does not
    open visible ORDER BY PQ；
  - future visible ORDER BY work still needs a reviewed gate for real
    sort-key-equal two-row ordering, default heap integration, and readiness
    flags。

### M11-E6d: Two-row cmp_ref Comparator Contract

Status: committed；Code / Docs / Test Review accepted。

Goal:

- 在 E6c 自等价 `cmp_ref(ref, ref)` 之后，建立 two-row / equal-key private
  comparator contract；
- 在 worker TABLE / handler 已打开且 PQ worker scan 尚未初始化前，使用
  DBUG-only local `ha_rnd_init()` / `ha_rnd_next()` 读取至少两条真实 worker
  row，对每条 worker record 调用 `handler::position(record)` 并 deep-copy
  `handler::ref/ref_length`；
- worker scan / worker table cleanup 后，通过 still-open leader handler
  调用 `cmp_ref(ref0, ref1)` 和 `cmp_ref(ref1, ref0)`，验证比较结果反对称；
- 只证明 private comparator contract，不接默认 `Exchange_sort` heap reader。

Non-goals:

- 不要求 `cmp_ref(ref0, ref1) < 0` 必然成立；只要求不同 row ref 可被
  handler 比较且结果反对称，因为物理 ref 顺序可能与 SQL 插入顺序不同；
- 不构造真实 Filesort sort-key，也不打开 equal sort-key user-visible ORDER
  BY；
- 不替换 `pq_orderby_cached_compare_records()` 的 byte-vector row_id 比较；
- 不改 `Query_result_mq`、`PQWR` 或 `PQOF` frame 格式；
- 不放开 optimizer `HAS_ORDER_BY` serial boundary。

Allowed files for E6d code:

- `sql/parallel_query/sql_parallel.{h,cc}`：
  - private SQL-owned row sink for two-row handler refs；
  - DBUG-only dispatcher / counters；
- `sql/mysqld.cc`：
  - only for E6d status counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- `Docs/pq_tasks/README.md` and this taskbook。

Forbidden files / behavior:

- `sql/parallel_query/query_result_mq.{h,cc}`；
- `sql/parallel_query/exchange_sort.{h,cc}` default comparator or heap reader
  replacement；
- `PQ_worker_result_frame_header` / `PQWR` changes；
- `PQOF` header / flags format changes；
- optimizer eligibility, `HAS_ORDER_BY`, readiness flags, visible ORDER BY PQ；
- `sql/handler.*` and `storage/innobase/**`。

TDD plan:

- RED: add DBUG-only MTR assertions for E6d two-row comparator counters:
  - attempts delta >= 1；
  - success delta >= 1；
  - rows/ref-count delta >= 2；
  - cmp nonzero delta >= 1；
  - antisymmetric success delta >= 1；
  - post-cleanup success delta >= 1；
  - unsupported delta == 0；
  - no-DBUG/default-path E6d counters stay 0；
  - existing visible ORDER BY guards continue to report
    `Not parallel HAS_ORDER_BY`。
- RED must fail before implementation because E6d counters stay `0`；
- GREEN: implement only private two-row ref sink and post-cleanup `cmp_ref()`
  validation；
- replay focused MTR, `pq_stats`, and full `parallel_query` suite。

Implementation notes:

- E6d 没有改 `Query_result_mq`、`PQWR`、`PQOF` 或 `Exchange_sort`；
- two-row refs 只在 `pq_orderby_handler_ref_two_row_cmp_smoke` DBUG flag 下
  采集；
- 采集阶段在 `pq_worker_scan_init()` 之前执行，避免同一个 worker handler
  scan state 与 PQ callback scan 混用；
- 采集后立即 `ha_rnd_end()`，随后继续原有 worker attach smoke，worker
  cleanup 完成后才使用 still-open leader handler 调用
  `cmp_ref(ref0, ref1)` / `cmp_ref(ref1, ref0)`；
- E6d 只要求两个不同 refs 比较结果非零且反对称，不假设物理 ref 顺序等于
  SQL 插入顺序。

Completion Report - M11-E6d Coding:

- Status: implementation completed locally；Code / Docs / Test Review accepted；
- Changed files:
  - `Docs/pq_tasks/README.md`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`；
  - `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `sql/mysqld.cc`；
  - `sql/parallel_query/sql_parallel.h`；
  - `sql/parallel_query/sql_parallel.cc`。
- RED evidence:
  - after adding E6d status counters and MTR assertions, focused
    `pq_worker_attach_contract_smoke` failed because E6d positive counters
    stayed `0`；
  - observed failing deltas: attempts / success / refs / cmp_nonzero /
    antisymmetric / post_cleanup expected positive, actual `0`。
- GREEN evidence:
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
    --vardir=/tmp/pq_e6d_green7_vardir
    --tmpdir=/tmp/pq_e6d_green7_tmp` passed；
  - `pq_stats --record` executed SQL successfully but hit the known final
    result-copy errno `1`; generated log was copied manually to
    `pq_stats.result`；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_stats --parallel=1
    --vardir=/tmp/pq_e6d_stats_replay_vardir
    --tmpdir=/tmp/pq_e6d_stats_replay_tmp` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query --parallel=1
    --vardir=/tmp/pq_e6d_full_vardir --tmpdir=/tmp/pq_e6d_full_tmp` passed
    89/89；
  - `git diff --check` passed。
- Boundary confirmation:
  - default ORDER BY PQ path remains disabled；
  - `HAS_ORDER_BY` serial boundary and visible ORDER BY negative guards remain；
  - no handler / InnoDB / optimizer eligibility / worker MQ wire-format change。

Code / Docs / Test Review - M11-E6d:

- First Review Agent verdict: `REVISE`；
- Important finding:
  - initial E6d implementation reused pre-`pq_worker_scan_init()` copied ref
    under the combined DBUG flag, causing E6a/E6b/E6c positive counters to pass
    without exercising the original `pq_worker_scan_callback_smoke()` path；
- Minor finding:
  - no-DBUG/default-path test checked only two E6d counters, while the taskbook
    required all E6d counters to stay `0` without the DBUG flag。
- Fixes:
  - restored the original E6a/E6b/E6c callback path unconditionally inside
    `pq_orderby_worker_handler_ref_positive_smoke`；
  - kept E6d as an independent post-cleanup two-row `cmp_ref()` check；
  - extended no-DBUG MTR coverage to attempts / success / unsupported / refs /
    cmp_nonzero / antisymmetric / post_cleanup counters。
- Re-validation after fixes:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
    --vardir=/tmp/pq_e6d_reviewfix_replay_vardir
    --tmpdir=/tmp/pq_e6d_reviewfix_replay_tmp` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_stats --parallel=1
    --vardir=/tmp/pq_e6d_reviewfix_stats_vardir
    --tmpdir=/tmp/pq_e6d_reviewfix_stats_tmp` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query --parallel=1
    --vardir=/tmp/pq_e6d_reviewfix_full_vardir
    --tmpdir=/tmp/pq_e6d_reviewfix_full_tmp` passed 89/89。
- Second Review Agent verdict: `ACCEPT`；
- Final review conclusion:
  - original callback contract remains exercised；
  - E6d post-cleanup two-row comparator check is independent；
  - no-DBUG coverage includes all E6d counters；
  - no default ORDER BY path, MQ wire, optimizer, handler, or InnoDB boundary
    was changed。

Required review after E6d coding:

- Code Review: confirm no default `Exchange_sort` comparator / heap reader /
  visible ORDER BY path changed；
- Docs Review: confirm E6d does not claim real ORDER BY readiness；
- Test Review: confirm no-DBUG and visible ORDER BY negative guards remain。

### M11-E6e: Exchange_sort Handler-ref Comparator Adapter Smoke

Status: implementation completed locally；Code / Docs / Test Review accepted。

Background:

- Commercial `Exchange_sort::heap_compare_records()` first compares ORDER BY
  sort keys and, when stable output is required and sort keys are equal, uses
  `handler::cmp_ref(row_id_0, row_id_1)` as the tie-break；
- Commercial `Query_result_mq::send_data()` sends handler refs when stable
  output is enabled, relying on worker-side `file->position()` having filled
  `file->ref`；
- Current branch has private contracts for handler-ref production, private
  `PQOF` wire, post-cleanup `cmp_ref(ref, ref)`, and two-row
  `cmp_ref(ref0, ref1)` / reverse antisymmetry；
- Current default `pq_orderby_cached_compare_records()` still compares equal
  sort-key `row_id` byte vectors directly. It is intentionally not yet a
  commercial `handler::cmp_ref()` tie-break path。

Goal:

- Add a private/DBUG-only comparator adapter shape inside `Exchange_sort` that
  models the commercial equal-sort-key tie-break；
- Construct two controlled ORDER BY cached records with identical sort key and
  `row_id` containing real handler refs copied by the E6d worker attach smoke；
- Compare equal sort-key records through still-open leader handler
  `cmp_ref(left.row_id, right.row_id)` and reverse direction；
- Verify adapter forward/reverse results are non-zero, antisymmetric, and
  directionally consistent with direct `handler::cmp_ref()`；
- Keep the default cached comparator, heap reader, visible ORDER BY path,
  readiness flags, and `HAS_ORDER_BY` serial boundary unchanged。

Non-goals:

- Do not replace `pq_orderby_cached_compare_records()`；
- Do not connect the adapter to the default `Exchange_sort` binary heap or
  `read_next_ordered_record_image_owned_shape()`；
- Do not modify `Query_result_mq`, `PQWR`, or `PQOF` frame formats；
- Do not set `rowid_tiebreak_ready`, `exchange_sort_heap_read_ready`, or
  `default_ordered_read_ready`；
- Do not relax optimizer eligibility or open visible ORDER BY PQ。

Allowed files for E6e code:

- `sql/parallel_query/exchange_sort.{h,cc}`：
  - private comparator adapter/helper and optional controlled smoke；
- `sql/parallel_query/sql_parallel.{h,cc}`：
  - reuse E6d copied refs and trigger E6e DBUG-only smoke/counters；
- `sql/mysqld.cc`：
  - only for E6e status counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- `Docs/pq_tasks/README.md` and this taskbook。

No new source files for E6e；extend existing smoke/MTR unless a new focused MTR
is explicitly justified during coding review。

Forbidden files / behavior:

- `sql/parallel_query/query_result_mq.{h,cc}`；
- `PQ_worker_result_frame_header` / `PQWR` changes；
- `PQOF` header / flags format changes；
- `sql/parallel_query/pq_optimizer.*` and `HAS_ORDER_BY` behavior；
- `PQOrderByExecutionPreflight` readiness flags；
- `ParallelScanIterator::Read()` visible ORDER BY path；
- `sql/handler.*` and `storage/innobase/**`；
- replacing default comparator / heap reader or starting real worker ORDER BY。
- incrementing `Parallel_orderby_execution_preflight_ready` or reducing
  missing ORDER BY preflight prerequisite counters。

TDD plan:

- RED: add DBUG-only MTR assertions for E6e counters:
  - attempts delta >= 1；
  - success delta >= 1；
  - refs / bytes delta >= 2 refs；
  - equal sort-key handler tie-break delta >= 1；
  - nonzero delta >= 1；
  - antisymmetric success delta >= 1；
  - direction-match success delta >= 1；
  - unsupported delta == 0；
  - no-DBUG/default-path E6e counters stay 0；
  - existing visible ORDER BY guards continue to report
    `Not parallel HAS_ORDER_BY` and no visible ORDER BY execution counters grow。
- E6e adapter smoke counters must be named as smoke / contract diagnostics,
  not as readiness flags。
- RED must fail before implementation because E6e counters stay `0`；
- GREEN: implement only private comparator adapter shape and focused smoke；
- replay focused MTR, `pq_stats`, and full `parallel_query` suite。

Required review before E6e coding:

- Design Review verdict: `ACCEPT` with minor wording tightening applied；
- Design Review: confirm E6e is the correct next task after E6d and before any
  visible ORDER BY gate；
- Source Scope Review: confirm default comparator / heap reader / readiness
  flags remain forbidden；
- Test Plan Review: confirm E6e no-DBUG and visible ORDER BY negative guards
  are explicit。

Required review after E6e coding:

- Code Review: confirm adapter remains private and does not replace default
  cached comparator；
- Docs Review: confirm E6e does not claim real ORDER BY readiness；
- Test Review: confirm no-DBUG, positive adapter, and visible ORDER BY negative
  windows remain。

Completion Report - M11-E6e Coding:

- Status: implementation completed locally；Code / Docs / Test Review accepted；
- Design:
  - design-only taskbook committed as `f2ceead3c62`；
  - Design Review Agent verdict: `ACCEPT`；
  - review wording tightened: no new source files, smoke/contract counter names
    only, no ORDER BY readiness/preflight counter changes。
- Changed files:
  - `sql/parallel_query/exchange_sort.{h,cc}`；
  - `sql/parallel_query/sql_parallel.{h,cc}`；
  - `sql/mysqld.cc`；
  - `mysql-test/suite/parallel_query/t/pq_worker_attach_contract_smoke.test`；
  - `mysql-test/suite/parallel_query/r/pq_worker_attach_contract_smoke.result`；
  - `mysql-test/suite/parallel_query/r/pq_stats.result`；
  - `Docs/pq_tasks/commercial-port-m11-order-by-exchange-sort.md`。
- Implementation:
  - added `pq_orderby_handler_ref_adapter_smoke()` as an external helper in
    `exchange_sort.{h,cc}`；
  - helper constructs two controlled `PQ_orderby_cached_record` values with
    identical sort key and handler refs in `row_id`；
  - helper compares only equal-sort-key records through the passed leader
    handler `cmp_ref()`；
  - `Gather_operator::run_worker_attach_contract_smoke()` invokes the helper
    only under `pq_orderby_handler_ref_adapter_smoke` after E6d has collected
    two refs and passed post-cleanup direct `cmp_ref()` checks；
  - added E6e smoke/contract diagnostic counters with shortened SHOW STATUS
    names under `Parallel_orderby_ref_adapter_smoke_*`；
  - no default `pq_orderby_cached_compare_records()`, heap reader,
    `Query_result_mq`, PQWR/PQOF wire, optimizer, readiness flag, handler, or
    InnoDB change。
- RED evidence:
  - after adding E6e counters and MTR assertions but before implementation,
    `pq_worker_attach_contract_smoke` failed because attempts / success / refs
    / cmp_nonzero / antisymmetric / direction / tiebreak deltas stayed `0`；
  - status variable names were shortened during RED setup because the initial
    long names produced `NULL` in `performance_schema.global_status`。
- GREEN / validation:
  - `git diff --check` passed；
  - `cmake --build build-ninja --target mysqld -j 16` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_worker_attach_contract_smoke --parallel=1
    --vardir=/tmp/pq_e6e_green_vardir --tmpdir=/tmp/pq_e6e_green_tmp`
    passed；
  - `pq_stats --record` SQL completed and hit the known final copy errno `1`;
    generated log was copied manually to `pq_stats.result`；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query pq_stats --parallel=1
    --vardir=/tmp/pq_e6e_stats_replay_vardir
    --tmpdir=/tmp/pq_e6e_stats_replay_tmp` passed；
  - `TMPDIR=/tmp MTR_BINDIR=../build-ninja perl mysql-test-run.pl
    --suite=parallel_query --parallel=1 --vardir=/tmp/pq_e6e_full_vardir
    --tmpdir=/tmp/pq_e6e_full_tmp` passed 89/89。
- Remaining boundary:
  - E6e proves only private equal-sort-key handler-ref comparator adapter
    shape；
  - it does not make the default Exchange_sort heap comparator commercial-ready
    and does not open visible ORDER BY PQ。
- Code / Docs / Test Review:
  - Review Agent verdict: `ACCEPT`；
  - confirmed `pq_orderby_handler_ref_adapter_smoke()` is controlled and uses
    `handler::cmp_ref()` only for private tie-break validation；
  - confirmed the only call site is DBUG-gated and runs after E6d direct
    post-cleanup two-row `cmp_ref()` checks；
  - confirmed no default comparator / heap reader, `Query_result_mq`, PQWR/PQOF,
    optimizer, readiness flag, handler/InnoDB, or visible ORDER BY diff；
  - confirmed MTR covers no-DBUG zero counters, positive adapter counters,
    unsupported `0`, `pq_stats`, and visible ORDER BY negative guards。

### M11-E6f: ORDER BY Readiness Inventory and Query_result_mq Stable Handler-ref Wire Contract

Status: taskbook started；docs/design-only before source coding。

Decision input:

- Current Branch Explorer recommends an E6f docs-only readiness /
  prerequisite inventory before any further source smoke, because the current
  branch is still fail-closed and E6a-E6e evidence is private；
- Commercial Reference Explorer recommends the next source direction after E6e
  should be a private `Query_result_mq` stable handler-ref wire contract,
  because the commercial chain depends on
  `position(record) -> file->ref -> MQ deep copy -> leader decode ->
  handler::cmp_ref()` before the default heap comparator has real row-id input；
- These recommendations are combined here: first record the inventory and
  reviewed wire contract, then code only a private/DBUG contract in a later
  reviewed E6f coding step。

Commercial behavior to preserve:

- worker-side `file->position(record)` fills `handler::ref` before sending the
  row；
- when stable output is enabled, `Query_result_mq::send_data()` deep-copies
  `file->ref` bytes of length `file->ref_length` into the MQ frame；
- leader-side exchange code decodes row-id bytes before reconstructing the
  record image；
- `Exchange_sort` receives the opened divided-table handler and `ref_length`；
- default heap comparison uses `handler::cmp_ref(row_id_0, row_id_1)` only when
  stable output is required and row-id bytes are present。

Current branch inventory:

- E6a proves only a private/debug-only worker attach contract:
  `position(record)` can produce a deep-copied handler ref and
  `cmp_ref(ref, ref)==0`；
- E6b proves only a private `PQOF` ORDER BY frame can carry the E6a copied
  handler ref；
- E6c proves the copied handler ref survives worker cleanup and can be compared
  after cleanup through a still-open leader handler；
- E6d proves two real worker row refs can be compared after cleanup with
  non-zero / antisymmetric `cmp_ref()` results；
- E6e proves only a private equal-sort-key comparator adapter can call
  `handler::cmp_ref()` on controlled cached records；
- current `Query_result_mq::send_data()` still sends only `PQWR` field-value
  payloads；`m_stable_output` is stored but does not add handler-ref bytes to
  the wire；
- current `Exchange_sort` default cached comparator still compares `row_id`
  bytes directly, then `worker_id`；it is not the commercial `cmp_ref()`
  default comparator；
- current `PQOrderByExecutionPreflight` still keeps ORDER BY execution
  fail-closed, including false prerequisites for worker ORDER BY frame
  producer, heap read, row-id tie-break, default ordered read, Filesort /
  Sort_param runtime state, and leader materialization；
- ordinary user-visible ORDER BY still stops at the `HAS_ORDER_BY` serial
  boundary。

Readiness interpretation:

- E6a-E6e private evidence must not be treated as:
  - `rowid_tiebreak_ready=true`；
  - `exchange_sort_heap_read_ready=true`；
  - `default_ordered_read_ready=true`；
  - `worker_order_frame_producer_ready=true`；
  - visible ORDER BY PQ eligibility；
  - proof that default `pq_orderby_cached_compare_records()` is commercial
    ready。
- E6f may add new diagnostics named as private wire / smoke / contract
  counters only；it must not reduce missing prerequisite counters or increment
  `Parallel_orderby_execution_preflight_ready`。

E6f design goal:

- define a private stable handler-ref wire contract for `Query_result_mq` that
  can be tested without routing user-visible ORDER BY through PQ；
- prove deep-copy semantics for handler refs in the worker-result wire shape；
- keep existing normal `PQWR` field-value behavior compatible with M11-B
  worker-result tests；
- keep the contract explicit that `position()` must have happened before
  `send_data()` and that E6f does not itself establish runtime readiness。

Proposed coding shape after design review:

- add a private helper around `Query_result_mq` worker-result frame construction
  that is not reachable from production `Query_result_mq::send_data()` in E6f；
- keep `Query_result_mq::send_data()` constructor-flag behavior unchanged in
  E6f：`m_stable_output` must remain unused by the production send path until a
  later reviewed runtime wiring phase；
- when stable output is explicitly requested by the private helper and a valid
  handler/ref is supplied, place deep-copied handler-ref bytes in a stable-ref
  wire contract isolated by a reserved `PQWR` `flags` bit；
- normal non-stable `PQWR` frames must keep `flags == 0` and remain
  byte-compatible for existing row-value decode paths；
- add a stable decode helper that first validates the stable-ref flag and then
  exposes borrowed row-id bytes and ref length from the raw frame；
- ordinary `pq_decode_worker_result_row()` must not silently decode stable-ref
  frames as normal field-value frames；it must reject or otherwise fail safely
  when the stable-ref flag is present；
- do not change `PQ_worker_result_frame_header` size, field order, magic,
  version, message type numbering, or the existing non-stable row payload
  layout；
- fail closed when `ref_length == 0`, handler is null, `handler::ref` is null,
  the stable frame would overflow `uint32`, or the frame header/length is
  inconsistent；
- add DBUG-only MTR that constructs a stable-output `Query_result_mq` or helper
  frame with copied handler ref, mutates the original source buffer after send,
  then verifies the decoded wire bytes remain unchanged；
- add negative DBUG MTR paths proving:
  - invalid stable-ref inputs do not produce a fake row id；
  - invalid flags, version, or length fail closed；
  - a stable-ref frame does not decode successfully through the normal
    non-stable `pq_decode_worker_result_row()` path。

Allowed files for E6f coding:

- `sql/parallel_query/query_result_mq.{h,cc}`；
- `sql/parallel_query/sql_parallel.{h,cc}` only for DBUG smoke entry and
  counters if the smoke is not self-contained；
- `sql/mysqld.cc` only for E6f private status counters；
- focused MTR under `mysql-test/suite/parallel_query/`；
- `Docs/pq_tasks/README.md` and this taskbook。

Forbidden files / behavior:

- `sql/parallel_query/pq_optimizer.*` and any `HAS_ORDER_BY` eligibility
  relaxation；
- `PQOrderByExecutionPreflight` readiness flag changes；
- default `pq_orderby_cached_compare_records()` replacement；
- default `Exchange_sort` heap reader or `ParallelScanIterator::Read()` ORDER
  BY path；
- `sql/sql_executor.*`, `sql/sql_optimizer.*`,
  `sql/join_optimizer/access_path.*`；
- `sql/handler.*` and `storage/innobase/**`；
- visible ORDER BY PQ activation；
- production `Query_result_mq::send_data()` behavior changes；
- `pq_make_join_readinfo()` / `pq_check_stable_sort()` production-path wiring；
- `PQ_worker_result_frame_header` size/order/magic/type renumbering。

Required review before E6f coding:

- Design Review: confirm inventory correctly separates private evidence from
  default runtime readiness；
- Commercial Reference Review: confirm the next source step should be stable
  handler-ref wire before default heap comparator；
- Scope Review: confirm existing non-stable `PQWR` decode remains compatible,
  stable-ref frames are isolated by a reserved flag, normal decode does not
  silently accept stable frames, and visible ORDER BY remains blocked。

Validation for this docs/design step:

- `git diff --check`；
- no build or MTR required before source coding；
- optional read-only confirmation that current `Query_result_mq::send_data()`
  still does not use `m_stable_output`, current ORDER BY preflight remains
  fail-closed, and current visible ORDER BY still reports `HAS_ORDER_BY`。

Next recommended sequence after E6f design review:

1. E6f coding: private `Query_result_mq` stable handler-ref wire contract；
2. E6g design: default `Exchange_sort` handler-ref comparator / heap reader
   precondition review；
3. E6g coding: guarded default heap comparator smoke only after stable wire
   evidence exists；
4. E7 design: user-visible ORDER BY gate remains deferred until wire,
   comparator, default reader, kill/cleanup, Filesort state, and preflight
   readiness are all reviewed。

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

Status: committed as `ee648d7ccd9`；Code-Docs-Test Review accepted。

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

Status: committed as `47138dcd8c6`；Code-Docs-Test Review accepted。

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

Status: committed as `30bc426afe2`；Code-Docs-Test Review accepted。

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

Status: committed as `d38ff96651a`；design review accepted。

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

Status: committed as `e8645dbc2b8`；design review accepted。

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
