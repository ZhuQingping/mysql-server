# V2-8I Iterator Runtime State Contract 任务书

## Goal

V2-8I 的目标是在 `PQTableScanIterator` 内部显式记录真实 row stream 的
不可回退边界，为后续打开 `Read()` 前建立代码级 contract：

- `SAFE_FALLBACK`：还没有启动真实 worker，也没有 row stream，可以 serial fallback；
- `PQ_STARTED`：真实 worker 或 row stream 已启动，不能再透明 fallback；
- `PQ_ROW_RETURNED`：至少一条真实 PQ row 已返回给 SQL executor，错误必须作为查询错误处理。

当前阶段只增加状态合同和断言，不打开真实执行。

## Scope

允许修改：

- `sql/parallel_query/pq_iterator.h`
- `sql/parallel_query/pq_iterator.cc`
- `Docs/pq_tasks/README.md`
- `Docs/pq_tasks/v2-8i-iterator-runtime-state.md`

禁止修改：

- `PQTableScanIterator::Read()` 真实 row path；
- worker launch；
- InnoDB worker row read；
- `PQ_execution_state::EXECUTED`；
- 真实执行计数。

## Design

新增 `PQTableScanIterator::Runtime_state`：

```c++
enum class Runtime_state {
  SAFE_FALLBACK,
  PQ_STARTED,
  PQ_ROW_RETURNED,
};
```

当前 `Init()` 在创建 serial fallback iterator 前断言：

```c++
assert(can_fallback_serial());
```

后续真实 worker/Exchange 接通时，必须在不可回退点调用
`mark_pq_started()`；第一条 row 返回给 executor 后调用
`mark_pq_row_returned()`。

## Validation

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_iterator_safe_fallback --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
```

## Acceptance Checklist

- [x] iterator runtime state enum 已增加；
- [x] serial fallback 前存在 `SAFE_FALLBACK` 断言；
- [x] 当前路径不进入 `PQ_STARTED`；
- [x] 不打开真实 `Read()`；
- [x] build 和 MTR 通过。

## Current Status

- Status: Completed
- Owner: Codex Orchestrator
- Started: 2026-06-11
- Completed: 2026-06-11

## Completion Report

实现内容：

- 增加 `PQTableScanIterator::Runtime_state`；
- 增加 `can_fallback_serial()` / `mark_pq_started()` /
  `mark_pq_row_returned()`；
- 在当前 serial fallback 创建前断言仍处于 `SAFE_FALLBACK`；
- 当前路径不进入 `PQ_STARTED`，不改变执行行为。

验证结果：

```text
cmake --build build-ninja --target mysqld -j 16
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query pq_iterator_safe_fallback --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed

TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv --tmpdir=/tmp/pqt
Result: passed, 19 tests successful
```
