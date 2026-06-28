# D8 Commercial Resource Sysvars Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for behavior changes
> and request independent review before commit.

**Goal:** Add the remaining commercial PQ resource/cost/fallback session
variables that are required by the taurusdbondstore `parallel_query` suite.

D7 exposed the commercial global `parallel_max_threads` sysvar and wired it
into the current fullscan DOP cap. D8 adds the remaining variable contract only:
`parallel_rows_threshold`, `parallel_tuple_cost`, `parallel_setup_cost`, and
`parallel_graceful_fallback`. These variables are not wired into optimizer cost,
row-threshold eligibility, or execution fallback behavior in D8.

---

## 状态

Status: completed.

## 允许修改

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_resource_sysvars.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_resource_sysvars.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`
- `Docs/pq_tasks/commercial-port-d26-resource-sysvars.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- optimizer eligibility or cost model
- optimizer trace
- `TryCreatePQTableScanIterator()` or worker execution paths
- InnoDB / handler paths
- hint parser / `PQ()` hint behavior
- fallback retry execution semantics

## 商用变量合同

- `parallel_rows_threshold`
  - scope: SESSION
  - type: unsigned integer
  - default: `10000`
  - range: `0..ULONG_MAX` equivalent for this branch
- `parallel_tuple_cost`
  - scope: SESSION
  - type: double
  - default: `1.5`
  - range: `0..DBL_MAX`
- `parallel_setup_cost`
  - scope: SESSION
  - type: double
  - default: `250.0`
  - range: `0..DBL_MAX`
- `parallel_graceful_fallback`
  - scope: SESSION
  - type: bool
  - default: `ON`

## RED / GREEN Plan

Create `pq_commercial_resource_sysvars`:

- verify all four variables exist with commercial defaults;
- verify session values can be changed and reset:
  - `parallel_rows_threshold=0`, then default;
  - `parallel_tuple_cost=0.0`, `1.5`;
  - `parallel_setup_cost=0.0`, `250.0`;
  - `parallel_graceful_fallback=OFF`, `ON`;
- verify global values are visible where MySQL exposes session sysvars as
  global defaults;
- verify invalid negative numeric values are truncated by sysvar
  machinery where applicable.

Run RED first:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_resource_sysvars \
  --vardir=/tmp/pq-d26-red-vardir \
  --tmpdir=/tmp/pq-d26-red-tmpdir
```

Expected RED before production changes: unknown system variable
`parallel_rows_threshold`.

## Verification

Run after implementation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_resource_sysvars pq_vars pq_commercial_max_threads \
  --vardir=/tmp/pq-d26-final-vardir \
  --tmpdir=/tmp/pq-d26-final-tmpdir
```

## Completion Report

Status: completed.

Changed files:

- `sql/system_variables.h`
- `sql/sys_vars.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_resource_sysvars.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_resource_sysvars.result`
- `mysql-test/suite/parallel_query/t/pq_vars.test`
- `mysql-test/suite/parallel_query/r/pq_vars.result`

Implementation:

- Added commercial-compatible session variable storage for
  `parallel_rows_threshold`, `parallel_tuple_cost`, `parallel_setup_cost`, and
  `parallel_graceful_fallback`.
- Added sysvar declarations with the commercial defaults and ranges:
  `10000`, `1.5`, `250.0`, and `ON`.
- Kept this task contract-only. No optimizer eligibility, cost-model, fallback,
  handler, InnoDB, hint-parser, or execution-path behavior was wired in this
  step.
- Extended `pq_vars` coverage so the common variable contract test observes the
  new commercial variables.
- Added invalid lower-bound checks showing negative numeric assignments are
  truncated to `0` with sysvar warning 1292, matching MySQL sysvar machinery.

Verification:

- RED observed before production changes:
  `pq_commercial_resource_sysvars` failed with unknown
  `parallel_rows_threshold`.
- `git diff --check`: passed.
- `cmake --build build-ninja --target mysqld -j 8`: passed.
- Initial targeted MTR passed:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_resource_sysvars pq_vars pq_commercial_max_threads \
  --vardir=/tmp/pq-d26-final-vardir \
  --tmpdir=/tmp/pq-d26-final-tmpdir
```

- After review feedback, added negative numeric assignment coverage and reran
  targeted MTR successfully:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_resource_sysvars pq_vars pq_commercial_max_threads \
  --vardir=/tmp/pq-d26-final2-vardir \
  --tmpdir=/tmp/pq-d26-final2-tmpdir
```

Review:

- Independent review verdict: ACCEPT.
- Lightweight re-review after invalid-value coverage: ACCEPT.

Residual risk:

- These variables are intentionally not yet connected to commercial optimizer
  selection, cost comparison, or graceful fallback behavior. Those semantics are
  tracked as follow-up commercial-port tasks.
