# D7 Parallel Max Threads Cap Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for behavior changes
> and request independent review before commit.

**Goal:** Expose the commercial `parallel_max_threads` resource variable and
align the current fullscan DOP gate with that configured thread budget.

The current branch already has a global `parallel_max_threads` variable and
thread-budget checks in `sql_parallel.cc`, but there is no sysvar for users or
tests to configure it. D6 aligned EXPLAIN with the hard execution cap
`PQ_Leader_context::MAX_THREADS`. D7 adds the commercial resource cap as the
next layer: requested DOP must be positive, must not exceed the compiled
fullscan cap, and must not exceed `parallel_max_threads` when that global
budget is non-zero.

---

## 状态

Status: in progress.

## 允许修改

- `sql/sys_vars.cc`
- `sql/parallel_query/pq_optimizer.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_max_threads.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_max_threads.result`
- `Docs/pq_tasks/commercial-port-d25-parallel-max-threads.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- hint parser / `PQ()` hint behavior
- cost model and optimizer trace
- worker queue timeout behavior
- InnoDB / handler scan implementation
- ORDER BY / GROUP BY / range / ref / ICP eligibility

## 设计要求

- Add global sysvar `parallel_max_threads` backed by existing global
  `parallel_max_threads`.
- Keep `parallel_max_threads=0` as the current internal "no resource cap"
  semantics.
- Default should follow the commercial value where feasible (`64`).
- The current fullscan cap should be:
  - `PQ_Leader_context::MAX_THREADS` when `parallel_max_threads=0`;
  - `min(PQ_Leader_context::MAX_THREADS, parallel_max_threads)` otherwise.
- Traditional EXPLAIN, FORMAT=TREE, and FORMAT=JSON should report DOP greater
  than the configured budget as not parallel.
- `TryCreatePQTableScanIterator()` should use the same helper so EXPLAIN and
  runtime iterator selection remain aligned.
- Existing DOP0 and above-compiled-cap behavior from D6 must remain intact.

## RED / GREEN Plan

Create `pq_commercial_max_threads`:

- verify `@@global.parallel_max_threads` exists and defaults to `64`;
- set `GLOBAL parallel_max_threads=2`;
- with `parallel_default_dop=3`, verify:
  - traditional EXPLAIN reports `Not parallel DOP_EXCEEDS_THREAD_BUDGET`;
  - TREE reports `not parallel (DOP_EXCEEDS_THREAD_BUDGET)`;
  - JSON reports `"not_parallel": "DOP_EXCEEDS_THREAD_BUDGET"`;
  - executing a simple fullscan uses serial fallback, not worker execution;
- with `parallel_default_dop=2`, verify eligible/executable fullscan remains
  possible and launches 2 workers;
- set `GLOBAL parallel_max_threads=0` and verify DOP3 is eligible again;
- restore `GLOBAL parallel_max_threads=64`.

Run RED first:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_max_threads \
  --vardir=/tmp/pq-d25-red-vardir \
  --tmpdir=/tmp/pq-d25-red-tmpdir
```

Expected RED before production changes: unknown system variable
`parallel_max_threads`.

## Verification

Run after implementation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_max_threads pq_vars pq_explain_dop_cap \
  pq_commercial_fullscan_dop pq_commercial_fullscan \
  --vardir=/tmp/pq-d25-final-vardir \
  --tmpdir=/tmp/pq-d25-final-tmpdir
```

## Completion Report

Status: pending.

Changed files: pending.

Implementation: pending.

Verification: pending.

Review: pending.

Residual risk: pending.
