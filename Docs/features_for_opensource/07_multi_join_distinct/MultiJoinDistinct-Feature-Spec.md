# Multi-Join Distinct Optimization -- Feature Specification

## 1. Overview

### Problem Statement

Customers encountered significant performance regression for `LEFT JOIN + DISTINCT` and `INNER JOIN + DISTINCT` scenarios when upgrading from MySQL 5.7 to MySQL 8.0.22+. The regression was introduced by the Volcano iterator executor refactoring in MySQL 8.0.16 (WL#12074, commit a56102e7). In MySQL 5.7, the DISTINCT implementation could prune scans for **all** `not_used_in_distinct` tables. After the Volcano refactor, the executor only stops scanning for the **last** `not_used_in_distinct` table when a new record is found (`JOIN::found_records` is updated). This means inner tables in a multi-join with DISTINCT are fully scanned even when their data does not contribute to the DISTINCT result, leading to orders-of-magnitude performance degradation.

Related upstream bugs:
- https://bugs.mysql.com/bug.php?id=109155
- https://bugs.mysql.com/bug.php?id=100536

### Solution

Port the MySQL 5.7 DISTINCT optimization into the Volcano iterator executor. When all inner tables of a nested loop join are marked `not_used_in_distinct`, the `NestedLoopIterator` stops scanning inner tables as soon as a new record is found (i.e., `found_records` increases). This avoids unnecessary row reads for inner tables whose values are irrelevant to the DISTINCT result.

A new optimizer switch flag `nlj_distinct_optimize` (default ON) allows enabling/disabling this optimization at the session or global level.

### Execution Model

```
Before optimization (MySQL 8.0 default):
  For each outer row:
    For each inner row:          // scans ALL inner rows
      if match: output row

After optimization:
  For each outer row:
    For each inner row:          // stops after FIRST match
      if match: output row, break inner loop
```

The optimization is visible in `EXPLAIN FORMAT=TREE` as "Nested loop inner join with distinct optimization" or "Nested loop left join with distinct optimization".

### Code Volume

| Metric | Value |
|--------|-------|
| Files changed | 23 |
| Lines added | +3,719 |
| Lines removed | -27 |
| Net change | +3,692 |
| New test files | 5 |
| Source code files modified | 8 |

---

## 2. New Files

| File | Description |
|------|-------------|
| `mysql-test/include/distinct_optimize_check.inc` | Reusable include file that compares Handler_read statistics with and without the optimization to verify correctness |
| `mysql-test/t/join_distinct.test` | Main MTR test suite (430 lines) covering session/global variable, EXPLAIN output, correctness, and performance comparison |
| `mysql-test/r/join_distinct.result` | Expected results for main test (1,316 lines) |
| `mysql-test/suite/dstore_main/include/distinct_optimize_check.inc` | DStore variant of the include file |
| `mysql-test/suite/dstore_main/t/join_distinct-dstore.test` | DStore variant test (436 lines) |
| `mysql-test/suite/dstore_main/r/join_distinct-dstore.result` | DStore variant expected results (1,331 lines) |

---

## 3. Modifications to Existing Files

| File | Change Description |
|------|--------------------|
| `sql/sql_executor.cc` | Added `CheckNljDistinctOptimze()` function to determine if distinct optimization applies; integrated into `CreateNestedLoopAccessPath()`; added `not_used_in_distinct` marking in `JOIN::optimize_distinct()` |
| `sql/iterators/composite_iterators.h` | Added `m_inner_not_used_in_distinct`, `m_found_records`, and `m_join` members to `NestedLoopIterator`; updated constructor to accept `distinct_optimize` parameter |
| `sql/iterators/composite_iterators.cc` | Modified `NestedLoopIterator::Read()` to check `found_records` and skip inner rows when optimization is active; modified `MaterializeIterator` to increment `found_records` |
| `sql/join_optimizer/access_path.h` | Added `distinct_optimize` field to the `NestedLoopJoin` access path struct |
| `sql/join_optimizer/access_path.cc` | Pass `distinct_optimize` flag when constructing `NestedLoopIterator` from access path |
| `sql/join_optimizer/explain_access_path.cc` | Added "with distinct optimization" label in EXPLAIN FORMAT=TREE output when `distinct_optimize` is true |
| `sql/sys_vars.cc` | Added `nlj_distinct_optimize` flag to the `optimizer_switch` system variable |
| `sql/sql_const.h` | Added `OPTIMIZER_SWITCH_NLJ_DISTINCT_OPTIMIZE` constant (`1ULL << 29`) |
| Various `.result` files | Updated to reflect new EXPLAIN output (e.g., "Nested loop inner join with distinct optimization") |

---

## 4. Core Data Structures

### 4.1 AccessPath::NestedLoopJoin::distinct_optimize

```cpp
// In sql/join_optimizer/access_path.h
struct NestedLoopJoin {
    AccessPath *outer;
    AccessPath *inner;
    JoinType join_type;
    bool pfs_batch_mode;
    // ... other fields ...
    bool distinct_optimize;  // NEW: true when all inner tables are not_used_in_distinct
};
```

Set by `CreateNestedLoopAccessPath()` when `CheckNljDistinctOptimze()` returns true and the `nlj_distinct_optimize` optimizer switch is on.

### 4.2 NestedLoopIterator members

```cpp
// In sql/iterators/composite_iterators.h
class NestedLoopIterator : public RowIterator {
    // NEW members:
    const JOIN *m_join = nullptr;               // Access to found_records
    bool m_inner_not_used_in_distinct = false;  // Optimization active flag
    ha_rows m_found_records = 0;               // Snapshot per outer-row iteration
};
```

- `m_join`: Pointer to the JOIN object, needed to read `found_records`.
- `m_inner_not_used_in_distinct`: Copied from `distinct_optimize` access path flag.
- `m_found_records`: Captured at the start of each outer-row iteration; compared with `m_join->found_records` during inner row reads.

### 4.3 JOIN::found_records

```cpp
// In sql/sql_select.h
class JOIN {
    ha_rows found_records;  // Incremented by MaterializeIterator when a new distinct row is inserted
};
```

This field already existed but was not updated by the iterator executor. The feature adds `m_join->found_records++` in `MaterializeIterator` when a new (non-duplicate) row is written.

### 4.4 QEP_TAB::not_used_in_distinct

Pre-existing boolean field set during `JOIN::optimize_distinct()`. Marks tables whose columns do not appear in the select list of a DISTINCT query. Starting from the last table in the join order and moving backwards, mark each table as `not_used_in_distinct = true` until a table whose columns ARE used in the select list is encountered.

---

## 5. Execution Flow

### 5.1 Optimization Phase

```
JOIN::optimize()
  -> JOIN::optimize_distinct()
       For each table from the end of join order:
         If table's columns not in select_list:
           tab->not_used_in_distinct = true
         Else:
           break

  -> create_access_paths()
       -> ConnectJoins()
            For each join:
              -> CreateNestedLoopAccessPath()
                   Check optimizer switch
                   -> CheckNljDistinctOptimze()
                        Verify join_type is INNER or OUTER
                        Verify ALL inner tables have not_used_in_distinct = true
                   Set path->nested_loop_join().distinct_optimize

            For last primary table:
              If not_used_in_distinct && no pending conditions && no hash join/BKA:
                Wrap with LimitOffsetAccessPath(limit=1)
```

### 5.2 Execution Phase

```
NestedLoopIterator::Read()

State: READING_FIRST_INNER_ROW (after a new outer row is read)
  m_found_records = m_join->found_records    // snapshot current count

  // Check: if found_records increased since snapshot, a match was found
  if (m_inner_not_used_in_distinct &&
      (m_join->found_records > m_found_records)) {
    m_state = NEEDS_OUTER_ROW               // skip remaining inner rows
    m_source_inner->EndPSIBatchModeIfStarted()
    continue                                 // go to next outer row
  }

  Read next inner row...
```

### 5.3 Interaction with found_records

The `found_records` counter in JOIN is incremented in `MaterializeIterator::MaterializeQueryBlock()` when a row is written to the deduplication temp table (i.e., a new distinct row). The optimization works by comparing the current `found_records` value to the snapshot taken at the start of the inner loop. If `found_records` has increased, a new matching row has been produced, so remaining inner rows can be skipped.

### 5.4 CheckNljDistinctOptimze Logic

1. If join_type is not INNER or OUTER, return false (no optimization for ANTI/SEMI joins).
2. Iterate over all inner tables of the nested loop join.
3. If any inner table has `not_used_in_distinct == false`, return false.
4. If all inner tables are `not_used_in_distinct`, return true.
5. In debug builds, additionally verify that all subsequent tables in the join slice are also `not_used_in_distinct`.

---

## 6. System Variables

### 6.1 optimizer_switch flag: nlj_distinct_optimize

| Property | Value |
|----------|-------|
| **Name** | `nlj_distinct_optimize` |
| **Type** | Boolean flag within `optimizer_switch` |
| **Default** | ON |
| **Scope** | SESSION, GLOBAL |
| **Hint updateable** | Yes, via `SET_VAR(optimizer_switch="nlj_distinct_optimize=ON/OFF")` |
| **Description** | Enable/disable the NLJ DISTINCT optimization. When enabled, nested loop joins stop scanning inner tables once a new distinct record is found. |

**Usage examples:**
```sql
-- Session level
SET SESSION optimizer_switch="nlj_distinct_optimize=ON";
SET SESSION optimizer_switch="nlj_distinct_optimize=OFF";

-- Global level
SET GLOBAL optimizer_switch="nlj_distinct_optimize=ON";

-- Query hint
SELECT /*+ SET_VAR(optimizer_switch="nlj_distinct_optimize=ON") */ DISTINCT a FROM t1 JOIN t2;
```

---

## 7. EXPLAIN Output

### EXPLAIN FORMAT=TREE

When the optimization is active, the EXPLAIN output changes:

```
-- Without optimization:
-> Nested loop inner join  (cost=2.05 rows=2)

-- With optimization:
-> Nested loop inner join with distinct optimization  (cost=2.05 rows=2)
```

Similarly for left joins:
```
-> Nested loop left join with distinct optimization  (cost=2.8 rows=3)
```

### EXPLAIN FORMAT=JSON

The JSON output includes:
```json
{
  "join_algorithm": "nested_loop",
  "description": "Nested loop inner join with distinct optimization"
}
```

### Traditional EXPLAIN

No visible change. The traditional EXPLAIN format does not show the optimization.

---

## 8. Error Handling

- No specific error codes are introduced by this feature.
- The optimization is purely a performance improvement; disabling it (via optimizer switch) falls back to the original MySQL 8.0 behavior of scanning all inner rows.
- The `NestedLoopIterator` assert guards ensure that ANTI/SEMI joins never have the optimization active (these join types stop after finding one matching row anyway).
- Debug assertions verify consistency: if `CheckNljDistinctOptimze()` returns true, all subsequent tables in the join slice must also be `not_used_in_distinct`.

---

## 9. Test Coverage

### Test Files

| Test File | Lines | Coverage |
|-----------|-------|----------|
| `mysql-test/t/join_distinct.test` | 430 | Main InnoDB test suite |
| `mysql-test/suite/dstore_main/t/join_distinct-dstore.test` | 436 | DStore engine test suite |
| `mysql-test/include/distinct_optimize_check.inc` | 32 | Reusable verification macro |

### Test Categories

1. **Variable verification**: Session and global `optimizer_switch` manipulation of `nlj_distinct_optimize`, including `SET_VAR` hints.
2. **EXPLAIN output**: Verification of "with distinct optimization" label in `FORMAT=TREE` and `FORMAT=JSON`.
3. **EXPLAIN ANALYZE**: Compares actual row counts with and without optimization.
4. **Correctness**: Query results are identical with and without optimization.
5. **Performance comparison**: Uses Handler_read statistics to verify that fewer rows are read with the optimization enabled.
6. **Join types covered**:
   - Nested loop inner join + DISTINCT
   - Nested loop left join + DISTINCT
   - Multi-table joins (3+ tables)
   - LEFT JOIN chains
7. **Edge cases**:
   - Single-table DISTINCT (no optimization applicable)
   - Tables with all columns in select list (no `not_used_in_distinct`)
   - Mixed scenarios where some inner tables are used in DISTINCT

### Test Methodology

The `distinct_optimize_check.inc` macro:
1. Runs query with `nlj_distinct_optimize=ON` and captures Handler_read total.
2. Runs same query with `nlj_distinct_optimize=OFF` and captures Handler_read total.
3. Asserts that the optimized read count is less than the unoptimized read count.

---

## 10. Limitations

1. **Join type restriction**: The optimization only applies to INNER and OUTER (LEFT) joins. ANTI and SEMI joins are excluded because they already stop after finding one matching row.

2. **Single-table queries not affected**: If there is only one table in the query, there are no inner tables to optimize.

3. **Requires all inner tables to be not_used_in_distinct**: If any inner table's columns appear in the DISTINCT select list, the optimization is not applied for that join level.

4. **Hash join incompatibility**: The optimization is specific to nested loop joins. Hash joins do not use the `NestedLoopIterator` and thus cannot benefit.

5. **BKA incompatibility**: Batched Key Access joins buffer rows and are incompatible with the early-stop optimization.

6. **Only works with iterator executor**: The optimization relies on the Volcano iterator executor's `NestedLoopIterator` and `MaterializeIterator`. The older executor is not affected.

7. **Dependent on MaterializeIterator updating found_records**: The optimization requires that `found_records` is incremented when a new row is successfully inserted into the dedup temp table. If the materialization path changes, this must be preserved.

8. **LimitOffsetAccessPath for last table only**: The `LimitOffsetAccessPath(limit=1)` wrapping for the last primary table is only added when there are no pending conditions, no hash join replacement, and no BKA.

---

## 11. Implementation Order

1. **System variable** - Add `OPTIMIZER_SWITCH_NLJ_DISTINCT_OPTIMIZE` constant to `sql/sql_const.h`; add `nlj_distinct_optimize` flag to the `optimizer_switch` names array and default value in `sql/sys_vars.cc`.

2. **Access path modification** - Add `distinct_optimize` field to `AccessPath::nested_loop_join` struct in `sql/join_optimizer/access_path.h`.

3. **Eligibility check** - Implement `CheckNljDistinctOptimze()` static function in `sql/sql_executor.cc`.

4. **Access path creation** - Modify `CreateNestedLoopAccessPath()` to accept `inner_tables` and `join` parameters, check `CheckNljDistinctOptimze()`, and set `distinct_optimize`; update both call sites in `ConnectJoins()`.

5. **Limit/offset path for last table** - Add `LimitOffsetAccessPath(limit=1)` wrapping for the last primary table when `not_used_in_distinct` is true and conditions are met.

6. **Iterator modification** - Add `m_join`, `m_inner_not_used_in_distinct`, `m_found_records` to `NestedLoopIterator` in `sql/iterators/composite_iterators.h`; update constructor and modify `Read()` method in `sql/iterators/composite_iterators.cc`.

7. **found_records tracking** - Add `m_join->found_records++` in `MaterializeIterator::MaterializeQueryBlock()` in `sql/iterators/composite_iterators.cc`.

8. **Iterator creation** - Pass new arguments (`join`, `distinct_optimize`) from `CreateIteratorFromAccessPath()` in `sql/join_optimizer/access_path.cc`.

9. **EXPLAIN output** - Update `sql/join_optimizer/explain_access_path.cc` to show "with distinct optimization" label.

10. **Test suite** - Port MTR test cases and update affected `.result` files.
