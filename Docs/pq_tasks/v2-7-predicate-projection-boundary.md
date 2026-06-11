# V2-7 Predicate/Projection Boundary 任务书

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. 本项目阶段文档使用中文；代码标识、命令、MySQL 概念保留英文。

**Goal:** 明确真实 PQ row stream 打开后 predicate/projection 的执行边界：worker 只产 base row，leader 继续使用现有 SQL executor/Item 逻辑执行 WHERE/projection；本阶段不引入 worker-side Item clone / JOIN clone。

**Architecture:** 当前 V2-6 只完成 synthetic Exchange/Gather smoke，`PQTableScanIterator::Read()` 仍走 serial fallback。V2-7 的首要价值是锁定 contract 和测试边界，避免下一阶段 V2-8 打开真实 full scan 时错误地把 predicate/projection 下推到 worker。实现上优先增加负向/边界 MTR 和文档，必要时增加轻量状态/注释，不做真实 row materialization。

**Tech Stack:** MySQL 8.0.46 debug build, SQL iterator framework, `PQTableScanIterator`, MySQL Item/WHERE/projection existing executor, MTR `parallel_query` suite.

---

## Current Baseline

- Baseline commit: `d4d784ab802 Add PQ V2-6 exchange row stream smoke`
- V2-6 已完成：
  - `Exchange_nosort::run_synthetic_row_stream_smoke()`；
  - `Gather_operator::run_exchange_row_stream_smoke()`；
  - `Parallel_exchange_smoke_rows` / `Parallel_exchange_smoke_finishes`；
  - `pq_exchange_rows_dop1`；
  - `PQTableScanIterator::Read()` 仍只 delegate 到 serial fallback iterator。
- InnoDB `pq_worker_scan_init()` / `pq_worker_scan_next()` 仍 unsupported。

## Core Contract

V2-7 明确以下合同：

1. Worker 只负责产生 base row image，不执行 SQL `Item` predicate；
2. Worker 不 clone `JOIN`、`Query_block`、`Item` tree、`TABLE_LIST`；
3. Leader `PQTableScanIterator::Read()` 未来填充 `table->record[0]` 后，现有 SQL executor 继续执行 WHERE、projection、聚合上层逻辑；
4. `SELECT *`、`SELECT cols`、简单 WHERE 的 correctness 必须等真实 row materialization 后由 leader 侧验证；
5. 当前 synthetic row/token 不允许进入 `Read()` 用户可见结果；
6. 在真实 row stream 打开前，所有 predicate/projection 查询仍必须 serial fallback。

## Scope

允许：

- 增加 V2-7 boundary MTR，验证 `SELECT cols`、简单 WHERE、空结果在当前阶段仍 serial fallback，且 smoke counters 不被 EXPLAIN 污染；
- 更新 `v2-test-matrix.md`，把 V2-7 当前验收从“真实 WHERE/projection 正确”拆成 contract boundary 和 V2-8 follow-up；
- 在 `pq_iterator.*` 增加注释或轻量 helper，明确 `Read()` 未来 row materialization 责任；
- 更新 README / Completion Report。

禁止：

- 不允许 `PQTableScanIterator::Read()` 返回 synthetic row；
- 不允许 worker-side Item/JOIN clone；
- 不允许修改 optimizer eligibility 扩大到 ORDER BY/GROUP BY/JOIN；
- 不允许调用 InnoDB worker next 读取真实 row；
- 不允许设置 `PQ_execution_state::EXECUTED`；
- 不允许增加 `Parallel_queries_executed`、`Parallel_workers_launched`、`Parallel_rows_scanned`。

## File Structure

- Modify `mysql-test/suite/parallel_query/t/pq_projection_where_boundary.test`
- Modify `mysql-test/suite/parallel_query/r/pq_projection_where_boundary.result`
- Modify `Docs/pq_tasks/v2-test-matrix.md`
- Modify `Docs/pq_tasks/v2-execution-path-roadmap.md`
- Modify `Docs/pq_tasks/README.md`
- Modify `Docs/pq_tasks/v2-7-predicate-projection-boundary.md`
- Optional modify `sql/parallel_query/pq_iterator.cc`
  - only comments/helper names; no behavior change unless a test proves a counter/state bug.

## Implementation Tasks

### Task 1: 写 boundary RED/GREEN MTR

**Files:**

- Create: `mysql-test/suite/parallel_query/t/pq_projection_where_boundary.test`
- Create: `mysql-test/suite/parallel_query/r/pq_projection_where_boundary.result`

Test intent:

- 建 InnoDB 单表：
  - `id INT PRIMARY KEY`
  - `val INT`
  - `pad CHAR(20)`
- 设置 `parallel_query=ON`、`parallel_cost_threshold=0`。
- 记录以下 counters：
  - `Parallel_queries_executed`
  - `Parallel_queries_fallback`
  - `Parallel_rows_scanned`
  - `Parallel_workers_launched`
  - `Parallel_worker_smoke_runs`
  - `Parallel_exchange_smoke_rows`
- 执行：
  - `SELECT id FROM t1 WHERE val >= 20`
  - `SELECT val, pad FROM t1 WHERE id IN (2,4)`
  - `SELECT id FROM t1 WHERE val > 999`
- 断言结果与串行语义一致；
- 断言：
  - `executed_delta = 0`
  - `workers_delta = 0`
  - `rows_delta = 0`
  - `fallback_delta` 与 eligible full scan 查询数量一致；
  - smoke counters 可以增加，但只说明 synthetic/smoke，不说明真实 execution。

### Task 2: EXPLAIN boundary

在同一或单独 MTR 中覆盖：

- `EXPLAIN SELECT id FROM t1 WHERE val >= 20`
- `EXPLAIN FORMAT=TREE SELECT id FROM t1 WHERE val >= 20`
- `EXPLAIN FORMAT=JSON SELECT id FROM t1 WHERE val >= 20`

断言：

- EXPLAIN 不增加 fallback/smoke counters；
- 文案仍是 candidate/eligible + execution disabled/serial fallback；
- 不出现 actual executed / workers launched 类文案。

### Task 3: 文档收敛

**Files:**

- Modify: `Docs/pq_tasks/v2-test-matrix.md`
- Modify: `Docs/pq_tasks/v2-execution-path-roadmap.md`
- Modify: `Docs/pq_tasks/README.md`
- Modify: `Docs/pq_tasks/v2-7-predicate-projection-boundary.md`

Required updates:

- 说明 V2-7 当前是 boundary phase，不打开真实 row materialization；
- 把“leader 执行 WHERE/projection 的真实结果正确性”明确放到 V2-8 full scan closure 后验收；
- 记录 smoke counters 的解释边界。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16

build-ninja/runtime_output_directory/mysqladmin --no-defaults --socket=/private/tmp/pq20.sock -uroot shutdown 2>/dev/null || true
rm -f /private/tmp/pq20.sock /private/tmp/pq-v2-4-mysql.pid /private/tmp/pq-v2-4-mysql.log
build-ninja/runtime_output_directory/mysqld --no-defaults \
  --datadir=/private/tmp/pq-v2-0-mysql \
  --basedir=$PWD/build-ninja \
  --lc-messages-dir=$PWD/share \
  --port=3350 \
  --socket=/private/tmp/pq20.sock \
  --pid-file=/private/tmp/pq-v2-4-mysql.pid \
  --log-error=/private/tmp/pq-v2-4-mysql.log \
  --skip-name-resolve \
  --mysqlx=0 \
  --daemonize

cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_projection_where_boundary pq_exchange_rows_dop1 pq_stats
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root

../../build-ninja/runtime_output_directory/mysqladmin --no-defaults --socket=/private/tmp/pq20.sock -uroot shutdown 2>/dev/null || true
```

## Acceptance Checklist

- [ ] `SELECT cols`、简单 WHERE、空结果在当前阶段结果正确；
- [ ] 查询仍 serial fallback，不设置真实 executed/workers/rows；
- [ ] EXPLAIN 不污染 fallback/smoke counters；
- [ ] 文档明确 worker-side Item/JOIN clone 禁止；
- [ ] V2-8 前置条件明确：真实 row materialization 后再验收 leader-side predicate/projection；
- [ ] targeted MTR 通过；
- [ ] full `parallel_query` suite 通过。

## Current Status

- Status: In Progress
- Owner: Codex Orchestrator
- Started: 2026-06-11

## Completion Report

待实现后补充。
