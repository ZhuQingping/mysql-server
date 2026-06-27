# D1.9 Default Fullscan Void-Pull Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ordinary eligible clustered fullscan DOP2 execution use the D1.8 visible void-pull path by default, moving current behavior closer to the taurusdbondstore commercial `ha_pq_next(void*)` worker scan path.

**Architecture:** D1.8 already proved a user-visible DBUG path where `PQTableScanIterator::Read()` pulls rows via worker handler `ha_pq_next(worker_record, leader_pq_ctx)`. D1.9 promotes that path into the default eligible clustered fullscan branch, while retaining the old PQWR record-gather path only behind its explicit DBUG hook for regression comparison. The change must remain scoped to fullscan and must not open ref/range/ICP/ORDER BY/partition/MVI/native Record_buffer paths.

**Tech Stack:** MySQL 8.0.46 debug build, Parallel Query SQL iterator layer, InnoDB PQ handler bridge, MTR `parallel_query` suite.

---

## 状态

Status: design prepared；coding not started。

## 背景

D1.8 已提交为 `3a040d891bd Add PQ visible void pull fullscan gate`，证明：

- `PQTableScanIterator::Init()` 可以创建 leader `pq_ctx`、独立 worker
  THD/TABLE/handler/typed worker context；
- `PQTableScanIterator::Read()` 可以逐行调用 worker handler
  `ha_pq_next(worker_record, leader_pq_ctx)`；
- worker record image 可以 copy 回 leader `table()->record[0]`；
- EOF / error / KILL / cleanup 在 targeted MTR 下通过；
- 默认 DOP2 visible fullscan 仍未切换，当前默认 eligible fullscan 分支仍优先
  进入 threaded shadow / PQWR 相关路径，而不是 commercial void-pull 形态。

taurusdbondstore 商用 fullscan worker 读取主形态见：

```text
PQblockScanIterator::Read()
  -> table()->file->ha_pq_next(m_record, m_pq_ctx)
  -> ha_innobase::pq_worker_scan_next(void*, uchar*)
```

D1.9 的迁移目标是让当前仓库的普通 DOP2 clustered fullscan 也默认走
`ha_pq_next(void*)` visible path，减少与商用实现的核心行为差异。

## 允许修改

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/mysqld.cc`
- `storage/innobase/handler/ha_innodb_pq.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `mysql-test/suite/parallel_query/t/pq_read_threaded_pqwr_record_gather.test`
- `mysql-test/suite/parallel_query/r/pq_read_threaded_pqwr_record_gather.result`
- `Docs/pq_tasks/commercial-port-d19-default-fullscan-void-pull.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `sql/handler.h` / `sql/handler.cc` public API shape
- `sql/parallel_query/query_result_mq.*`
- `sql/parallel_query/exchange.*`
- `sql/join_optimizer/access_path.cc`
- `sql/sql_executor.*`
- `storage/innobase/row/row0pread_pq.cc`
- ORDER BY / GROUP BY / ref / range / ICP / partition / MVI production path

## Task 1: RED - 默认 fullscan 应选择 visible void-pull

**Files:**

- Modify: `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- Modify: `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`

- [ ] **Step 1: Add default-path counter window**

在已有默认 DOP2 fullscan 正向查询附近增加一个独立窗口：

```sql
SET @default_void_selected_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_void_pull_selected');
SET @default_void_rows_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_void_pull_rows');
SET @default_void_eofs_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_void_pull_eofs');
SET @default_void_cleanup_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_void_pull_cleanup');
SET @default_void_failures_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_void_pull_failures');
SET @default_pqwr_selected_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_pqwr_record_gather_selected');
SET @default_pqwr_rows_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_visible_pqwr_record_gather_rows');

--sorted_result
SELECT id FROM t1 WHERE val >= 10;

SELECT
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_void_pull_selected') -
    @default_void_selected_before AS default_void_selected_delta,
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_void_pull_rows') -
    @default_void_rows_before AS default_void_rows_delta,
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_void_pull_eofs') -
    @default_void_eofs_before AS default_void_eofs_delta,
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_void_pull_cleanup') -
    @default_void_cleanup_before AS default_void_cleanup_delta,
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_void_pull_failures') -
    @default_void_failures_before AS default_void_failures_delta;

SELECT
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_pqwr_record_gather_selected') -
    @default_pqwr_selected_before AS default_pqwr_selected_delta,
  (SELECT CAST(variable_value AS UNSIGNED) FROM performance_schema.global_status
   WHERE variable_name = 'Parallel_visible_pqwr_record_gather_rows') -
    @default_pqwr_rows_before AS default_pqwr_rows_delta;
```

期望 result：

```text
default_void_selected_delta default_void_rows_delta default_void_eofs_delta default_void_cleanup_delta default_void_failures_delta
1 8 1 1 0
default_pqwr_selected_delta default_pqwr_rows_delta
0 0
```

- [ ] **Step 2: Run RED**

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan \
  --vardir=/tmp/pq-d19-red-vardir \
  --tmpdir=/tmp/pq-d19-red-tmpdir
```

Expected: FAIL because default DOP2 still does not increment
`Parallel_visible_void_pull_*` counters.

## Task 2: GREEN - Promote D1.8 path to default fullscan

**Files:**

- Modify: `sql/parallel_query/pq_iterator.h`
- Modify: `sql/parallel_query/pq_iterator.cc`

- [ ] **Step 1: Extract D1.8 Init code into a helper**

Add a private helper to `PQTableScanIterator`:

```c++
bool init_visible_void_pull_fullscan_path();
```

Move the D1.8 DBUG block body into that helper without changing behavior:

- allocate `Gather_operator(1)`;
- normalize fullscan handler state (`pq_ref=false`, `pq_ref_depend=false`,
  `pq_range_type=PQ_QUICK_SELECT_NONE`, `ha_set_reverse_scan(false)`);
- call `ha_pq_init(1, MAX_KEY)`;
- set `m_leader_ctx_from_ha_pq_init=true` and `m_use_visible_void_pull=true`;
- configure worker open contexts;
- create worker THD, open worker table, call typed worker scan init;
- restore leader globals;
- increment `visible_void_pull_selected`;
- `mark_pq_started()`;
- return `false` on success, `true` on error after cleanup and `PrintError()`.

- [ ] **Step 2: Add a default fullscan eligibility helper**

Add a private helper:

```c++
bool should_enter_visible_void_pull_fullscan_path(uint requested_dop) const;
```

The default branch returns true only when:

- `thd() != nullptr`;
- `m_join != nullptr && m_join->pq_eligible`;
- `thd()->variables.parallel_query`;
- `requested_dop == 2`;
- `table() != nullptr && table()->s != nullptr`;
- `table()->s->blob_fields == 0`;
- `table()->s->reclength > 0`;
- `pq_table_has_read_fields(table())`;
- `!DBUG` hook is not required.

Keep `DBUG_EXECUTE_IF("pq_visible_void_pull_fullscan_path", enabled = true;)`
inside the helper so the D1.8 explicit path still works.

- [ ] **Step 3: Reorder default path selection**

In `PQTableScanIterator::Init()` after smoke-only DBUG hooks and before
`threaded_pqwr_record_gather_path` selection:

```c++
const bool visible_void_pull_fullscan_path =
    should_enter_visible_void_pull_fullscan_path(requested_dop);
const bool threaded_pqwr_record_gather_path =
    !visible_void_pull_fullscan_path &&
    should_enter_threaded_pqwr_record_gather_path(requested_dop);
...
if (visible_void_pull_fullscan_path) {
  return init_visible_void_pull_fullscan_path();
}
```

The old PQWR path remains reachable only by its explicit DBUG hook or if D1.9
helper returns false. Do not delete PQWR code or counters in D1.9.

- [ ] **Step 4: Run GREEN targeted build/MTR**

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d19-green-vardir \
  --tmpdir=/tmp/pq-d19-green-tmpdir
```

Expected: all pass. `pq_read_threaded_pqwr_record_gather` must continue to pass
because it uses the explicit DBUG hook for legacy PQWR path coverage.

## Task 3: Review, docs, commit

**Files:**

- Modify: `Docs/pq_tasks/commercial-port-d19-default-fullscan-void-pull.md`
- Modify: `Docs/pq_tasks/README.md`
- Modify: `Docs/pq_tasks/commercial-full-port-sprint.md`

- [ ] **Step 1: Update completion report**

Record:

- changed files;
- RED command and failure summary;
- build/MTR command output summary;
- independent review result;
- any deferred risk.

- [ ] **Step 2: Request independent review**

Review focus:

- default DOP2 fullscan now selects `ha_pq_next(void*)`;
- PQWR remains available only as explicit regression hook;
- no ref/range/ICP/ORDER BY paths opened;
- cleanup and KILL semantics retained from D1.8;
- MTR proves default void-pull and PQWR non-growth in default window.

- [ ] **Step 3: Fix Critical/Important findings and rerun verification**

Required commands after fixes:

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d19-final-vardir \
  --tmpdir=/tmp/pq-d19-final-tmpdir
```

- [ ] **Step 4: Commit and push**

```bash
git add \
  Docs/pq_tasks/README.md \
  Docs/pq_tasks/commercial-full-port-sprint.md \
  Docs/pq_tasks/commercial-port-d19-default-fullscan-void-pull.md \
  mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result \
  mysql-test/suite/parallel_query/r/pq_read_threaded_pqwr_record_gather.result \
  mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test \
  mysql-test/suite/parallel_query/t/pq_read_threaded_pqwr_record_gather.test \
  sql/parallel_query/pq_iterator.cc \
  sql/parallel_query/pq_iterator.h
git commit -m "Use PQ void pull for default fullscan"
git push
```

## Acceptance Checklist

- [ ] Default eligible DOP2 clustered fullscan increments
  `Parallel_visible_void_pull_selected/rows/eofs/cleanup` and not PQWR visible
  counters.
- [ ] Explicit `pq_visible_void_pull_fullscan_path` DBUG path remains valid.
- [ ] Explicit `pq_read_threaded_pqwr_record_gather_path` regression path remains
  valid.
- [ ] BLOB/TEXT fallback in `pq_commercial_fullscan` remains serial.
- [ ] No public handler API changes.
- [ ] No ref/range/ICP/ORDER BY/partition/MVI production path opened.
