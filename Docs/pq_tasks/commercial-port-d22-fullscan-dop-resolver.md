# D4 Fullscan DOP Resolver Alignment Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for code changes and
> request independent code review before commit.

**Goal:** Align the visible clustered fullscan DOP contract with the commercial
implementation for the already-open threaded visible void-pull path.

Commercial reference accepts `parallel_default_dop=0..1024`; `0` disables PQ
selection for the statement, while positive DOP values choose the worker count.
D3 still hard-codes the default visible fullscan gate to DOP2/DOP4 only. D4
extends that gate to positive DOP values supported by the current worker-thread
path, without migrating parser hint syntax, range/ref/ICP, ORDER BY, or GROUP
BY semantics.

---

## 状态

Status: design ready；等待 TDD 编码。

## 背景

D2/D3 completed:

- default DOP2/DOP4 clustered fullscan uses real worker threads;
- worker threads pull rows through `ha_pq_next(void*)`;
- leader consumes typed record-image ROW/FINISH frames through
  `Exchange_nosort`;
- explicit DOP4 shadow/callback coverage remains reachable through
  `parallel_query_experimental_threaded_dop4` and
  `pq_read_threaded_dop4_shadow_path`.

Remaining commercial DOP gap:

- current `parallel_default_dop` range is `1..256`;
- commercial range is `0..1024`;
- current `PQTableScanIterator` normalizes DOP0 to DOP1;
- current default threaded visible fullscan gate accepts only DOP2/DOP4;
- `THD::pq_dop` exists in the current tree but D4 does not migrate full hint
  parser semantics.

## 允许修改

- `sql/sys_vars.cc`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/pq_iterator.h` if a small resolver helper is needed
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan_dop.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan_dop.result`
- `Docs/pq_tasks/commercial-port-d22-fullscan-dop-resolver.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `sql/parse_tree_hints.*` / `sql/sql_yacc.yy` / `sql/opt_hints.*`
- range/ref/ICP/ORDER BY/GROUP BY eligibility or iterators
- InnoDB non-fullscan scan code
- `sql/parallel_query/sql_parallel.*` worker lifecycle semantics, unless RED
  shows positive DOP values cannot be cleaned up safely
- new error codes or commercial `parallel_max_threads` warning text

## Task 1: RED - commercial DOP variable range

Update `pq_vars` expectations first:

- `SET SESSION parallel_default_dop=0` should be accepted and read back as `0`;
- `SET SESSION parallel_default_dop=1024` should be accepted and read back as
  `1024`;
- `SET SESSION parallel_default_dop=1025` should truncate to `1024`;
- remove the old expectations that DOP0 truncates to 1 and DOP257 truncates to
  256.

Run:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_vars \
  --vardir=/tmp/pq-d22-vars-red-vardir \
  --tmpdir=/tmp/pq-d22-vars-red-tmpdir
```

Expected: FAIL before the sysvar range change.

## Task 2: GREEN - sysvar range

Change `parallel_default_dop` valid range from `1..256` to `0..1024`.

Do not add the commercial `parallel_max_threads` warning in D4 because the
current tree does not expose the same sysvar/error-code contract.

## Task 3: RED - default fullscan positive DOP resolver

Add `pq_commercial_fullscan_dop`:

- table with enough rows to distribute across workers;
- `parallel_query=ON`;
- `parallel_cost_threshold=0`;
- no debug hook;
- no `parallel_query_experimental_threaded_dop*`;
- DOP0 fullscan:
  - `parallel_default_dop=0`;
  - `Parallel_queries_executed` delta `0`;
  - `Parallel_queries_fallback` delta `1` or stable documented serial fallback
    behavior;
  - `Parallel_workers_launched` delta `0`;
- DOP3 fullscan:
  - `parallel_default_dop=3`;
  - row result is complete;
  - `Parallel_queries_executed` delta `1`;
  - `Parallel_queries_fallback` delta `0`;
  - `Parallel_rows_scanned` delta equals table row count;
  - `Parallel_workers_launched` delta `3`;
  - threaded visible void-pull selected delta `1`, FINISH delta `3`,
    workers delta `3`, failures delta `0`;
- DOP5 fullscan with the same shape and workers/FINISH delta `5`.

Run:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan_dop \
  --vardir=/tmp/pq-d22-dop-red-vardir \
  --tmpdir=/tmp/pq-d22-dop-red-tmpdir
```

Expected: FAIL before source code change because DOP0 is normalized to DOP1 and
DOP3/DOP5 do not enter the visible fullscan path.

## Task 4: GREEN - fullscan DOP resolver

Implementation constraints:

- `requested_dop == 0` must not be normalized to `1` in
  `PQTableScanIterator::Init()`;
- default threaded visible fullscan gate should accept positive DOP values
  within the sysvar range, not only 2/4;
- keep explicit DOP4 shadow/callback precedence from D3;
- keep PQWR and visible DBUG gates precedence unchanged;
- keep BLOB/read-set/reclength/fullscan guards unchanged;
- do not open DOP1 experimental shadow path by accident; DOP1 default visible
  fullscan may enter only if it uses the same threaded visible void-pull
  topology and passes tests.

## Task 5: Review and verification

Run:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_vars pq_commercial_fullscan_dop pq_commercial_fullscan \
  pq_read_threaded_dop4_worker_error pq_read_threaded_dop4_external_kill \
  pq_stats \
  --vardir=/tmp/pq-d22-final-vardir \
  --tmpdir=/tmp/pq-d22-final-tmpdir
```

Independent review focus:

- DOP0 is not silently coerced to DOP1;
- positive DOP values use the threaded visible void-pull producer only for
  eligible clustered fullscan;
- explicit DOP4 shadow/callback tests remain reachable;
- no range/ref/ICP/ORDER BY/GROUP BY path is opened.

## Completion Report

Pending.
