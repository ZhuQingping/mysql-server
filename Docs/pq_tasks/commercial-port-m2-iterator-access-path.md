# M2 Commercial Iterator Access Path Skeleton Taskbook

## 状态

Completed。

本任务将商用 PQ iterator 和 access path 名称/参数边界迁入当前分支，但保持默认不可达，不替换当前 `PQTableScanIterator`。

## 目标

建立商用主架构的执行器承载结构：

- `ParallelScanIterator`
- `PQblockScanIterator`
- `PQRefIterator`
- `AccessPath::PARALLEL_SCAN`
- `AccessPath::PQ_BLOCK_SCAN`
- `AccessPath::PQ_REF_SCAN`

M2 只做 compile-only skeleton。真正 worker plan clone、worker result path、handler/InnoDB 商用扫描和 ref/ICP 支持后置。

## 允许修改

- Create: `sql/parallel_query/pq_iterators.h`
- Create: `sql/parallel_query/pq_iterators.cc`
- Modify: `sql/CMakeLists.txt`
- Modify: `sql/join_optimizer/access_path.h`
- Modify: `sql/join_optimizer/access_path.cc`
- Modify: `sql/join_optimizer/explain_access_path.cc`
- Modify: `sql/join_optimizer/join_optimizer.cc`
- Modify: `sql/join_optimizer/walk_access_paths.h`
- Modify: `Docs/pq_tasks/commercial-port-m2-iterator-access-path.md`
- Modify: `Docs/pq_tasks/commercial-port-gap-analysis.md`
- Modify: `Docs/pq_tasks/README.md`

## 禁止修改

- `storage/innobase/**`
- `sql/handler.h`
- `sql/sql_class.h`
- `sql/sql_lex.h`
- `sql/sql_optimizer.*`
- `sql/parallel_query/pq_iterator.*`
- `sql/parallel_query/sql_parallel.*`
- `sql/parallel_query/query_result_mq.*`
- `mysql-test/suite/parallel_query/t/*`
- `mysql-test/suite/parallel_query/r/*`

## 设计要求

- 新 access path 类型必须默认不可达；
- 不改变 `TABLE_SCAN` 当前 `TryCreatePQTableScanIterator()` 逻辑；
- `ParallelScanIterator::Init()` fail-closed，不能启动 worker；
- `PQblockScanIterator::Init()` fail-closed，不能调用 handler/InnoDB PQ API；
- `PQRefIterator::Init()` fail-closed，不能打开 ref/ICP 路径；
- EXPLAIN 只允许描述 candidate/skeleton，不声称 executed；
- 如果编译要求扩展 `THD/JOIN/QEP_TAB/handler/InnoDB`，停止并记录，不扩大 M2。

## 验证

```bash
cmake --build build-ninja --target mysqld -j 16
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m2_vars --tmpdir=/tmp/pqt_m2_vars
```

## Completion Report

M2 source migration completed as a compile-only iterator/access path skeleton.

### Changed files

- Added `sql/parallel_query/pq_iterators.h`
- Added `sql/parallel_query/pq_iterators.cc`
- Updated `sql/CMakeLists.txt`
- Updated `sql/join_optimizer/access_path.h`
- Updated `sql/join_optimizer/access_path.cc`
- Updated `sql/join_optimizer/explain_access_path.cc`
- Updated `sql/join_optimizer/join_optimizer.cc`
- Updated `sql/join_optimizer/walk_access_paths.h`

### Implementation notes

- Added commercial iterator class shells:
  - `ParallelScanIterator`
  - `PQblockScanIterator`
  - `PQRefIterator`
- Added commercial access path type shells:
  - `AccessPath::PARALLEL_SCAN`
  - `AccessPath::PQ_BLOCK_SCAN`
  - `AccessPath::PQ_REF_SCAN`
- Added accessors and factories:
  - `NewParallelScanAccessPath()`
  - `NewPQblockScanAccessPath()`
  - `NewPQrefScanAccessPath()`
- Kept current `TABLE_SCAN` hook and `TryCreatePQTableScanIterator()` unchanged.
- `Init()` for all new commercial iterators returns failure. If a new path is accidentally made reachable before M3-M6, it fails closed instead of silently falling back or pretending to execute PQ.
- EXPLAIN labels these paths as `skeleton_not_executed`.

### Deferred dependencies

- M3: real plan rewrite and worker plan clone.
- M4: commercial `Query_result_mq` and MQ worker-result protocol.
- M5/M6: handler/InnoDB commercial scan/ref execution, including secondary index and ICP.

### Commands run

```bash
cmake --build build-ninja --target mysqld -j 16
```

Result:

```text
passed
```

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query pq_vars --parallel=1 --vardir=/tmp/pqv_m2_vars --tmpdir=/tmp/pqt_m2_vars
```

Result:

```text
passed
```

```bash
cd build-ninja/mysql-test
TMPDIR=/tmp ./mtr --suite=parallel_query --parallel=1 --vardir=/tmp/pqv_m2_full --tmpdir=/tmp/pqt_m2_full
```

Result:

```text
passed, 69 tests successful
```

### Next suggested task

Proceed to M3 Commercial Plan Clone And Resolver Activation. M3 should remain fail-before-worker-start: clone may be attempted only under explicit protection, and clone failure must fall back before any worker or commercial iterator starts.
