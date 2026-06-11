# V2-1 Iterator Ownership + Safe Fallback 任务书

## Goal

在 V2-0 execution state contract 之后，建立 `PQTableScanIterator` 的真实 ownership 形态：eligible 的普通执行可以返回非空 PQ iterator，但在真实 worker/InnoDB 扫描尚未接通前，`Init()` 必须在安全窗口内回退到内部 serial `TableScanIterator`，保证结果、handler 状态和计数语义稳定。

本阶段仍不启用真实并行执行。

## Scope

允许：

- `TryCreatePQTableScanIterator()` 对普通 eligible execution 返回 `PQTableScanIterator`；
- `PQTableScanIterator` 内部持有 serial fallback iterator；
- `PQTableScanIterator::Init()` 在未启动 worker、未调用 InnoDB PQ API、未改变 handler scan 状态前进入 serial fallback；
- fallback counter 由 iterator `Init()` 记录，EXPLAIN 不记录；
- `Read()` 和 row iterator 相关方法委托给内部 serial iterator。

禁止：

- 调用 InnoDB PQ scan API；
- 启动 worker THD；
- 使用 Exchange/Gather row stream；
- 对 DOP>1 或真实并行执行做能力宣称；
- 修改 `storage/innobase/**`。

## Design Requirements

### Iterator ownership

`PQTableScanIterator` 必须拥有一个 `unique_ptr_destroy_only<RowIterator>` 作为 serial fallback。该 fallback 使用同一个 `MEM_ROOT` 创建，并持有与原 serial `TableScanIterator` 相同的参数：

- `THD *thd`
- `TABLE *table`
- `expected_rows`
- `examined_rows`

### Safe fallback window

V2-1 的 `Init()` 必须在以下动作之前 fallback：

- `handler->ha_rnd_init()` 以外的 PQ scan 初始化；
- InnoDB `pq_leader_scan_init()`；
- worker/Gather/Exchange 创建；
- 任何 row 已流出；
- 任何不可重入的 handler 状态修改。

V2-1 可以直接初始化内部 serial `TableScanIterator`。如果 serial `Init()` 失败，错误按 serial iterator 原逻辑上报。

### State and counters

- EXPLAIN 路径仍不创建 PQ iterator，不增加 `Parallel_queries_fallback`；
- ordinary execution 的 eligible TABLE_SCAN 进入 `ITERATOR_SELECTED`；
- V2-1 安全窗口 fallback 后进入 `FALLBACK_SERIAL`；
- `Parallel_queries_fallback` 在 fallback 初始化时增加一次；
- `Parallel_queries_executed` 仍保持 0。

## Allowed Files

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `Docs/pq_tasks/v2-1-iterator-safe-fallback.md`
- `Docs/pq_tasks/README.md`
- 必要时 `mysql-test/suite/parallel_query/**`

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_stats pq_explain_eligible pq_fullscan_result
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
```

## Acceptance Checklist

- [x] eligible ordinary execution 返回非空 `PQTableScanIterator`；
- [x] `PQTableScanIterator` 内部持有 serial fallback iterator；
- [x] `Init()` 在安全窗口内 fallback；
- [x] `Read()` 委托 serial fallback，结果不变；
- [x] EXPLAIN 不增加 fallback counter；
- [x] 完整 `parallel_query` suite 通过；
- [x] 未启用 worker/InnoDB/Exchange 真实并行路径。

## Completion Report

### Codex 实现结果（2026-06-11）

Changed files:

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `mysql-test/suite/parallel_query/t/pq_iterator_safe_fallback.test`
- `mysql-test/suite/parallel_query/r/pq_iterator_safe_fallback.result`
- `Docs/pq_tasks/v2-1-iterator-safe-fallback.md`
- `Docs/pq_tasks/README.md`

实现说明:

1. `TryCreatePQTableScanIterator()` 对 ordinary eligible execution 返回非空 `PQTableScanIterator`。
2. EXPLAIN 路径仍返回 `nullptr`，保持 candidate-only，不增加 fallback counter。
3. `PQTableScanIterator` 内部持有 `unique_ptr_destroy_only<RowIterator> m_serial_iterator`，由同一个 `MEM_ROOT` 上的 `NewIterator<TableScanIterator>()` 创建。
4. `Init()` 先进入 `ITERATOR_SELECTED`，在未调用 InnoDB PQ API、未创建 worker/Gather/Exchange、未产生 row stream 前，切到 owned serial fallback。
5. fallback counter 从 factory 迁移到 iterator `Init()`，并通过 `m_fallback_counted` 保证同一个 query iterator 只计一次。
6. `Read()`、`UnlockRow()`、`SetNullRowFlag()`、`StartPSIBatchMode()`、`EndPSIBatchModeIfStarted()` 均委托给内部 serial iterator。
7. 新增 `pq_iterator_safe_fallback` MTR，验证 EXPLAIN 不计数、3 个 eligible real execution 计入 fallback、结果保持串行一致、executed/workers/rows 仍为 0。

并行 review 结论:

- Critical: none
- Important: none
- Minor: 新增任务书和 MTR 文件需显式纳入 commit；当前提交会包含这些文件。

验证结果:

```bash
cmake --build build-ninja --target mysqld -j 16
# PASS

cd build-ninja/mysql-test
./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root pq_iterator_safe_fallback pq_stats pq_explain_eligible pq_fullscan_result
# PASS: 4 个目标测试 + shutdown_report

./mtr --suite=parallel_query --parallel=1 --extern socket=/private/tmp/pq20.sock --extern user=root
# PASS: 13 个实际 parallel_query 测试 + shutdown_report，共 14 项
```

风险点:

- V2-1 只证明 iterator ownership 与 safe fallback，不证明真实并行执行。
- `m_fallback_counted` 防止同一个 iterator 多次 `Init()` 重复计 query fallback；当前 eligibility 仍限制为 single-table，因此没有 nested-loop counter 回归。
- 真正 worker 启动后不能再静默 serial fallback，该规则留给 V2-2/V2-5 继续落实。

## Current Status

- Status: Code complete, validated, pending commit
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11
