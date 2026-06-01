# Left Join Elimination (LJE) - Feature Specification

> Version: 3.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main commit: bf20096d415b47664fc4a7685d4f0dd6ceaccfc3
> Author: Guilhem Bichot
> Commits: 6 (1 main + 5 bugfixes)
> Files changed: 66, +7,101 / -120

---

## 1. Overview

### Problem Statement

When a query contains a LEFT JOIN where the right-side (weak-side) table's
columns are not referenced outside the ON clause, the join serves no purpose
for the query result. The optimizer still plans and executes the join, wasting
CPU and I/O resources. This is especially impactful for GROUP BY / DISTINCT
queries where the unreferenced table causes row multiplication (each left-side
row appears N times, N >= 1), forcing unnecessary de-duplication work.

Common ORM-generated queries and view-based queries often include LEFT JOINs
that are semantically unnecessary.

### Solution

Left Join Elimination (LJE) removes unnecessary tables from the right side of
LEFT JOINs during the resolution phase, before the optimizer builds access
paths. The feature recognizes three elimination scenarios:

1. **GROUP BY / DISTINCT elimination**: If the query has GROUP BY or DISTINCT
   and no aggregate functions depend on the cardinality of the eliminated table,
   the table can be removed because de-duplication cancels out the row
   multiplication caused by the left join.

2. **Unique-key elimination**: If the ON condition contains equalities that
   functionally determine all columns of a unique key on the weak side, then
   each left-side row matches at most one right-side row (N = 1), so the join
   is redundant. This also applies to materialized derived tables whose
   definitions contain GROUP BY or DISTINCT, creating pseudo-unique keys.

3. **Semi-join / Anti-join / IN/EXISTS subquery elimination**: If the left
   join is inside a semi-join, anti-join, or an untransformed IN/EXISTS
   subquery, the output is identical regardless of whether N = 1 or N > 1,
   so the unreferenced table can be eliminated.

### Execution Model

LJE runs during the query resolution phase (`sql_resolver.cc`), after
`simplify_joins()` and `record_join_nest_info()`. It operates on the
`Query_block`'s join nest structures, removing eliminated tables from
`m_table_nest` (so EXPLAIN does not show them) and `leaf_tables` (so
`JOIN::optimize()` ignores them), while keeping them in `m_table_list` (for
cleanup) and `LEX::query_tables` (for prepared statement re-execution).

If `outer_join == 0` (no outer joins present), LJE is skipped entirely,
ensuring zero CPU overhead for the most common queries.

### Code Volume

| Metric | Value |
|--------|-------|
| Commits | 6 (1 main + 5 bugfixes) |
| Files changed (main commit) | 66 |
| Lines added (main commit) | +7,101 |
| Lines removed (main commit) | -120 |
| New source file | 1 (`sql/sql_join_elimination.cc`, ~1,190 lines) |
| New test include | 1 (`join_elimination.inc`, 984 lines) |

---

## 2. New Files

### 2.1 SQL Layer

| File | Lines | Purpose |
|------|-------|---------|
| `sql/sql_join_elimination.cc` | ~1,190 | Core implementation: `Unique_analyzer` class, `do_join_elimination()`, `do_join_elimination_for_list()` |

No header file is needed; all public methods are declared in `sql/sql_lex.h`.

### 2.2 Test Files

| File | Lines | Purpose |
|------|-------|---------|
| `mysql-test/include/join_elimination.inc` | 984 | Comprehensive test cases for all LJE scenarios |
| `mysql-test/include/turn_off_join_elimination.inc` | ~5 | Helper to disable LJE for existing tests |
| `mysql-test/include/restore_opt_switch_after_turn_off_join_elimination.inc` | ~5 | Helper to restore optimizer switch |
| `mysql-test/r/join_elimination.result` | 2,349 | Expected results (InnoDB) |
| `mysql-test/t/join_elimination.test` | 3 | Test wrapper sourcing the .inc file |
| `mysql-test/suite/dstore_main/r/join_elimination-dstore.result` | 2,349 | Expected results (DStore) |
| `mysql-test/suite/dstore_main/t/join_elimination-dstore.test` | 3 | Test wrapper (DStore) |

---

## 3. Modifications to Existing Files

### 3.1 `sql/sql_const.h`

New optimizer switch constant:

```c++
constexpr const uint64_t OPTIMIZER_SWITCH_LEFT_JOIN_ELIMINATION{1ULL << 31};
```

### 3.2 `sql/sql_lex.h` (Query_block class)

New members:

```c++
uint m_hwm_free_tableno{0};       // High water mark for tableno allocation
table_map m_eliminated_tables{0};  // Bitmap of eliminated tables
uint m_saved_hwm_free_tableno{0};  // Backup for PS re-execution
```

New methods:

```c++
// Returns smallest guaranteed-free tableno
uint get_free_tableno() {
  if (m_hwm_free_tableno < leaf_table_count)
    m_hwm_free_tableno = leaf_table_count;
  return m_hwm_free_tableno;
}

void consume_tableno(uint tableno) {
  if (m_hwm_free_tableno <= tableno)
    m_hwm_free_tableno = tableno + 1;
}

// Returns map of all tables, excluding eliminated ones
table_map all_tables_map() const {
  return ((1ULL << std::max(leaf_table_count, m_hwm_free_tableno)) - 1) &
         ~m_eliminated_tables;
}

// Entry points for LJE
void do_join_elimination(THD *thd);
void do_join_elimination_for_list(THD *thd,
    mem_root_deque<Table_ref *> *tables,
    table_map *tables_used_out_of_FROM);
```

### 3.3 `sql/sql_lex.cc`

Save and restore of `m_hwm_free_tableno` for prepared statement re-execution:

```c++
// In save_properties():
m_saved_hwm_free_tableno = m_hwm_free_tableno;

// In restore_properties():
assert(m_hwm_free_tableno >= m_saved_hwm_free_tableno);
m_hwm_free_tableno = m_saved_hwm_free_tableno;
```

### 3.4 `sql/sql_resolver.cc`

Integration point -- LJE is called after `simplify_joins()` and
`record_join_nest_info()`:

```c++
if (simplify_joins(thd, &m_table_nest, true, false, &m_where_cond))
  return true;
if (record_join_nest_info(&m_table_nest)) return true;
if (outer_join &&
    thd->optimizer_switch_flag(OPTIMIZER_SWITCH_LEFT_JOIN_ELIMINATION))
  do_join_elimination(thd);
build_bitmap_for_nested_joins(&m_table_nest, 0);
```

Initialization and assertions:

```c++
// In resolve():
m_hwm_free_tableno = 0;

// In record_join_nest_info():
m_hwm_free_tableno = leaf_table_count;
assert(m_eliminated_tables == 0);
```

### 3.5 `sql/sql_optimizer.cc`

Adaptations for gaps in tableno values after elimination; modified
`optimize_keyuse()` to handle non-contiguous tablenos.

### 3.6 `sql/sys_vars.cc`

Registration of `left_join_elimination` in the `optimizer_switch` flagset
(on by default):

```c++
"left_join_elimination",  // in optimizer_switch_names
OPTIMIZER_SWITCH_LEFT_JOIN_ELIMINATION |  // in OPTIMIZER_SWITCH_DEFAULT
```

### 3.7 `sql/CMakeLists.txt`

Added `sql_join_elimination.cc` to build.

### 3.8 Other Modified Files

- `sql/sql_select.cc`: Include of new header
- `unittest/gunit/connect_joins-t.cc`: Unit test adjustments
- ~30 existing test include/test files: Added `turn_off_join_elimination` calls
  to preserve original EXPLAIN plans

---

## 4. Core Data Structures

### 4.1 Unique_analyzer (class, local to sql_join_elimination.cc)

Analyzes the ON condition of a LEFT JOIN, looking for equalities that
functionally determine columns of a unique key on the weak side.

```c++
class Unique_analyzer {
 private:
  const Query_block *qb;           // Owning query block
  table_map weak_tables;           // Map of all weak-side tables
  table_map tested_map_for_keys{0};// Tables already checked for unique keys
  table_map whole_tables_fd{0};    // Tables confirmed as functionally determined
  Mem_root_array<const Item_ident *> fd;  // Functionally dependent columns

  void analyze_scalar_eq(const Item_func_eq *cond,
                          const Item *left_item, const Item *right_item);

 public:
  Unique_analyzer(const Query_block *qb_, table_map weak_tables_,
                  MEM_ROOT *mem_root_)
      : qb(qb_), weak_tables(weak_tables_), fd(mem_root_) {}
  void analyze_conjunct(const Item *conjunct);
  bool all_weak_side_is_fd();
};
```

Key methods:

- `analyze_conjunct()`: Processes one AND-ed conjunct of the ON condition;
  dispatches to `analyze_scalar_eq()` for equality predicates.
- `analyze_scalar_eq()`: Checks if an equality maps strong-side expressions to
  weak-side columns. Guards against: RAND_TABLE_BIT (nondeterministic),
  non-FIELD_ITEM right side (view wrappers), and string type collation
  mismatches.
- `all_weak_side_is_fd()`: Determines if all weak-side tables are functionally
  determined. Checks explicit UNIQUE keys, then falls back to derived tables
  with GROUP BY or DISTINCT.

### 4.2 Key Data Members in Query_block

| Member | Type | Purpose |
|--------|------|---------|
| `m_eliminated_tables` | `table_map` | Bitmap of all tables eliminated by LJE |
| `m_hwm_free_tableno` | `uint` | High water mark for tableno allocation; prevents reuse of eliminated tables' tablenos |
| `m_saved_hwm_free_tableno` | `uint` | Backup of HWM for PS re-execution |

---

## 5. Execution Flow

### 5.1 Overall Flow

```
sql_resolver.cc: resolve()
  |
  +-> simplify_joins()               // outer-to-inner conversion
  +-> record_join_nest_info()        // sets outer_join, nested_join maps
  +-> if (outer_join && switch_on)
  |     do_join_elimination(thd)
  |       |
  |       +-> Check supported statement types (SELECT, INSERT SELECT,
  |       |   REPLACE SELECT, CREATE SELECT, subqueries)
  |       +-> Check for DEFAULT(weak_column) -- skip if present
  |       +-> Compute tables_used_out_of_FROM (common subset, computed once)
  |       +-> do_join_elimination_for_list(thd, &m_table_nest,
  |                                       &tables_used_out_of_FROM)
  |             |
  |             +-> For each table in the list (right to left):
  |                   1. Recurse into nested joins first
  |                   2. Skip if not the right side of a LEFT JOIN
  |                   3. Compute tables_used_in_FROM_out_of_cur_nest
  |                   4. Check if weak_tables referenced from outside -> skip
  |                   5. Determine elimination eligibility:
  |                      a. GROUP BY/DISTINCT (no agg_func_used)
  |                      b. Inside semi-join / anti-join nest
  |                      c. Inside IN/EXISTS subquery (no agg/windows)
  |                      d. Unique key fully determined (Unique_analyzer)
  |                   6. If eligible: remove from leaf_tables, fix bitmaps,
  |                      cleanup ON condition, detach derived tables,
  |                      record in m_eliminated_tables, erase from m_table_nest
  |             +-> If aggregates/windows exist, force update_used_tables()
  |
  +-> build_bitmap_for_nested_joins()
```

### 5.2 Call Order Rationale

`simplify_joins()` is called first because it simplifies the join nest
structure (e.g., converting `T1 LEFT JOIN T2 WHERE T2.col IS NOT NULL` to an
inner join), giving LJE a simpler structure to analyze and access to
`nested_join->used_tables`. `record_join_nest_info()` is called before LJE so
that `outer_join` is available -- if it is zero, LJE is skipped entirely (zero
CPU cost for queries without outer joins). The accepted trade-off is that LJE
must then correct information previously set by `record_join_nest_info()`.

### 5.3 Reference Analysis (tables_used_out_of_FROM)

Computes the map of tables referenced from outside the FROM clause. This is
done once and reused across multiple elimination rounds. Uses `Item::compile()`
with a custom analyzer to correctly handle `Item_sum::used_tables()` which
returns all tables (unreliable for aggregates and window functions).

Sources collected from: `visible_fields()`, `where_cond()`, `having_cond()`,
`group_list`, `order_list`, `m_windows`, and ON DUPLICATE KEY UPDATE values.

### 5.4 Reference Analysis (tables_used_in_FROM_out_of_cur_nest)

Computes tables referenced in the FROM clause but outside the current join
nest. Temporarily hides the current nest by setting `table->nested_join =
nullptr` to prevent `walk_join_list` from diving into it.

Sources: `join_cond()->used_tables()` from other tables, lateral dependencies
from derived tables, and `table_function->used_tables()`.

### 5.5 Elimination Conditions

#### Condition 1: GROUP BY or DISTINCT

```c++
if (is_grouped() || is_distinct()) {
  // Aggregates depend on cardinality and are computed before de-duplication
  // Window functions are ok with GROUP BY (computed after), but not with
  // DISTINCT (computed before). LIMIT is ok (applied after de-duplication).
  if (!agg_func_used() && (!has_windows() || is_grouped()))
    can_eliminate = true;
}
```

#### Condition 2: Semi-join / Anti-join Context

Walks the `table->embedding` chain upward. Works even with multiple nest levels
between the semi-anti-join and the left join.

#### Condition 3: IN / EXISTS Subquery Context

Checks `master_query_expression()->item` to determine if the query block is
inside an IN or EXISTS subquery. Requires no aggregates and no windows.
LIMIT in IN is not supported by MySQL, so it is implicitly excluded.

#### Condition 4: Unique Key Functional Dependency

Uses `Unique_analyzer` to check all AND-ed conjuncts in the ON condition. Each
equality is analyzed to find weak-side columns that are functionally dependent
on strong-side expressions. Then checks whether any UNIQUE key of each weak-side
table has all its columns in the FD list. For derived tables, checks GROUP BY
or DISTINCT expressions.

### 5.6 Elimination Execution

When `can_eliminate` is true:

1. **Remove from leaf_tables**: Walk the linked list, unlink eliminated tables,
   decrement `leaf_table_count`.
2. **Clear read_set**: `bitmap_clear_all(tl->table->read_set)` -- triggers
   assertion failure if any stale reference tries to read.
3. **Fix parent bitmaps**: Walk `table->embedding` chain, removing
   `weak_tables` from `used_tables`, `not_null_tables`, `sj_corr_tables`,
   `sj_depends_on`, `dep_tables`, `join_cond_dep_tables`, `sj_inner_tables`.
4. **Fix outer_join**: `outer_join &= ~weak_tables`.
5. **Remove empty semi-join nests**: If `sj_nest->sj_inner_tables` is a subset
   of `weak_tables`, erase from `sj_nests`.
6. **Fix dependency chains**: If a table after the eliminated one has
   `dep_tables` referencing it, redirect to the next outer table.
7. **Cleanup ON condition**: Walk eliminated table's join condition with
   `Item::clean_up_after_removal`, which detaches subquery query expressions.
8. **Detach derived table expressions**: For materialized derived tables, call
   `exclude_tree()` on the derived query expression (unless shared CTE).
9. **Record eliminated tables**: `m_eliminated_tables |= weak_tables`.
10. **Remove from m_table_nest**: `tables->erase(li)`.
11. **Update used_tables for aggregates/windows**: If eliminated tables exist
    and there are aggregates or window functions, force `update_used_tables()`
    to remove stale table references. Final assertion confirms no eliminated
    table is referenced.

---

## 6. System Variables

### optimizer_switch flag: `left_join_elimination`

| Property | Value |
|----------|-------|
| Type | Boolean flag within `optimizer_switch` |
| Default | `on` |
| Scope | SESSION, GLOBAL |
| Bit position | 31 (`1ULL << 31`) |
| Constant | `OPTIMIZER_SWITCH_LEFT_JOIN_ELIMINATION` |

Usage:

```sql
SET optimizer_switch='left_join_elimination=off';
SET optimizer_switch='left_join_elimination=on';
```

---

## 7. EXPLAIN Output

When a table is eliminated, it no longer appears in any EXPLAIN format.
The rewritten query shown in the `Note` line of `EXPLAIN` does not contain
the eliminated table.

Optimizer trace output when elimination occurs:

```json
{
  "join_elimination": {
    "number_of_eliminated_tables": 1
  }
}
```

The expanded query printed in the trace reflects the post-elimination form.

### Example

Before elimination (switch off):

```
EXPLAIN SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
+----+-------------+-------+...
| id | select_type | table |...
+----+-------------+-------+...
|  1 | SIMPLE      | t1    |...
|  1 | SIMPLE      | t2    |...
```

After elimination (switch on, default):

```
EXPLAIN SELECT DISTINCT t1.a FROM t1 LEFT JOIN t2 ON t2.a>t1.b;
+----+-------------+-------+...
| id | select_type | table |...
+----+-------------+-------+...
|  1 | SIMPLE      | t1    |...
```

---

## 8. Error Handling

| Scenario | Handling |
|----------|----------|
| DEFAULT(weak_column) present | `OPTION_DEFAULT_WEAK_COLUMN` flag prevents LJE entirely; avoids MySQL bug #119453 where DEFAULT returns NULL on NULL-complemented rows |
| Tableno gaps after elimination | `m_hwm_free_tableno` tracks highest tableno; `get_free_tableno()` prevents reuse; `all_tables_map()` masks eliminated tables |
| Null pointer in `attach_join_condition_to_nest` | Fixed in bugfix #2: null check added before accessing join condition |
| Wrong results when elimination incorrectly applied | Fixed in bugfix #3: additional conditions checked before elimination |
| Crash with `partial_result_cache` + LJE | Fixed in bugfix #4: `CreateNestedLoopAccessPath` null pointer; both features share this fix |
| Crash in `compare_costs_of_subquery_strategies` | Fixed in bugfixes #5 and #6: null pointer when subquery cost comparison encounters eliminated tables |
| ON condition cleanup | `Item::clean_up_after_removal` walks the condition tree to detach subqueries safely |
| Shared CTE references | CTEs with multiple references or recursive CTEs are not detached to preserve shared materialization |

When LJE encounters any issue during analysis, it simply does not eliminate the
table -- there is no error reported to the user. The query falls back to the
normal execution path with all tables present.

---

## 9. Test Coverage

### 9.1 Main Test File

`mysql-test/include/join_elimination.inc` (984 lines) covers:

| Category | Test Cases |
|----------|-----------|
| Basic elimination | GROUP BY, DISTINCT with no references to weak side |
| Unique key | Equality on PRIMARY KEY, multi-column UNIQUE key |
| String type guards | CHAR vs INT comparison, collation mismatch |
| Nested joins | Multi-level nesting, `t1 LEFT JOIN (t2 LEFT JOIN t3)` |
| Consecutive eliminations | t3 eliminated, then t2 becomes eligible |
| Two-table nest elimination | Eliminating `t2 JOIN t3` as a unit |
| Semi-join / anti-join | EXISTS, IN subquery context, nested levels |
| Window functions | With GROUP BY (allowed) vs DISTINCT (blocked) |
| Aggregate functions | COUNT, SUM preventing elimination |
| View and derived tables | Pseudo-unique key via GROUP BY/DISTINCT in derived table |
| DEFAULT(column) | Prevention of elimination with DEFAULT on weak column |
| ON DUPLICATE KEY UPDATE | INSERT SELECT with references in update values |
| Table functions | JSON_TABLE on weak side, lateral derived tables |
| Lateral derived tables | Materialized lateral, non-materialized lateral |
| Subquery in ON | Subquery in ON condition of eliminated table |
| Switch on/off | `SET optimizer_switch='left_join_elimination=off'` |
| Supported statements | SELECT, INSERT SELECT, REPLACE SELECT, CREATE SELECT |
| Unsupported statements | Multi-table UPDATE/DELETE excluded |
| Right join | `t2 RIGHT JOIN t1` (flipped to LEFT JOIN internally) |
| Prepared statements | PS protocol re-execution after elimination |

### 9.2 Existing Test Updates

~30 existing test files include `turn_off_join_elimination.inc` to preserve
their original EXPLAIN plans. These include tests for:
`desc_index`, `select`, `subquery`, `subquery_sj`,
`explain_for_connection_small`, `explain_non_select`,
`gcol_select`, `opt_trace/subquery`,
`partial_result_cache`, `ptrc_bugs`, and their dstore variants.

### 9.3 Bug-Specific Tests

Each of the 5 bugfix commits adds regression tests for the specific crash or
wrong-result scenario they fix.

---

## 10. Limitations

1. **No inner join elimination**: Only LEFT JOIN elimination is implemented.
   Inner join elimination (via self-join or FK-PK relationships) is described
   in source code comments but not implemented.

2. **No multi-table UPDATE/DELETE support**: Excluded because they establish
   pointers between tables to modify and tables in FROM, making elimination
   risky. They also cannot contain GROUP BY or DISTINCT.

3. **DEFAULT(column) blocks elimination**: If `DEFAULT(weak_column)` appears
   anywhere in the query, elimination is disabled due to a MySQL bug where
   DEFAULT returns NULL for NULL-complemented rows of non-nullable columns
   (bug #119453).

4. **Aggregate functions block GROUP BY/DISTINCT elimination**: Queries like
   `SELECT COUNT(*) FROM t1 LEFT JOIN t2 ON ... GROUP BY t1.a` cannot
   eliminate t2 because COUNT depends on cardinality.

5. **Window functions block DISTINCT elimination**: Window functions are
   computed before DISTINCT de-duplication, so they depend on cardinality in
   DISTINCT queries. However, they are allowed with GROUP BY (computed after
   de-duplication).

6. **No equality propagation in ON condition**: The `Unique_analyzer` does not
   perform equality propagation within the ON condition, which could miss some
   elimination opportunities.

7. **String comparison collation sensitivity**: When the weak-side column is a
   string type, elimination is blocked unless the comparison uses the same
   collation as the column's definition.

8. **RAND_TABLE_BIT blocks unique-key elimination**: If the strong-side
   expression in an ON equality contains `RAND()`, elimination is blocked.

9. **Tableno gaps**: After elimination, tablenos are not renumbered. Code that
   assumes contiguous tablenos must use alternative lookup methods.

10. **Eliminated tables remain open**: Tables remain in `LEX::query_tables`
    and `m_table_list`, so they are still opened during prepared statement
    re-execution (necessary for proper cleanup).

---

## 11. Implementation Order

| Step | Component | Description |
|------|-----------|-------------|
| 1 | `sql/sql_const.h` | Define `OPTIMIZER_SWITCH_LEFT_JOIN_ELIMINATION` bit constant |
| 2 | `sql/sql_lex.h` | Add `m_eliminated_tables`, `m_hwm_free_tableno`, `m_saved_hwm_free_tableno`, method declarations |
| 3 | `sql/sql_join_elimination.cc` | Implement `Unique_analyzer`, `do_join_elimination()`, `do_join_elimination_for_list()` |
| 4 | `sql/CMakeLists.txt` | Add new source file to build |
| 5 | `sql/sys_vars.cc` | Add `left_join_elimination` flag to `optimizer_switch` |
| 6 | `sql/sql_resolver.cc` | Add call site after `simplify_joins()` and `record_join_nest_info()` |
| 7 | `sql/sql_optimizer.cc` | Adapt for tableno gaps (key_use handling) |
| 8 | `sql/sql_lex.cc` | Add HWM save/restore logic |
| 9 | Test infrastructure | Create `turn_off_join_elimination.inc`, `restore_opt_switch_after_turn_off_join_elimination.inc` |
| 10 | Test cases | Create `join_elimination.inc` with comprehensive coverage |
| 11 | Existing test adjustments | Add `turn_off_join_elimination` includes to affected tests |
| 12 | Bugfixes | Apply crash and wrong-result fixes from commits #2-#6 |
