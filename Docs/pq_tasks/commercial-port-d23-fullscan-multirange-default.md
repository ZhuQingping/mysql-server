# D5 Default Fullscan Multi-Range Coverage Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use TDD for test changes and
> request independent review before commit.

**Goal:** Add no-debug MTR coverage proving default DOP2/DOP4 clustered
fullscan drains multiple InnoDB PQ ranges through the commercial-shaped
threaded visible void-pull path.

D2/D3/D4 opened the default fullscan path for positive DOP values. Existing
multi-range coverage still primarily exercises debug-only shadow/callback
paths. D5 adds a default-path guard without changing production source.

---

## 状态

Status: design ready；等待 TDD 测试实现。

## 允许修改

- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan_multirange.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan_multirange.result`
- `Docs/pq_tasks/commercial-port-d23-fullscan-multirange-default.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- production source files
- debug-only shadow/callback tests
- range/ref/ICP/ORDER BY/GROUP BY behavior

## RED / GREEN Plan

Create `pq_commercial_fullscan_multirange`:

- build a 1024-row InnoDB table;
- `parallel_query=ON`;
- `parallel_cost_threshold=0`;
- no `debug`;
- no `parallel_query_experimental_threaded_dop*`;
- run DOP2 and DOP4 fullscan queries;
- verify complete aggregate result (`COUNT`, `SUM`, `MIN`, `MAX`);
- verify:
  - `Parallel_queries_executed` delta `1`;
  - `Parallel_queries_fallback` delta `0`;
  - `Parallel_rows_scanned` delta `1024`;
  - `Parallel_workers_launched` delta equals DOP;
  - threaded visible void-pull selected delta `1`;
  - threaded visible void-pull rows delta `1024`;
  - threaded visible void-pull FINISH/workers delta equals DOP;
  - failures delta `0`;
  - `Parallel_ranges_built` and `Parallel_ranges_dispatched` deltas are at
    least DOP.

Run:

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan_multirange \
  --vardir=/tmp/pq-d23-vardir \
  --tmpdir=/tmp/pq-d23-tmpdir
```

Expected: PASS without production source changes. If it fails, investigate
before widening D5.

## Verification

Run:

```bash
git diff --check
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan_multirange pq_commercial_fullscan_dop \
  pq_read_threaded_dop2_multirange pq_read_threaded_dop4_shadow_multirange \
  --vardir=/tmp/pq-d23-final-vardir \
  --tmpdir=/tmp/pq-d23-final-tmpdir
```

Build is optional if D5 remains test/docs-only.

## Completion Report

Pending.
