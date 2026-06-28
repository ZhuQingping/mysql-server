# D6 EXPLAIN DOP Cap Alignment Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for behavior changes
> and request independent review before commit.

**Goal:** Align PQ EXPLAIN annotations with the runtime DOP gate used by
`TryCreatePQTableScanIterator()`.

D4 allowed `parallel_default_dop` values from 0 to 1024, while the current
fullscan runtime can execute only positive DOP values up to
`PQ_Leader_context::MAX_THREADS` (256). Runtime already falls back to the
serial iterator for DOP0 and DOP257..1024. EXPLAIN must not keep reporting
those cases as PQ eligible execution candidates.

---

## 状态

Status: completed.

## 允许修改

- `sql/parallel_query/pq_optimizer.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/opt_explain.cc`
- `sql/join_optimizer/explain_access_path.cc`
- `mysql-test/suite/parallel_query/t/pq_explain_dop_cap.test`
- `mysql-test/suite/parallel_query/r/pq_explain_dop_cap.result`
- `Docs/pq_tasks/commercial-port-d24-explain-dop-cap.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `TryCreatePQTableScanIterator()` runtime gate semantics
- InnoDB / handler execution paths
- ORDER BY / GROUP BY / range / ref / ICP eligibility
- sysvar range contract from D4 (`parallel_default_dop=0..1024`)

## 设计要求

- Add one shared helper for EXPLAIN-facing DOP executability so traditional
  EXPLAIN, TREE, and JSON do not drift from each other.
- DOP0 should be reported as not parallel with a stable reason.
- DOP values above the current threaded fullscan cap should be reported as not
  parallel with a stable reason.
- Positive DOP values within the cap must keep the existing eligible
  annotation and `pq_dop` JSON field.
- EXPLAIN must remain read-only for PQ execution counters.
- No production execution behavior should change in D6.

## RED / GREEN Plan

Create `pq_explain_dop_cap`:

- build a small InnoDB table;
- enable `parallel_query` and `parallel_cost_threshold=0`;
- verify DOP0:
  - traditional EXPLAIN shows `Not parallel DOP_DISABLED`;
  - TREE shows `not parallel (DOP_DISABLED)`;
  - JSON has `"not_parallel": "DOP_DISABLED"` and no PQ eligible annotation;
- verify DOP1024:
  - traditional EXPLAIN shows `Not parallel DOP_EXCEEDS_EXECUTION_CAP`;
  - TREE shows `not parallel (DOP_EXCEEDS_EXECUTION_CAP)`;
  - JSON has `"not_parallel": "DOP_EXCEEDS_EXECUTION_CAP"`;
- verify DOP3 remains eligible and carries `dop=3`;
- verify EXPLAIN does not increment PQ executed/fallback counters.

Run RED first:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_explain_dop_cap \
  --vardir=/tmp/pq-d24-red-vardir \
  --tmpdir=/tmp/pq-d24-red-tmpdir
```

Expected RED before production changes: DOP0/DOP1024 currently still show the
eligible annotation.

## Verification

Run after implementation:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_explain_dop_cap pq_explain_eligible pq_explain_json_tree_minimal \
  pq_commercial_fullscan_dop \
  --vardir=/tmp/pq-d24-final-vardir \
  --tmpdir=/tmp/pq-d24-final-tmpdir
```

## Completion Report

Status: completed.

Changed files:

- `sql/parallel_query/pq_optimizer.h`
- `sql/parallel_query/pq_optimizer.cc`
- `sql/opt_explain.cc`
- `sql/join_optimizer/explain_access_path.cc`
- `mysql-test/suite/parallel_query/t/pq_explain_dop_cap.test`
- `mysql-test/suite/parallel_query/r/pq_explain_dop_cap.result`
- `Docs/pq_tasks/commercial-port-d24-explain-dop-cap.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

Implementation:

- Added `pq_fullscan_requested_dop()` and
  `pq_fullscan_dop_unsuite_reason()` so EXPLAIN uses a single fullscan DOP
  executability check.
- Added `DOP_DISABLED` and `DOP_EXCEEDS_EXECUTION_CAP` unsuite reasons.
- Traditional EXPLAIN, FORMAT=TREE, and FORMAT=JSON now report DOP0 and
  DOP1024 as `Not parallel` / `not_parallel` instead of eligible.
- In-cap DOP values, such as DOP3, keep the existing eligible annotation and
  DOP display.
- No runtime execution gate or sysvar range behavior was changed.

Verification:

RED before production changes:

```text
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_explain_dop_cap \
  --vardir=/tmp/pq-d24-red-vardir \
  --tmpdir=/tmp/pq-d24-red-tmpdir
```

Result: failed as expected because DOP0 and DOP1024 still showed the eligible
annotation.

GREEN / targeted:

```text
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_explain_dop_cap pq_explain_eligible pq_explain_json_tree_minimal \
  pq_commercial_fullscan_dop \
  --vardir=/tmp/pq-d24-targeted-vardir \
  --tmpdir=/tmp/pq-d24-targeted-tmpdir
```

Result: build passed; all 5 MTR entries passed, including shutdown report.

Review:

- Independent review agent: ACCEPT.
- Findings: no Critical, Important, or Minor issues.

Residual risk:

- `pq_optimizer.cc` now includes `pq_handler.h` for
  `PQ_Leader_context::MAX_THREADS`. The dependency is limited to the `.cc` and
  verified by the `mysqld` build.
- This task aligns EXPLAIN for the current fullscan path only; later
  non-fullscan commercial paths may need their own execution-cap display rules.
