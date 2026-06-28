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

Pending.
