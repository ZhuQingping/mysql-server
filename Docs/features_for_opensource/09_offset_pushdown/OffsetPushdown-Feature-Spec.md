# Offset Pushdown -- Feature Specification

## 1. Overview

### Problem Statement

When a query contains `LIMIT n OFFSET m`, MySQL's SQL layer typically receives all rows from the storage engine and then discards the first `m` rows. For large offset values, this wastes significant I/O and CPU resources reading rows that are never returned to the client. The storage engine (InnoDB) is capable of skipping rows internally during its scan, avoiding the overhead of materializing and transferring them to the SQL layer.

### Solution

Implement an offset pushdown mechanism where the SQL optimizer communicates the OFFSET value to the storage engine. The storage engine then skips the first `m` rows internally during its scan, returning only the rows that the SQL layer actually needs. This eliminates the overhead of transferring and filtering offset rows at the SQL layer.

The feature is controlled by:
- An `optimizer_switch` flag `offset_pushdown` (default: ON)
- Optimizer hints `OFFSET_PUSHDOWN` and `NO_OFFSET_PUSHDOWN`
- A threshold variable `op_over_pq_offset_threshold` to resolve conflicts with Parallel Query

### Execution Model

```
Before offset pushdown:
  SQL layer: LIMIT 10 OFFSET 1000
  InnoDB: reads rows 1..1010, returns all to SQL layer
  SQL layer: discards rows 1..1000, returns rows 1001..1010

After offset pushdown:
  SQL layer: LIMIT 10 OFFSET 0 (adjusted)
  InnoDB: skips rows 1..1000 internally, returns rows 1001..1010
  SQL layer: returns rows 1001..1010 directly
```

### Code Volume

| Metric | Value |
|--------|-------|
| Files changed (all commits) | 67 + 27 + 7 + 3 = 104 total |
| Lines added | +6,898 (commit 1) + 3,336 (commit 2) + 109 (commit 3) + 25 (commit 4) = +10,368 |
| Lines removed | -151 (commit 1) - 35 (commit 2) - 1 (commit 3) - 1 (commit 4) = -188 |
| Net change | +10,180 |
| Core SQL source files | ~15 |
| InnoDB source files | 6 |
| Hint system files | 6 |
| DStore source files | 4 |
| Test files | ~40 |

---

## 2. New Files

| File | Description |
|------|-------------|
| `mysql-test/t/offset_pushdown.test` | Main MTR test suite (653 lines) for InnoDB offset pushdown |
| `mysql-test/r/offset_pushdown.result` | Expected results |
| `mysql-test/t/offset_pushdown_limitations.test` | Limitations test (123 lines) |
| `mysql-test/r/offset_pushdown_limitations.result` | Expected results |
| `mysql-test/t/offset_pushdown_locking_test.test` | Locking read tests |
| `mysql-test/r/offset_pushdown_locking_test.result` | Expected results |
| `mysql-test/t/offset_pushdown_insert_delete_update.test` | DML operations test (618 lines) |
| `mysql-test/r/offset_pushdown_insert_delete_update.result` | Expected results |
| `mysql-test/suite/dstore_main/t/offset_pushdown-dstore.test` | DStore variant (626 lines) |
| `mysql-test/suite/dstore_main/r/offset_pushdown-dstore.result` | DStore expected results |
| `mysql-test/suite/dstore_main/t/offset_pushdown_insert_delete_update-dstore.test` | DStore DML test (561 lines) |
| `mysql-test/suite/dstore_main/r/offset_pushdown_insert_delete_update-dstore.result` | DStore DML expected results |
| `mysql-test/suite/dstore_main/t/offset_pushdown_limitations-dstore.test` | DStore limitations test (81 lines) |
| `mysql-test/suite/dstore_main/r/offset_pushdown_limitations-dstore.result` | DStore limitations expected results |
| `mysql-test/suite/dstore_main/t/offset_pushdown_locking_test-dstore.test` | DStore locking test (129 lines) |
| `mysql-test/suite/dstore_main/r/offset_pushdown_locking_test-dstore.result` | DStore locking expected results |

---

## 3. Modifications to Existing Files

### SQL Layer

| File | Change Description |
|------|--------------------|
| `sql/sql_select.cc` | Added `JOIN::offset_pushdown()` function with eligibility checks, parallel query conflict resolution, and offset/limit adjustment |
| `sql/sql_optimizer.cc` | Added call to `offset_pushdown()` before `create_access_paths()`; hint/optimizer_switch precedence logic |
| `sql/sql_optimizer.h` | Declared `void offset_pushdown()` method in JOIN class |
| `sql/sql_select.h` | Declared standalone `void offset_pushdown(JOIN *join)` function |
| `sql/sql_const.h` | Added `OPTIMIZER_SWITCH_OFFSET_PUSHDOWN` constant (`1ULL << 28`) |
| `sql/sys_vars.cc` | Added `offset_pushdown` flag to `optimizer_switch`; added `op_over_pq_offset_threshold` session variable |
| `sql/handler.h` | Added `set_pushed_offset()` and `get_pushed_offset()` virtual methods to handler base class |
| `sql/lex.h` | Added `OFFSET_PUSHDOWN` and `NO_OFFSET_PUSHDOWN` hint tokens |
| `sql/opt_hints.h` | Added `OFFSET_PUSHDOWN_HINT_ENUM` and `offset_pushdown_hint` member to `Opt_hints_qb` |
| `sql/opt_hints.cc` | Added hint info entry for `OFFSET_PUSHDOWN`; implemented `apply_offset_pushdown_hint()` |
| `sql/parse_tree_hints.cc` | Added hint parsing for `OFFSET_PUSHDOWN_HINT_ENUM` |
| `sql/sql_lex_hints.cc` | Added lexer cases for `NO_OFFSET_PUSHDOWN_HINT` and `OFFSET_PUSHDOWN_HINT` |
| `sql/opt_explain_format.h` | Added `ET_USING_OFFSET_PUSHDOWN` enum value |
| `sql/opt_explain_traditional.cc` | Added "Using offset pushdown" string for traditional EXPLAIN |
| `sql/opt_explain_json.cc` | Added "offset pushdown" string for JSON EXPLAIN |
| `sql/opt_explain.cc` | Added `push_extra(ET_USING_OFFSET_PUSHDOWN)` logic |
| `sql/parallel_query/pq_optimizer.h` | Added `OFFSET_PUSHDOWN_PRIO` to `PQUnsuiteInfo` enum |
| `sql/parallel_query/pq_optimizer.cc` | Added description for `OFFSET_PUSHDOWN_PRIO` |

### InnoDB Storage Engine

| File | Change Description |
|------|--------------------|
| `storage/innobase/handler/ha_innodb.h` | Implemented `set_pushed_offset()`, `get_pushed_offset()`, `increment_scanned_offset()`, `get_scanned_offset()`; added `m_pushed_offset` and `m_scanned_offset` members |
| `storage/innobase/handler/ha_innodb.cc` | Added offset state reset in `rnd_init()`, `index_init()`, `reset()`; added `m_pushed_offset`/`m_scanned_offset` copy in `clone()`; added status variable `rds_rows_skipped_by_offset_pushdown` |
| `storage/innobase/row/row0sel.cc` | Implemented `skip_row_for_offset_pushdown()` inline function; added row skip logic in three locations within `row_search_mvcc()` |
| `storage/innobase/include/srv0srv.h` | Added `n_rds_rows_skipped_by_offset_pushdown` counter and `innodb_rds_rows_skipped_by_offset_pushdown` status variable |
| `storage/innobase/srv/srv0srv.cc` | Implemented counter snapshots and export for the status variable |
| `storage/innobase/srv/srv0mon.cc` | Added monitor counter `dml_reads_skipped_by_offset_pushdown` |

### DStore Storage Engine

| File | Change Description |
|------|--------------------|
| `storage/dstore/handler/ha_cde.h` | Implemented `set_pushed_offset()`, `get_pushed_offset()`, `increment_scanned_offset()`, `get_scanned_offset()`, `skip_row_for_offset_pushdown()`; added `m_pushed_offset` and `m_scanned_offset` members |
| `storage/dstore/handler/ha_cde.cc` | Added offset state reset in `rnd_init()`, `index_init()`, `reset()`; added state copy in `clone()`; added skip logic in `rnd_next()` and index scan methods; added temporary table DestroyTuple fix |
| `storage/dstore/dml/cde_dml.cc` | Added offset skip logic in DStore DML scan operations |
| `storage/dstore/dml/cde_dml.h` | Updated DML scan function signatures |

---

## 4. Core Data Structures

### 4.1 Offset Pushdown State in Storage Engine Handler

```cpp
// In ha_innobase / ha_cde:
ha_rows m_pushed_offset{0};   // Set by SQL layer via set_pushed_offset()
                              // Non-zero means offset pushdown is active
ha_rows m_scanned_offset{0};  // Counter incremented by storage engine for each skipped row
                              // When scanned_offset == pushed_offset, skipping stops
```

**Lifecycle**:
- `set_pushed_offset(value)`: Called by `JOIN::offset_pushdown()` during optimization
- `m_scanned_offset = 0`: Reset in `rnd_init()`, `index_init()`, and `reset()`
- `increment_scanned_offset()`: Called by storage engine for each skipped row
- `reset()`: Sets both `m_pushed_offset` and `m_scanned_offset` to 0
- `clone()`: Copies both values to the new handler

### 4.2 Offset/Limit Adjustment in Query Expression

When offset pushdown is activated:

```cpp
query_expression()->offset_limit_cnt = 0;                  // SQL layer no longer skips
query_expression()->select_limit_cnt -= offset_value;      // Adjust limit accordingly
```

The offset is subtracted from the limit because the storage engine now handles skipping. The SQL layer sees `LIMIT (original_limit - offset) OFFSET 0`.

### 4.3 Handler Virtual Interface

```cpp
// In sql/handler.h
virtual void set_pushed_offset(ha_rows pushed_offset) {
  assert(false);
}  // not implemented by default

virtual ha_rows get_pushed_offset() const { return 0; }
```

Only InnoDB (`ha_innobase`) and DStore (`ha_cde`) override these methods. Other engines will hit the assertion if offset pushdown is attempted.

### 4.4 skip_row_for_offset_pushdown (InnoDB)

```cpp
// In storage/innobase/row/row0sel.cc
inline bool skip_row_for_offset_pushdown(row_prebuilt_t *prebuilt) {
  return (prebuilt->m_mysql_handler != nullptr &&
          prebuilt->m_mysql_handler->get_pushed_offset() >
              prebuilt->m_mysql_handler->get_scanned_offset());
}
```

Returns true if offset pushdown is active and not all rows have been skipped yet. When `scanned_offset` reaches `pushed_offset`, returns false and the row is returned to the SQL layer normally.

### 4.5 Parallel Query Conflict Resolution

```cpp
// In sql/parallel_query/pq_optimizer.h
OFFSET_PUSHDOWN_PRIO,  // Offset pushdown takes priority over PQ
```

When both parallel query and offset pushdown are candidates, the `op_over_pq_offset_threshold` variable (default 1000) determines which wins:
- If offset < threshold: parallel query wins (offset pushdown disabled)
- If offset >= threshold: offset pushdown wins (parallel query disabled)

---

## 5. Execution Flow

### 5.1 Optimization Phase

```
JOIN::optimize()
  -> Check for OFFSET_PUSHDOWN hint or optimizer_switch flag
     Hint takes precedence over optimizer_switch:
     - If hint is active: use hint value (ON or OFF)
     - If no hint: use optimizer_switch flag
  -> JOIN::offset_pushdown()
       1. Check offset_value != 0
       2. Check exactly one non-const primary table
       3. Check all eligibility conditions (see Section 5.2)
       4. Resolve conflict with parallel query
       5. Call tbl->file->set_pushed_offset(offset_value)
       6. Adjust offset_limit_cnt to 0
       7. Adjust select_limit_cnt -= offset_value
  -> create_access_paths()  (uses adjusted offset/limit values)
```

### 5.2 Eligibility Conditions

Offset pushdown is only activated when ALL of the following conditions are met:

| Condition | Rationale |
|-----------|-----------|
| `offset_value != 0` | No offset means nothing to push down |
| `(primary_tables - const_tables) == 1` | Only single-table queries; multi-join is too complex |
| `!select_distinct` | DISTINCT requires seeing all rows for dedup |
| `!grouped` | GROUP BY requires all rows for aggregation |
| `!group_optimized_away` | Even if optimized away, grouping semantics prevent pushdown |
| `!implicit_grouping` | Implicit GROUP BY (aggregate without GROUP BY) prevents pushdown |
| Range scan does NOT use DS-MRR | MRR sorts rows, changing order; offset pushdown expects sequential order |
| `!(tbl->part_info && num_partitions != 1)` | Multi-partition scans are not supported |
| Table is not a view, derived, table function, or schema table | Only base tables |
| `effective_index() != MAX_KEY \|\| type() == JT_ALL` | Must have a valid access path |
| Engine is InnoDB or DStore | Only these engines implement `set_pushed_offset()` |
| Table is not a non-transactional temp table | `NO_TMP_TABLE` or `TRANSACTIONAL_TMP_TABLE` only |
| `!having_cond` | HAVING requires all rows |
| `!agg_func_used()` | Aggregate functions require all rows |
| `!group_list.first` | No GROUP BY |
| `olap == UNSPECIFIED_OLAP_TYPE` | No ROLLUP |
| `!(m_windows.elements > 0)` | No window functions |
| `!tab->condition()` | No SQL-layer filter (or filter is already removed by redundant condition removal) |
| `!tab->filesort` | No filesort needed |
| `!need_tmp_before_win` | No temporary table before window functions |
| `!calc_found_rows` | SQL_CALC_FOUND_ROWS requires counting all rows |
| `select_limit_cnt != HA_POS_ERROR` | There must be an explicit LIMIT |
| Query is simple or query_block != global_parameters | UNION offset handling must not conflict |

### 5.3 Execution Phase (InnoDB)

```
row_search_mvcc()
  For each row in the scan:
    1. Read the row from the B+tree
    2. Apply ICP (Index Condition Pushdown) if active
       - If ICP_MATCH and non-locking read:
         if skip_row_for_offset_pushdown():
           increment_scanned_offset()
           goto next_rec
    3. If secondary index, look up clustered record
       - After MVCC visibility check (non-locking read):
         if skip_row_for_offset_pushdown():
           increment_scanned_offset()
           goto next_rec
    4. If locking read:
       - Set the lock first (preserving locking semantics)
       - Then check end-of-range before skipping
       - If skip_row_for_offset_pushdown():
         increment_scanned_offset()
         if semi-consistent read: try_unlock(true)
         goto next_rec
    5. When scanned_offset reaches pushed_offset:
       skip_row_for_offset_pushdown() returns false
       Row is returned to SQL layer normally
```

### 5.4 Execution Phase (DStore)

```
ha_cde::rnd_next() / index scan methods
  For each row in the scan:
    1. Read the row
    2. if skip_row_for_offset_pushdown():
         increment_scanned_offset()
         // For locking reads on non-temporary tables:
         if (select_lock_type != LOCK_NONE && tuple):
           if (!is_temporary()):
             TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple)
           tuple = nullptr
         continue
    3. Return the row to SQL layer
```

### 5.5 Locking Read Semantics

For locking reads (`SELECT ... FOR UPDATE`, `SELECT ... LOCK IN SHARE MODE`), offset pushdown preserves the locking behavior:

1. The row is locked first (same as without offset pushdown)
2. Only then is the row skipped
3. For semi-consistent reads, `try_unlock(true)` is called after skipping (same as `unlock_row()`)
4. End-of-range is checked before skipping to avoid locking rows outside the range when offset exceeds the number of matching rows

---

## 6. System Variables

| Variable | Type | Scope | Default | Hint Updateable | Description |
|----------|------|-------|---------|-----------------|-------------|
| `optimizer_switch=offset_pushdown` | flag | session/global | on | Yes (via SET_VAR) | Enable/disable offset pushdown |
| `op_over_pq_offset_threshold` | ulong | session | 1000 | Yes | Minimum offset value for offset pushdown to take priority over parallel query |

### Hint Syntax

```sql
-- Enable offset pushdown for a query block
SELECT /*+ OFFSET_PUSHDOWN(@qb1) */ * FROM t1 LIMIT 10 OFFSET 1000;

-- Disable offset pushdown for a query block
SELECT /*+ NO_OFFSET_PUSHDOWN(@qb1) */ * FROM t1 LIMIT 10 OFFSET 1000;
```

Hints take precedence over the `optimizer_switch` setting. If no hint is specified, the `optimizer_switch` flag controls the behavior.

---

## 7. EXPLAIN Output

### Traditional EXPLAIN

When offset pushdown is active, the Extra column shows "Using offset pushdown":

```
| id | select_type | table | type  | key | Extra                              |
|----|-------------|-------|-------|-----|------------------------------------|
|  1 | SIMPLE      | t1    | range | b   | Using where; Using offset pushdown |
```

### EXPLAIN FORMAT=TREE

The Limit/Offset node shows the adjusted offset:

```
-> Limit/Offset: 10/0 row(s), with offset pushdown  (cost=X rows=X)
    -> Table scan on t1  (cost=X rows=X)
```

Note: The offset shows as 0 because it has been pushed down to the storage engine. The "with offset pushdown" annotation confirms the feature is active.

### EXPLAIN FORMAT=JSON

The JSON format includes `"offset pushdown"` as an extra annotation.

---

## 8. Error Handling

### 8.1 Locking Read Edge Cases

When the offset value exceeds the number of rows in the range for a locking read:
- The code checks `end_range_check(rec, vrow)` before skipping
- If the current row is the last in the range, returns `DB_RECORD_NOT_FOUND`
- This prevents incorrectly locking the next row beyond the range

### 8.2 NULL Handler Guard

The `skip_row_for_offset_pushdown()` function checks `prebuilt->m_mysql_handler != nullptr` to guard against cases where the handler is not set up.

### 8.3 ICP Interaction

When ICP (Index Condition Pushdown) is active:
- ICP filters rows at the storage engine level
- If ICP returns `ICP_MATCH`, offset pushdown can skip the row (non-locking read only)
- If ICP returns `ICP_OUT_OF_RANGE`, the scan ends (no more rows to skip)
- Only rows that pass ICP are counted toward the offset

### 8.4 MVCC Visibility

When a secondary index lookup requires clustered record access for MVCC visibility:
- The clustered record is fetched first
- If it is not visible (old version), the row is skipped via `goto next_rec` (not counted toward offset)
- Only visible rows count toward the offset skip

### 8.5 MRR Conflict (Bug Fix #3)

When DS-MRR (Disk Sweep Multi-Range Read) is active, rows are read in a different order than the natural index order. Offset pushdown assumes sequential order, so the two features conflict. The fix checks for DS-MRR:

```cpp
!(range_scan_ap &&
  !(range_scan_ap->type == AccessPath::INDEX_RANGE_SCAN &&
    range_scan_ap->index_range_scan().mrr_flags &
        HA_MRR_USE_DEFAULT_IMPL))
```

If the range scan uses DS-MRR (not the default MRR implementation), offset pushdown is disabled. When the default MRR implementation is used (`HA_MRR_USE_DEFAULT_IMPL` flag set), rows maintain their order and offset pushdown can proceed.

### 8.6 Temporary Table Handling (Bug Fix #4)

For DStore temporary tables with locking reads, the `DestroyTuple` call must be skipped because temporary tables use a different memory management scheme:

```cpp
if (m_dstore->select_lock_type != LOCK_NONE && tuple) {
  if (!m_dstore->table_handler->is_temporary()) {
    TupleInterface::DestroyTuple(tuple, CdeFreeMemForDtuple);
  }
  tuple = nullptr;
}
```

Without this fix, `DestroyTuple` crashes when called on a temporary table row during offset skip.

### 8.7 Handler Clone State

When the InnoDB or DStore handler is cloned (for partitioned tables or internal temporary tables), the offset state must be copied to the new handler:

```cpp
new_handler->m_pushed_offset = m_pushed_offset;
new_handler->m_scanned_offset = m_scanned_offset;
```

This was missing in the initial implementation (bug fix #3) and caused incorrect behavior with MRR.

---

## 9. Test Coverage

### Test Files

| Test File | Lines | Description |
|-----------|-------|-------------|
| `mysql-test/t/offset_pushdown.test` | 653 | Core functionality: table scan, index scan, range scan, ref access |
| `mysql-test/t/offset_pushdown_limitations.test` | 123 | Cases where offset pushdown is disabled |
| `mysql-test/t/offset_pushdown_locking_test.test` | ~1660 | Locking reads (FOR UPDATE, LOCK IN SHARE MODE) |
| `mysql-test/t/offset_pushdown_insert_delete_update.test` | 618 | DML operations with offset pushdown |
| `mysql-test/suite/dstore_main/t/offset_pushdown-dstore.test` | 626 | DStore engine variant |
| `mysql-test/suite/dstore_main/t/offset_pushdown_insert_delete_update-dstore.test` | 561 | DStore DML test |
| `mysql-test/suite/dstore_main/t/offset_pushdown_limitations-dstore.test` | 81 | DStore limitations test |
| `mysql-test/suite/dstore_main/t/offset_pushdown_locking_test-dstore.test` | 129 | DStore locking test |

### Test Categories

| Category | Test Cases |
|----------|-----------|
| Table scan | Full table scan with OFFSET |
| Index scan | Index scan with OFFSET |
| Range scan | WHERE condition with range, OFFSET |
| Covering index | ICP interaction, redundant condition removal dependency |
| Non-covering index | Secondary index with cluster lookup |
| ORDER BY | ASC/DESC with OFFSET |
| Multi-column index | Composite key range scans |
| BETWEEN | Range scan with BETWEEN condition |
| Type conversion | String-to-int, decimal comparisons |
| Optimizer switch | `offset_pushdown=ON/OFF` |
| Hints | `OFFSET_PUSHDOWN` / `NO_OFFSET_PUSHDOWN` |
| MRR conflict | DS-MRR with OFFSET (offset pushdown disabled) |
| Parallel query | Conflict resolution, `op_over_pq_offset_threshold` |
| Locking reads | FOR UPDATE, LOCK IN SHARE MODE, semi-consistent |
| Temporary tables | Transactional temp tables, DStore temp tables |
| SQL_CALC_FOUND_ROWS | Disabled with offset pushdown |
| UNION | Simple and UNION queries |
| Subqueries | Derived tables, correlated subqueries |
| Partitioned tables | Single partition only |
| INSERT/DELETE/UPDATE | DML with ORDER BY ... LIMIT OFFSET |
| JSON table | JSON table function with OFFSET |
| CTE | Common Table Expressions with OFFSET |

### Test Methodology

Tests compare results with and without offset pushdown enabled, verifying:
1. Query results are identical
2. EXPLAIN output correctly shows the feature status
3. Row skipping statistics are tracked in `innodb_rds_rows_skipped_by_offset_pushdown`
4. Locking behavior is preserved (rows are still locked even when skipped)

---

## 10. Limitations

1. **Only InnoDB and DStore engines**: Other storage engines do not implement `set_pushed_offset()`.

2. **Only single-table queries**: No joins beyond const tables (`(primary_tables - const_tables) == 1`).

3. **No DISTINCT, GROUP BY, HAVING, aggregates, or window functions**: These require processing all rows.

4. **No SQL_CALC_FOUND_ROWS**: Requires counting all rows.

5. **No SQL-layer filter conditions**: Unless the filter is already removed by `rds_empty_redundant_check_in_range_scan`. This creates a dependency between the two features for covering index scenarios.

6. **No filesort needed**: If a sort is required, the SQL layer must see all rows.

7. **Range scan with DS-MRR not supported**: MRR reorders rows, which conflicts with the sequential row counting assumption.

8. **Multi-partition scans not supported**: Only single-partition queries are eligible.

9. **Non-transactional temporary tables not supported**: Only `NO_TMP_TABLE` or `TRANSACTIONAL_TMP_TABLE`.

10. **Must have an explicit LIMIT**: `select_limit_cnt != HA_POS_ERROR`.

11. **Cannot be used simultaneously with parallel query**: Resolved by `op_over_pq_offset_threshold` (default 1000).

12. **Subqueries inside parallel query workers**: Cannot use offset pushdown because `m_suite_for_pq` is set on the subquery.

13. **row_search_no_mvcc()**: InnoDB's intrinsic temporary table scan function does not have offset pushdown support; only `row_search_mvcc()` does.

14. **UNION conflict**: The query must be simple or the query_block must not be the global_parameters block, to avoid UNION offset handling conflicts.

---

## 11. Implementation Order

1. **Optimizer switch constant** - Add `OPTIMIZER_SWITCH_OFFSET_PUSHDOWN` (`1ULL << 28`) to `sql/sql_const.h`.

2. **System variables** - Add `offset_pushdown` flag to `optimizer_switch` names array and default value in `sql/sys_vars.cc`; add `op_over_pq_offset_threshold` session variable.

3. **Handler interface** - Add `set_pushed_offset()` and `get_pushed_offset()` virtual methods to `sql/handler.h`.

4. **InnoDB handler implementation** - Implement `set_pushed_offset()`, `get_pushed_offset()`, `get_scanned_offset()`, `increment_scanned_offset()` and member variables `m_pushed_offset`, `m_scanned_offset` in `storage/innobase/handler/ha_innodb.h`; add reset logic in `rnd_init()`, `index_init()`, `reset()` in `storage/innobase/handler/ha_innodb.cc`.

5. **Hint system** - Add `OFFSET_PUSHDOWN_HINT_ENUM` to `sql/opt_hints.h`; add hint tokens in `sql/lex.h`; add grammar rules in `sql/sql_hints.yy`; implement hint parsing in `sql/parse_tree_hints.cc`, `sql/sql_lex_hints.cc`, and `sql/opt_hints.cc`.

6. **Optimizer function** - Implement `JOIN::offset_pushdown()` in `sql/sql_select.cc` with all eligibility conditions; add declaration in `sql/sql_optimizer.h` and `sql/sql_select.h`.

7. **Optimizer call site** - Add `offset_pushdown()` call before `create_access_paths()` in `sql/sql_optimizer.cc`, with hint/optimizer_switch precedence logic.

8. **Parallel query conflict** - Add `OFFSET_PUSHDOWN_PRIO` to `PQUnsuiteInfo` enum in `sql/parallel_query/pq_optimizer.h`; add conflict resolution logic in `offset_pushdown()`; add description in `sql/parallel_query/pq_optimizer.cc`.

9. **InnoDB row-level skip** - Implement `skip_row_for_offset_pushdown()` inline function in `storage/innobase/row/row0sel.cc`; add skip logic in three locations within `row_search_mvcc()` (after ICP match, after clustered lookup, after locking read).

10. **Statistics** - Add `n_rds_rows_skipped_by_offset_pushdown` counter in `storage/innobase/include/srv0srv.h`, `storage/innobase/srv/srv0srv.cc`, `storage/innobase/srv/srv0mon.cc`, and `storage/innobase/handler/ha_innodb.cc`.

11. **EXPLAIN output** - Add `ET_USING_OFFSET_PUSHDOWN` to `sql/opt_explain_format.h`; add strings to `sql/opt_explain_traditional.cc` and `sql/opt_explain_json.cc`; add push logic in `sql/opt_explain.cc`.

12. **DStore engine support** - Implement `set_pushed_offset()`, `get_pushed_offset()`, `increment_scanned_offset()`, `get_scanned_offset()`, `skip_row_for_offset_pushdown()` in `storage/dstore/handler/ha_cde.h` and `ha_cde.cc`; add skip logic in `storage/dstore/dml/cde_dml.cc`.

13. **MRR conflict fix** - Add DS-MRR check (`HA_MRR_USE_DEFAULT_IMPL` flag) in `JOIN::offset_pushdown()` eligibility conditions; copy `m_pushed_offset`/`m_scanned_offset` in `ha_innobase::clone()` and `ha_cde::clone()`.

14. **Temporary table fix** - Skip `DestroyTuple` for temporary tables in DStore handler's offset skip logic.

15. **Test suite** - Port all MTR test cases for InnoDB and DStore.
