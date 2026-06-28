# D3 Default DOP4 Fullscan Void-Pull Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for code changes and
> request independent code review before commit.

**Goal:** Extend D2 default threaded fullscan void-pull from DOP2 to DOP4, so
ordinary eligible clustered fullscan follows `parallel_default_dop` for DOP4
without requiring `parallel_query_experimental_threaded_dop4`.

**Architecture:** D2 introduced the commercial-shaped default fullscan topology:
leader initializes a fullscan leader context, starts real worker threads, each
worker pulls rows through `ha_pq_next(void*)`, and leader consumes typed
record-image ROW/FINISH frames through `Exchange_nosort`. D3 should reuse that
same path for DOP4. It must not route default DOP4 through the older
experimental threaded shadow/callback producer path.

---

## 状态

Status: design ready；等待 TDD 编码。

## 背景

D2 completed:

- default DOP2 clustered fullscan starts real worker threads;
- worker producer calls `ha_pq_next(void*)`;
- leader consumes existing Exchange/MQ row materialization;
- legacy synchronous void-pull and PQWR paths remain explicit DBUG regression
  hooks.

Remaining DOP gap:

- `should_enter_threaded_visible_void_pull_fullscan_path()` currently accepts
  only `requested_dop == 2`;
- `parallel_default_dop=4` without experimental gate does not enter D2;
- existing DOP4 coverage uses `parallel_query_experimental_threaded_dop4=ON`,
  which exercises the older threaded shadow/callback producer path, not D2
  `ha_pq_next(void*)` worker pull.

Commercial reference launches workers according to the chosen DOP; DOP4
clustered fullscan should therefore use the same D2 topology by default when the
query is eligible.

## 允许修改

- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `Docs/pq_tasks/commercial-port-d21-default-dop4-fullscan-void-pull.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `sql/handler.h` / `sql/handler.cc` public API shape；
- `sql/parallel_query/sql_parallel.*` worker task semantics, unless RED shows
  existing D2 producer cannot handle DOP4；
- `storage/innobase/row/row0pread_pq.cc`；
- ref/range/ICP/ORDER BY/GROUP BY/partition/MVI eligibility；
- old `parallel_query_experimental_threaded_dop4` variable removal。

## Task 1: RED - DOP4 default fullscan should not need experimental gate

**Files:**

- Modify: `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- Modify: `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`

Change the existing DOP4 window:

- keep `SET SESSION parallel_default_dop=4`;
- do not enable `parallel_query_experimental_threaded_dop4`;
- add D2 threaded counter baselines around the DOP4 query:
  - `Parallel_visible_void_pull_threaded_selected`
  - `Parallel_visible_void_pull_threaded_rows`
  - `Parallel_visible_void_pull_threaded_finishes`
  - `Parallel_visible_void_pull_threaded_workers`
  - `Parallel_visible_void_pull_threaded_failures`

Expected DOP4 result for the existing 8-row fullscan:

```text
executed_delta fallback_delta rows_delta workers_delta
1 0 8 4
threaded_void_selected_delta threaded_void_rows_delta threaded_void_finishes_delta threaded_void_workers_delta threaded_void_failures_delta
1 8 4 4 0
```

Run RED:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan \
  --vardir=/tmp/pq-d21-red-vardir \
  --tmpdir=/tmp/pq-d21-red-tmpdir
```

Expected: FAIL before code change because default DOP4 is not on the D2
void-pull threaded path.

## Task 2: GREEN - Accept DOP4 in D2 eligibility

**Files:**

- Modify: `sql/parallel_query/pq_iterator.cc`

Implementation:

- update `should_enter_threaded_visible_void_pull_fullscan_path()` to accept
  `requested_dop == 2 || requested_dop == 4`;
- keep the existing table shape guard:
  - `m_join->pq_eligible`;
  - `parallel_query=ON`;
  - no BLOB;
  - valid record length;
  - `pq_table_has_read_fields(table())`;
- keep explicit DBUG hooks precedence:
  - `pq_read_threaded_pqwr_record_gather_path` still wins;
  - `pq_visible_void_pull_fullscan_path` still forces the old synchronous path.
- keep explicit legacy DOP4 shadow/callback coverage reachable:
  - `parallel_query_experimental_threaded_dop4=ON` still selects the old
    shadow/callback producer;
  - `pq_read_threaded_dop4_shadow_path` still selects the old shadow/callback
    producer for debug-only worker error / external kill tests.

Do not make DOP1 default in D3. DOP1 has different commercial value and should
remain a separate decision.

## Task 3: Review and verification

Run:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d21-final-vardir \
  --tmpdir=/tmp/pq-d21-final-tmpdir
```

Independent review focus:

- DOP4 default really uses D2 `VOID_PULL_RECORD_IMAGE_PRODUCER`;
- old experimental DOP4 shadow path is not removed;
- worker count/FINISH count equals 4 for DOP4;
- no non-fullscan path is opened.

## Completion Report

Status: completed.

Changed files:

- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- `mysql-test/suite/parallel_query/r/pq_stats.result`

Implementation:

- Extended default threaded visible void-pull fullscan eligibility from DOP2
  to DOP2/DOP4.
- Added explicit DOP4 shadow/callback precedence so
  `parallel_query_experimental_threaded_dop4=ON` and
  `pq_read_threaded_dop4_shadow_path` continue to exercise the legacy producer.
- Converted the existing `pq_commercial_fullscan` DOP4 window from experimental
  DOP4 to default DOP4, with threaded void-pull selected/rows/FINISH/workers
  assertions.
- Updated `pq_stats` fallback/executed expectations for the newly visible
  DOP4 execution path. `GROUP BY` remains non-fallback.

TDD RED:

```text
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan \
  --vardir=/tmp/pq-d21-red-vardir \
  --tmpdir=/tmp/pq-d21-red-tmpdir
```

Observed expected failure before production code change:

```text
expected: executed_delta=1 fallback_delta=0 rows_delta=8 workers_delta=4
actual:   executed_delta=0 fallback_delta=1 rows_delta=0 workers_delta=0

expected threaded_void_selected/rows/finishes/workers/failures = 1/8/4/4/0
actual   threaded_void_selected/rows/finishes/workers/failures = 0/0/0/0/0
```

Verification:

```text
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather \
  pq_read_threaded_dop4_worker_error pq_read_threaded_dop4_external_kill \
  pq_stats \
  --vardir=/tmp/pq-d21-final-v3-vardir \
  --tmpdir=/tmp/pq-d21-final-v3-tmpdir
```

Result: all 6 tests passed. Build passed with the existing
`Prepared_statement` final-class virtual destructor warning.

Review:

- First independent review rejected the initial diff because default DOP4
  visible void-pull shadowed legacy DOP4 shadow/callback tests.
- Fixed by preserving explicit DOP4 shadow/callback precedence.
- Final independent review: ACCEPT, no Critical/Important/Minor findings.
  Reviewer confirmed the DOP4 priority issue is resolved, DOP2 is unchanged,
  PQWR DBUG gate still wins, visible DBUG gate still wins, and `pq_stats`
  expectations match D3 behavior.

Residual risk:

- DOP4 now makes ordinary eligible fullscan / implicit aggregate queries count
  as executed rather than fallback. This is intended for D3 but changes old
  `pq_stats` expectations.
- Legacy DOP4 shadow/callback path remains for explicit experimental and DBUG
  coverage; it is not the default commercial-shaped DOP4 path.
