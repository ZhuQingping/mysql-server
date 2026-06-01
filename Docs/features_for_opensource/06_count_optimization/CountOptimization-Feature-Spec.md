# COUNT Optimization (Simplify COUNT(not_null_col)) - Feature Specification

> Version: 3.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main commit: 63c4346880e18bf02268bdfd0097426e8e4cdb6e
> Author: Guilhem Bichot
> Commits: 2 (1 main + 1 post-fix)
> Files changed: 31 (+790/-36 main), 36 (+522/-193 post-fix)
> Total delta: +1,312 / -229

---

## 1. Overview

### Problem Statement

`COUNT(column)` counts non-NULL values of `column`. When `column` is declared
`NOT NULL`, `COUNT(column)` is semantically equivalent to `COUNT(*)` because
there are no NULL values to skip. However, MySQL still reads the column during
execution, which prevents the use of covering indexes that do not include the
column. This results in unnecessary table lookups instead of index-only scans.

Example:

```sql
CREATE TABLE t1(a INT NOT NULL, b INT, INDEX(b));
SELECT COUNT(a) FROM t1 WHERE b > 1;
-- Without optimization: reads column a, cannot use index on b alone
-- With optimization: COUNT(a) becomes COUNT(0), index on b is covering
```

### Solution

During `Item_sum_count::fix_fields()`, if the argument to COUNT is a single
non-nullable column, replace it with the constant 0 (`Item_int(0, 1)`). This
has the same semantics (COUNT of a constant non-NULL expression is equivalent
to COUNT(*)) and removes the column dependency, allowing the optimizer to use
covering indexes that do not include the column.

The replacement is done carefully to avoid side effects on the table's bitmap
state (covering_keys, read_set), using a `Variable_scope_guard` on
`thd->mark_used_columns` to defer column marking until after the nullability
check.

### Execution Model

The optimization occurs during `Item_sum_count::fix_fields()`, in the
resolution phase. It is a compile-time transformation -- the `COUNT(col)`
expression is rewritten to `COUNT(0)` before the optimizer sees it. The feature
is controlled by the `simplify_count_not_null` flag in `optimizer_switch`.

### Code Volume

| Metric | Value |
|--------|-------|
| Commits | 2 (1 main + 1 post-fix) |
| Files changed (main commit) | 31 |
| Lines added (main commit) | +790 |
| Lines removed (main commit) | -36 |
| Files changed (post-fix) | 36 |
| Lines added (post-fix) | +522 |
| Lines removed (post-fix) | -193 |
| Core logic (item_sum.cc) | ~97 new lines |

The post-fix commit changed the control mechanism from an independent system
variable (`rds_simplify_count_not_null`) to an `optimizer_switch` flag
(`simplify_count_not_null`), added more test cases (prepared statements,
stored procedures), and fixed a GCC 11 compile error in `sql_plan_cache.h`.

---

## 2. New Files

No new source files are created. All changes are modifications to existing
files.

### 2.1 Test Files

| File | Lines | Purpose |
|------|-------|---------|
| `mysql-test/suite/dstore_main/t/simplify_count_not_null.test` | 213 | Main test file (shared between InnoDB and DStore) |
| `mysql-test/suite/dstore_main/r/simplify_count_not_null.result` | 486 | Expected results |

---

## 3. Modifications to Existing Files

### 3.1 `sql/item_sum.cc` (Core Implementation, ~97 new lines)

The main implementation is in `Item_sum_count::fix_fields()`:

```c++
bool Item_sum_count::fix_fields(THD *thd, Item **ref) {
  if (super::fix_fields(thd, ref)) return true;
  if (init_sum_func_check(thd)) return true;

  auto query_block = thd->lex->current_query_block();
  Condition_context CCT(query_block);
  set_nullable(false);

  if (thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SIMPLIFY_COUNT_NOT_NULL) &&
      sum_func() == COUNT_FUNC && arg_count == 1 &&
      args[0]->type() == FIELD_ITEM &&
      !(m_is_window_function && query_block->olap != UNSPECIFIED_OLAP_TYPE) &&
      !thd->lex->is_view_context_analysis()) {
    // Phase 1: Run fix_fields on the column WITHOUT marking it used
    {
      Variable_scope_guard guard(thd->mark_used_columns);
      thd->mark_used_columns = MARK_COLUMNS_NONE;
      if ((!args[0]->fixed && args[0]->fix_fields(thd, args + 0)) ||
          args[0]->check_cols(1))
        return true;
    }

    // Phase 2: Check nullability
    set_nullable(is_nullable() || args[0]->is_nullable());
    if (!args[0]->is_nullable()) {
      // Simplify to COUNT(*) (i.e. COUNT(0))
      args[0] = new (thd->mem_root) Item_int(int32{0}, 1);
      if (args[0] == nullptr) return true;
    } else {
      // Cannot simplify; execute the deferred column marking
      if (args[0]->type() != FIELD_ITEM) {
        Mark_field mf(thd->mark_used_columns);
        args[0]->walk(&Item::mark_field_in_map, enum_walk::SUBQUERY_POSTFIX,
                      reinterpret_cast<uchar *>(&mf));
      } else {
        auto f = down_cast<Item_field *>(args[0])->field;
        f->table->mark_column_used(f, thd->mark_used_columns);
      }
    }
  } else {
    // Normal path: fix_fields on all arguments with standard column marking
    for (uint i = 0; i < arg_count; i++) {
      if ((!args[i]->fixed && args[i]->fix_fields(thd, args + i)) ||
          args[i]->check_cols(1))
        return true;
      set_nullable(is_nullable() || args[i]->is_nullable());
    }
  }
  // ... rest of fix_fields
}
```

### 3.2 `sql/sql_const.h`

New optimizer switch constant (final version, after post-fix):

```c++
/**
   Replace COUNT(column) with COUNT(*) if they are equivalent. This may
   eliminate the need for 'column' during execution, opening more
   possibilities for access methods.
*/
constexpr const uint64_t OPTIMIZER_SWITCH_SIMPLIFY_COUNT_NOT_NULL{1ULL << 27};
```

Note: The initial commit used a separate system variable
`rds_simplify_count_not_null`. The post-fix commit moved it to
`optimizer_switch` as `simplify_count_not_null` (bit 27).

### 3.3 `sql/sys_vars.cc`

Added `simplify_count_not_null` to the `optimizer_switch` flagset (on by
default):

```c++
"simplify_count_not_null",  // in optimizer_switch_names
OPTIMIZER_SWITCH_SIMPLIFY_COUNT_NOT_NULL |  // in OPTIMIZER_SWITCH_DEFAULT
```

Note: The initial commit defined a separate `Sys_var_bool
Sys_rds_simplify_count_not_null` system variable. The post-fix commit removed
this and moved the control to `optimizer_switch`.

### 3.4 `sql/system_variables.h`

The initial commit added:
```c++
bool rds_simplify_count_not_null;  // removed in post-fix
```

The post-fix commit removed this member since the feature is now controlled
via `optimizer_switch`.

### 3.5 `sql/sql_plan_cache.h`

GCC 11 compile fix: corrected `std::map` template parameter to match the
allocator's value_type. This is a general fix, not specific to COUNT
optimization, but was included in the same commit because the error appeared
when building with the feature enabled.

### 3.6 Other Modified Files (Test Result Updates)

~25 existing test result files updated, mostly to:
- Reflect `simplify_count_not_null=on` in `optimizer_switch` default value
- Show `count(0)` instead of `count(column)` in EXPLAIN Note lines
- Disable the feature in specific tests that need the original column
  reference (using `SET optimizer_switch='simplify_count_not_null=off'`)

Affected test files include:
- `explain_non_select.inc`, `skip_scan_test.inc`, `group_by.result`
- `heap_btree.result`, `heap_hash.result`, `join.result`
- `subquery_scalar_to_derived.result`, `invisible_indexes.result`
- Various MyISAM and dstore variants

---

## 4. Core Data Structures

No new data structures are introduced. The feature works by modifying existing
structures in-place:

| Structure | Modification |
|-----------|-------------|
| `Item_sum_count::args[0]` | Replaced from `Item_field` (column reference) to `Item_int(0, 1)` (constant) |
| `thd->mark_used_columns` | Temporarily set to `MARK_COLUMNS_NONE` during nullability check |
| `table->covering_keys` | Left unchanged because column was never marked as used |

### 4.1 Variable_scope_guard

The key technique is using `Variable_scope_guard` to temporarily suppress
column marking:

```c++
Variable_scope_guard guard(thd->mark_used_columns);
thd->mark_used_columns = MARK_COLUMNS_NONE;
```

This prevents `fix_fields()` from updating `table->covering_keys`,
`table->read_set`, and other bitmaps. If the column turns out to be nullable,
the deferred marking is executed manually.

---

## 5. Execution Flow

### 5.1 Overall Flow

```
Item_sum_count::fix_fields(thd, ref)
  |
  +-> Check eligibility:
  |     - simplify_count_not_null switch is ON
  |     - sum_func() == COUNT_FUNC
  |     - arg_count == 1 (single argument)
  |     - args[0]->type() == FIELD_ITEM (column reference)
  |     - Not a ROLLUP window function
  |     - Not creating a view
  |
  +-> If eligible:
  |     1. Suppress column marking (MARK_COLUMNS_NONE)
  |     2. Run fix_fields on args[0]
  |     3. Check args[0]->is_nullable()
  |     4. If NOT nullable:
  |        - Replace args[0] with Item_int(0, 1)
  |        - Column never marked as used -> covering_keys preserved
  |     5. If nullable:
  |        - Execute deferred column marking manually
  |
  +-> If not eligible:
        - Standard fix_fields on all arguments
```

### 5.2 Eligibility Conditions

The optimization applies when ALL of the following are true:

1. `optimizer_switch` flag `simplify_count_not_null` is ON
2. The aggregate function is `COUNT` (not SUM, AVG, etc.)
3. COUNT has exactly one argument (`COUNT(col)`, not `COUNT(DISTINCT col)`)
4. The argument is a direct column reference (`FIELD_ITEM`)
5. The COUNT is not a ROLLUP window function (ROLLUP makes non-nullable
   columns nullable)
6. Not creating a view definition (the rewrite would change the view's
   semantics for outer references)

### 5.3 Why MARK_COLUMNS_NONE is Necessary

When `fix_fields()` is called on a column, it normally calls
`mark_column_used()`, which:

1. Adds the column to `table->read_set`
2. Removes from `table->covering_keys` any index that does not include this
   column
3. Updates other bitmaps (key_read, etc.)

If we let this happen before simplifying, the covering index optimization
would already be defeated. By suppressing the marking during the inner
`fix_fields()`, we can safely check nullability and then simplify, at which
point the column is no longer needed.

### 5.4 Why ROLLUP is Excluded

A window function with ROLLUP applies after ROLLUP, but ROLLUP can make a
non-nullable column become nullable (by adding NULL group rows). This
nullability change happens only later (in `resolve_rollup_item()`), so in
`fix_fields()` the column would still appear non-nullable, which would be
incorrect.

### 5.5 Why View Creation is Excluded

If the rewrite happens during view creation, it changes the query's semantics.
For example:

```sql
CREATE VIEW v2 AS
  SELECT (SELECT count(ext.a) FROM t1 WHERE t1.b>1)
  FROM t1 AS ext WHERE b>1;
```

With the rewrite, `count(ext.a)` becomes `count(0)`, which changes where
aggregation occurs (top query vs subquery). The rewrite is allowed when
*reading* from the view, but not when *creating* it.

### 5.6 Outer Reference Limitation

If the COUNT argument is an outer reference (`Item_outer_ref`), the
optimization is applied but may not be fully effective because
`Item_outer_ref::fix_fields()` has an explicit call to mark the column with
`MARK_COLUMNS_READ`, regardless of `thd->mark_used_columns`. This was
considered an acceptable trade-off given the rareness of `COUNT(outer_ref)`.

### 5.7 Post-Simplification Behavior

After `COUNT(column)` is simplified to `COUNT(0)`:

1. The `Item_int(0, 1)` has `used_tables() == 0` (no table dependency)
2. The optimizer sees `COUNT(0)` which is equivalent to `COUNT(*)`
3. Covering indexes that do not include the original column become available
4. EXPLAIN shows the rewrite as a "Note" and may show "Using index"

---

## 6. System Variables

### optimizer_switch flag: `simplify_count_not_null`

| Property | Value |
|----------|-------|
| Type | Boolean flag within `optimizer_switch` |
| Default | `on` |
| Scope | SESSION, GLOBAL |
| Bit position | 27 (`1ULL << 27`) |
| Constant | `OPTIMIZER_SWITCH_SIMPLIFY_COUNT_NOT_NULL` |

Usage:

```sql
SET optimizer_switch='simplify_count_not_null=off';
SET optimizer_switch='simplify_count_not_null=on';
```

Note: The initial implementation used a separate boolean system variable
`rds_simplify_count_not_null` (default OFF). The post-fix commit moved it
to `optimizer_switch` and changed the default to ON.

---

## 7. EXPLAIN Output

### Rewritten Expression

The EXPLAIN `Note` line shows `count(0)` instead of `count(column)`:

```sql
-- Without optimization (switch off):
Note 1003: select count(`test`.`t1`.`a`) AS `count(a)` from `test`.`t1`
  where (`test`.`t1`.`b` > 1)

-- With optimization (switch on):
Note 1003: select count(0) AS `count(a)` from `test`.`t1`
  where (`test`.`t1`.`b` > 1)
```

### Covering Index Effect

The most visible EXPLAIN change is the appearance of "Using index" in the
Extra column:

```sql
-- Without optimization:
EXPLAIN SELECT count(a) FROM t1 WHERE b>1;
Extra: Using aggregate
-- (needs to read column a from the table)

-- With optimization:
EXPLAIN SELECT count(a) FROM t1 WHERE b>1;
Extra: Using aggregate; Using index
-- (index on b is now covering because a is no longer needed)
```

---

## 8. Error Handling

The feature has very simple error handling because it is a compile-time
transformation:

| Scenario | Handling |
|----------|----------|
| Column is nullable | Optimization skipped; normal `COUNT(column)` evaluation |
| ROLLUP window function | Optimization skipped |
| View creation context | Optimization skipped (`is_view_context_analysis()`) |
| Non-FIELD_ITEM argument | Optimization skipped (e.g., `COUNT(a+0)`, `COUNT(DISTINCT a)`) |
| Outer reference | Optimization applied but may not be fully effective |
| OOM on Item_int creation | Returns `true` (error), aborts fix_fields |

If any condition is not met, the code falls through to the standard path
where `fix_fields()` is called on all arguments with normal column marking.

### Post-Fix: Plan Cache Compatibility

The post-fix commit (#2) addressed an issue where the optimization interacted
incorrectly with the plan cache feature. The fix ensures that:
- The `optimizer_switch` flag is properly checked during plan cache decisions
- The GCC 11 compile error in `sql_plan_cache.h` (incorrect `std::map`
  allocator value_type) is fixed

---

## 9. Test Coverage

### 9.1 Main Test File

`mysql-test/suite/dstore_main/t/simplify_count_not_null.test` (213 lines)
covers:

| Category | Test Cases |
|----------|-----------|
| Basic optimization | `COUNT(not_null_col)` with WHERE clause |
| Covering index effect | "Using index" appearing in EXPLAIN after optimization |
| Nullable column | `COUNT(b)` where b is nullable -- no rewrite |
| `COUNT(*)` and `COUNT(0)` | Already optimal, no change needed |
| Mixed aggregates | `COUNT(a)`, `COUNT(DISTINCT a)`, `COUNT(b)`, `SUM(a)`, `COUNT(a+1)` |
| Inner join vs left join | Column becomes nullable on right side of LEFT JOIN |
| Group by | `COUNT(a)` with GROUP BY |
| Outer reference | `COUNT(ext.a)` in correlated subquery |
| View (merged) | `COUNT(a)` through a merged view definition |
| View (materialized) | `COUNT(a)` through a materialized view with NO_MERGE hint |
| Derived table (merged) | `COUNT(a)` through a merged derived table |
| Derived table (materialized) | `COUNT(a)` through a materialized derived table |
| Outer reference + GROUP BY | `COUNT(ext.a)` with GROUP BY |
| Switch on/off | Both states verified |
| Prepared statement | PS protocol with `COUNT(not_null_col)` |
| Stored procedure | SP with `COUNT(not_null_col)` |
| NOT NULL column | Verified that only NOT NULL columns are simplified |
| Multiple tables | `COUNT(t1.a)`, `COUNT(t2.a)` in joins |
| Three-table left join | `COUNT(t2.a)`, `COUNT(t1.a)`, `COUNT(t3.a)` |

### 9.2 Test Pattern

The test uses a loop to run all queries twice: once with
`simplify_count_not_null=off` and once with `=on`. This allows comparing
EXPLAIN output and verifying that:
1. Results are identical with and without the optimization
2. EXPLAIN shows covering index usage when the optimization is on
3. The "Note" about count rewrite appears in EXPLAIN

### 9.3 Existing Test Adjustments

Several existing tests are adjusted to disable the feature when it would
change their expected EXPLAIN output:

```sql
-- Pattern used in affected tests:
SET @save_optimizer_switch=@@optimizer_switch;
SET optimizer_switch='simplify_count_not_null=off';
-- ... test code ...
SET optimizer_switch=@save_optimizer_switch;
```

Affected files: `explain_non_select.inc`, `skip_scan_test.inc`,
`heap_btree.test`, `heap_hash.test`, and their dstore variants.

---

## 10. Limitations

1. **Only single-column FIELD_ITEM is optimized**: `COUNT(a+0)`,
   `COUNT(DISTINCT a)`, and other non-trivial expressions are not simplified.
   This is intentional to minimize bug risk.

2. **ROLLUP window functions excluded**: ROLLUP can make non-nullable columns
   nullable, and this change happens after `fix_fields()`, so the optimization
   would be incorrect.

3. **View creation excluded**: Rewriting during view creation changes the
   query's semantics for outer references. The rewrite is allowed when reading
   from the view, but not when creating it.

4. **Outer references partially effective**: `Item_outer_ref::fix_fields()`
   marks columns with `MARK_COLUMNS_READ` regardless of
   `thd->mark_used_columns`, so the optimization may not achieve a covering
   index for `COUNT(outer_ref)`.

5. **No EXPLAIN indicator**: There is no specific EXPLAIN tag like
   "COUNT simplified". The only visible effect is:
   - `count(0)` in the Note line instead of `count(column)`
   - "Using index" may appear in Extra when it previously did not

6. **Not applied to COUNT(DISTINCT col)**: `COUNT(DISTINCT not_null_col)`
   is not simplified because DISTINCT still requires reading the column values
   for uniqueness checking.

7. **Replacement happens after max_aggr_level is set**: This is intentional
   and correct -- the aggregation query of the Item is not influenced by
   replacing args[0] -- but requires care if modifying the code.

---

## 11. Implementation Order

| Step | Component | Description |
|------|-----------|-------------|
| 1 | `sql/sql_const.h` | Define `OPTIMIZER_SWITCH_SIMPLIFY_COUNT_NOT_NULL` bit constant (bit 27) |
| 2 | `sql/sys_vars.cc` | Add `simplify_count_not_null` to `optimizer_switch_names` and `OPTIMIZER_SWITCH_DEFAULT` |
| 3 | `sql/item_sum.cc` | Implement the nullability check and replacement logic in `Item_sum_count::fix_fields()` |
| 4 | `sql/sql_plan_cache.h` | Fix GCC 11 compile error (allocator value_type mismatch) |
| 5 | Test cases | Create `simplify_count_not_null.test` with comprehensive coverage |
| 6 | Existing test adjustments | Add `simplify_count_not_null=off` to affected tests |
| 7 | Result updates | Update all existing test results that include `optimizer_switch` output or `count()` expressions |
