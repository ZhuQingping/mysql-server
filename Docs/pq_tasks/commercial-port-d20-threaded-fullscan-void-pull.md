# D2 Default Threaded Fullscan Void-Pull Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development or superpowers:executing-plans to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** Move ordinary eligible clustered fullscan DOP2 execution from the
D1.9 leader-synchronous visible void-pull path to a real worker-thread producer
topology, so the user-visible fullscan shape is closer to the taurusdbondstore
commercial `ParallelScanIterator -> worker thread -> PQblockScanIterator ->
ha_pq_next() -> leader gather` path.

**Architecture:** D1.9 made default fullscan call worker handler
`ha_pq_next(void*)`, but the leader still performs the pull directly inside
`PQTableScanIterator::Read()`. Commercial fullscan starts worker threads during
`ParallelScanIterator::Init()`, workers scan and send row frames, and leader
`Read()` consumes from gather/exchange. D2 should reuse the current
`Gather_operator` worker manager, Exchange/MQ materialization, and D1.9 typed
worker scan context. It must remain scoped to clustered fullscan.

**Tech Stack:** MySQL 8.0.46 debug build, Parallel Query SQL iterator layer,
InnoDB PQ handler bridge, MTR `parallel_query` suite.

---

## 状态

Status: design ready；等待编码。

## 背景

D1.9 已完成：

- 普通 eligible DOP2 clustered fullscan 默认进入
  `PQTableScanIterator::init_visible_void_pull_fullscan_path()`；
- `PQTableScanIterator::Read()` 在 leader 线程中切换到 worker THD globals，
  调 worker handler `ha_pq_next(worker_record, leader_pq_ctx)`；
- 行复制回 leader `table()->record[0]`，并覆盖 EOF/error/KILL/cleanup；
- targeted MTR 已证明结果正确。

剩余核心差异：

- 当前默认路径不启动真实 worker 线程，`Parallel_workers_launched` 对 D1.9
  默认 fullscan 仍为 `0`；
- 商用参考 `ParallelScanIterator::Init()` 的关键顺序是：
  `pq_init_record_gather() -> m_gather->init() -> pq_create_innodb_snapshot()
  -> pq_launch_worker()`；
- 商用 `ParallelScanIterator::Read()` 只从 `MQ_record_gather` 拉 row，EOF
  后 `End()` detach/wait/merge error；
- D2 目标是收敛 topology，不扩大到 ref/range/ICP/ORDER BY/partition/MVI。

## 当前代码依据

- 当前默认 D1.9 入口：
  `sql/parallel_query/pq_iterator.cc` 中
  `should_enter_visible_void_pull_fullscan_path()` 与
  `init_visible_void_pull_fullscan_path()`。
- 当前 leader MQ 消费循环：
  `PQTableScanIterator::Read()` 中非 `m_use_visible_void_pull` 分支已经支持
  `Exchange_nosort::materialize_next_record_image_status()`、WOULD_BLOCK 等待、
  EOF/error/KILL/cleanup。
- 当前可复用 worker thread producer：
  `Gather_operator::run_worker_callback_threaded_producer()` 和
  `run_worker_callback_pqwr_threaded_producer()` 已负责配置 worker task、
  thread budget、`start_workers()` 和
  `Parallel_workers_launched` 计数。
- 商用参考：
  `/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/sql/parallel_query/pq_iterators.cc`
  中 `ParallelScanIterator::pq_launch_worker()`、
  `ParallelScanIterator::Init()`、`ParallelScanIterator::Read()` 和
  `PQblockScanIterator::Read()`。

## 允许修改

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `sql/parallel_query/sql_parallel.h`
- `sql/parallel_query/sql_parallel.cc`
- `sql/mysqld.cc`
- `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`
- `mysql-test/suite/parallel_query/t/pq_read_threaded_pqwr_record_gather.test`
- `mysql-test/suite/parallel_query/r/pq_read_threaded_pqwr_record_gather.result`
- `mysql-test/suite/parallel_query/t/pq_stats.test`
- `mysql-test/suite/parallel_query/r/pq_stats.result`
- `Docs/pq_tasks/commercial-port-d20-threaded-fullscan-void-pull.md`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/commercial-full-port-sprint.md`

## 禁止修改

- `sql/handler.h` / `sql/handler.cc` public API shape；
- `storage/innobase/row/row0pread_pq.cc` range/partition 算法；
- ref/range/ICP/ORDER BY/GROUP BY production eligibility；
- `Query_result_mq` wire protocol 大改；
- 当前 DBUG-only PQWR regression hook 删除；
- unrelated docs、历史 patch、worktree artifact。

## Task 1: RED - 默认 fullscan 必须启动 worker 线程

**Files:**

- Modify: `mysql-test/suite/parallel_query/t/pq_commercial_fullscan.test`
- Modify: `mysql-test/suite/parallel_query/r/pq_commercial_fullscan.result`

- [ ] **Step 1: Add default-threaded counter window**

D2 首选新增专用 status counters，避免只用 `Parallel_workers_launched`
时被其他 smoke/debug 路径污染：

- `Parallel_visible_void_pull_threaded_selected`
- `Parallel_visible_void_pull_threaded_rows`
- `Parallel_visible_void_pull_threaded_finishes`
- `Parallel_visible_void_pull_threaded_workers`
- `Parallel_visible_void_pull_threaded_failures`

在 D1.9 默认 fullscan counter window 附近增加 worker launch 和 D2 专用
counter 断言：

```sql
SET @default_threaded_workers_before = (
  SELECT CAST(variable_value AS UNSIGNED)
    FROM performance_schema.global_status
    WHERE variable_name = 'Parallel_workers_launched');

--sorted_result
SELECT id FROM t1 WHERE val >= 10;

SELECT
  (SELECT CAST(variable_value AS UNSIGNED)
     FROM performance_schema.global_status
     WHERE variable_name = 'Parallel_workers_launched') -
    @default_threaded_workers_before AS default_threaded_workers_delta;
```

期望 result：

```text
default_threaded_workers_delta
2
```

保留 D1.9 visible void-pull counters 或迁移为 D2 threaded counters，作为
默认 fullscan 仍进入 commercial void-pull family 的证据。若 D2 改为 Exchange
record-image materializer，`Parallel_visible_void_pull_rows` 可以改为新增 D2
threaded counters；但必须同时证明：

- 默认 DOP2 query 返回完整结果；
- `Parallel_workers_launched` 增加 2；
- legacy PQWR DBUG hook 仍可通过独立测试覆盖。

- [ ] **Step 2: Run RED**

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan \
  --vardir=/tmp/pq-d20-red-vardir \
  --tmpdir=/tmp/pq-d20-red-tmpdir
```

Expected: FAIL because D1.9 default fullscan currently does not launch worker
threads.

## Task 2: GREEN - Add default threaded fullscan producer

**Files:**

- Modify: `sql/parallel_query/pq_iterator.h`
- Modify: `sql/parallel_query/pq_iterator.cc`
- Modify: `sql/parallel_query/sql_parallel.h`
- Modify: `sql/parallel_query/sql_parallel.cc`
- Modify: `sql/mysqld.cc` only if new status counters are needed

- [ ] **Step 1: Add an explicit D2 path flag**

Add a private `PQTableScanIterator` path flag, for example:

```c++
bool m_use_threaded_visible_void_pull{false};
```

The existing `m_use_visible_void_pull` remains the synchronous D1.9 fallback or
DBUG regression path. Do not overload it with threaded semantics.

- [ ] **Step 2: Add default threaded eligibility helper**

Add:

```c++
bool should_enter_threaded_visible_void_pull_fullscan_path(
    uint requested_dop) const;
```

It should match D1.9 fullscan guard at minimum:

- `thd() != nullptr`；
- `m_join != nullptr && m_join->pq_eligible`；
- `thd()->variables.parallel_query`；
- `requested_dop == 2`；
- `table() != nullptr && table()->s != nullptr`；
- no BLOB fields；
- valid record length；
- `pq_table_has_read_fields(table())`；
- fullscan only, no ref/range/ICP/ORDER BY expansion.

Keep DBUG hooks explicit:

- `pq_visible_void_pull_fullscan_path` should still force the D1.9 synchronous
  fallback path for regression;
- `pq_read_threaded_pqwr_record_gather_path` should still force legacy PQWR
  path.

- [ ] **Step 3: Start threaded producer in Init**

In `PQTableScanIterator::Init()` choose D2 before D1.9 synchronous path:

1. initialize leader EXECUTE/fullscan context using the current typed context
   contract;
2. create `Gather_operator(requested_dop)` and `Exchange_nosort`;
3. configure worker open contexts;
4. start worker-thread producer that reads worker-local rows and enqueues record
   images;
5. set `m_use_threaded_visible_void_pull=true`;
6. call `mark_pq_started()`;
7. return false.

Preferred implementation direction:

- add a narrowly scoped worker task for threaded void-pull fullscan
  that uses worker TABLE/handler/ctx and sends record-image ROW + FINISH into
  `Exchange_nosort`;
- reuse `run_worker_callback_threaded_producer()` only as a lifecycle template,
  not as a semantic substitute, because callback producer currently does not
  prove the commercial `ha_pq_next(void*)` worker pull chain;
- keep cleanup through existing `cleanup_pq_resources(true/false)`,
  `abort_workers()`, `destroy()`, and worker join path.

- [ ] **Step 4: Reuse leader Exchange Read path**

For D2, `PQTableScanIterator::Read()` should enter the existing non-void-pull
Exchange loop:

- ROW: materialize into leader `table()->record[0]` and return `0`;
- EOF: resolve worker errors, cleanup, return `-1`;
- WOULD_BLOCK: bounded wait and retry;
- KILL: propagate to workers, cleanup, send kill message;
- ERROR: use existing error priority.

Do not add another leader-side direct `ha_pq_next()` loop for D2.

- [ ] **Step 5: Preserve regression hooks**

- `pq_visible_void_pull_fullscan_path` continues to cover D1.9 direct
  `ha_pq_next()` fallback；
- `pq_read_threaded_pqwr_record_gather_path` continues to cover old PQWR
  record-gather path；
- existing `pq_read_threaded_*` tests must remain meaningful.

## Task 3: Review and targeted verification

- [ ] **Step 1: Run basic verification**

```bash
git diff --check
cmake --build build-ninja --target mysqld -j 8
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 \
  pq_commercial_fullscan pq_read_threaded_pqwr_record_gather pq_stats \
  --vardir=/tmp/pq-d20-green-vardir \
  --tmpdir=/tmp/pq-d20-green-tmpdir
```

- [ ] **Step 2: Request independent review**

Review focus:

- default path really launches worker threads and does not silently fall back to
  synchronous D1.9；
- worker THD/TABLE/handler lifecycle is owned and cleaned once；
- leader `Read()` cannot return EOF before worker FINISH/ERROR is observed；
- KILL/error cleanup joins or detaches worker resources correctly；
- DBUG regression hooks still exercise their intended paths；
- no ref/range/ICP/ORDER BY paths are opened accidentally.

- [ ] **Step 3: Fix Critical/Important issues**

Run the same targeted verification after fixes.

## Task 4: Docs and commit

- [ ] Update this file with completion report:
  changed files, RED failure, build/MTR evidence, review result, risks.
- [ ] Update `Docs/pq_tasks/README.md` current summary.
- [ ] Update `Docs/pq_tasks/commercial-full-port-sprint.md` current status.
- [ ] Commit as a small standalone D2 commit.
- [ ] Push branch after commit.

## 风险与边界

- Threaded D2 must not reuse the worker handler from leader thread after worker
  thread starts; worker-owned THD/TABLE/handler context must stay local to the
  worker task or be cleaned under a clear ownership rule.
- `m_use_visible_void_pull` cleanup currently switches THD globals around
  worker handler cleanup. D2 should avoid mixing that cleanup branch unless it
  intentionally uses the same synchronous context shape.
- If D2 uses record-image Exchange rather than PQWR string frames, test counters
  should distinguish it from D1.9 direct void-pull and legacy PQWR path.
- If full commercial worker plan cloning is required before threaded void-pull
  can be safely enabled, stop after RED/design and record the blocker. Do not
  fake worker launch counters without a real worker thread.
