# Redundant Condition Removal -- Feature Specification

## 1. Overview

### Problem Statement

When MySQL uses an index range scan to access a table, the range optimizer evaluates conditions like `col > 5` or `col BETWEEN 2 AND 10` to determine the scan range. However, after the range scan is chosen, the SQL layer still evaluates the same conditions for every row returned by the storage engine. This is redundant work: the storage engine has already guaranteed that the condition holds for every row in the range. Removing these redundant conditions from the SQL-layer filter reduces CPU overhead, especially for queries with large result sets.

### Solution

Extend the existing `reduce_cond_for_table()` function (which already handles redundant conditions for ref/equality access) to also handle redundant inequality conditions for range scan access. When a table is accessed via `INDEX_RANGE_SCAN` and the range covers a single interval, the optimizer compares each comparison predicate in the WHERE clause against the range's min/max key values. If the predicate is guaranteed to be true by the range bounds, it is removed from the SQL-layer filter.

A new session variable `rds_empty_redundant_check_in_range_scan` (default: ON) controls this optimization. The feature integrates with the existing `finalize_table_conditions()` function which is called after the access path has been chosen.

### Execution Model

```
Before optimization:
  SQL layer: WHERE col > 5 AND col < 10 AND other_cond
  Range scan: scans index range (5, 10)
  For each row: evaluates (col > 5) AND (col < 10) AND other_cond

After optimization:
  SQL layer: other_cond
  Range scan: scans index range (5, 10)  (same range)
  For each row: evaluates only other_cond
  (col > 5 and col < 10 are guaranteed true by the range)
```

### Code Volume

| Metric | Value |
|--------|-------|
| Files changed | 220 (across 3 commits) |
| Lines added | +5,184 |
| Lines removed | -1,344 |
| Net change | +3,840 |
| Core source files | 2 (`sql/sql_optimizer.cc`, `sql/sys_vars.cc`, `sql/system_variables.h`) |
| Test-only files | ~218 (mostly `.result` file updates) |

Note: The large file count (220) is primarily due to result file updates across the MTR test suite, as removing redundant conditions changes the "Extra" column in EXPLAIN output across many existing tests.

---

## 2. New Files

| File | Description |
|------|-------------|
| `mysql-test/t/range_scan_remove_redundant_condition.test` | Main MTR test suite (697 lines) for redundant condition removal in range scans |
| `mysql-test/r/range_scan_remove_redundant_condition.result` | Expected results for main test |
| `mysql-test/r/range_scan_remove_redundant_condition.result-pq` | Expected results for parallel query variant |
| `mysql-test/suite/dstore_main/r/range_scan_remove_redundant_condition-dstore.result` | DStore expected results |
| `mysql-test/suite/dstore_main/t/range_scan_remove_redundant_condition-dstore.test` | DStore test variant |

---

## 3. Modifications to Existing Files

| File | Change Description |
|------|--------------------|
| `sql/sql_optimizer.cc` | Extended `reduce_cond_for_table()` with range scan redundancy check for LT/GT/EQ/LE/GE/BETWEEN/ISNULL operators; added `is_column_then_constant()` helper; added OR-clause safeguard; added BETWEEN non-constant third-argument check (bug fix #3) |
| `sql/system_variables.h` | Added `bool rds_empty_redundant_check_in_range_scan` member to `System_variables` |
| `sql/sys_vars.cc` | Added `Sys_var_bool Sys_var_rds_empty_redundant_check_in_range_scan` session variable |
| `sql/join_optimizer/explain_access_path.cc` | Added documentation comment referencing the feature |
| ~218 result files | Updated EXPLAIN output across the MTR suite where redundant conditions were removed (e.g., "Using where" disappears, "attached_condition" removed in JSON format, "index_condition" removed) |

---

## 4. Core Data Structures

### 4.1 Range Scan Redundancy Check Algorithm

The algorithm for determining if a comparison is redundant proceeds in these steps:

1. **Identify the field and constant**: Determine which side of the comparison is the column and which is the constant. Three paths:
   - `path_1`: left_item is field, right_item is constant
   - `path_2`: right_item is field, left_item is constant
   - Neither: cannot optimize

2. **Verify range scan access**: Check that the table is accessed via `INDEX_RANGE_SCAN` (not dynamic range), the field type is not `MYSQL_TYPE_BIT`, and the field is comparable in the index.

3. **Verify single range**: The range scan must have `num_ranges == 1`. Disjoint ranges from OR clauses are not supported.

4. **Locate the key part**: Find the position of the field in the range scan's `used_key_part` array and compute the byte offset (`key_offset`) into `key_min`/`key_max`.

5. **Compare values**: Depending on the operator, compare the constant against `key_min` and/or `key_max`:
   - `EQ_FUNC`: compare against both min and max
   - `LT_FUNC`/`LE_FUNC`: compare against max
   - `GT_FUNC`/`GE_FUNC`: compare against min
   - `BETWEEN`: compare lower bound against min, upper bound against max
   - `ISNULL_FUNC`: check that both min and max are NULL

6. **Handle strict inequalities**: For strict inequalities (`<`, `>`), the comparison is only valid if:
   - The key part is the last one used by the range scan, AND
   - The `NEAR_MAX`/`NEAR_MIN` flag is set in the range (indicating a strict bound)
   - For descending key parts, `NEAR_MIN` is checked instead of `NEAR_MAX` (and vice versa)

### 4.2 Key Comparison Lambda

```cpp
auto cmp = [field, &key_part](Item *cmp_item, const uchar *key) -> bool {
  field->set_key_image(key, key_part.length);
  Item_result res_type = item_cmp_type(
      field->result_type(), cmp_item->result_type());
  if (res_type == INT_RESULT) {
    Integer_value a(field->val_int(), field->is_unsigned());
    Integer_value b(cmp_item->val_int(), cmp_item->unsigned_flag);
    if (cmp_item->null_value) return false;
    return a == b;
  }
  bool ret = stored_field_cmp_to_item(current_thd, field, cmp_item) == 0;
  if (cmp_item->null_value) return false;
  return ret;
};
```

Integer comparisons are handled separately because `stored_field_cmp_to_item()` returns 0 for all integer comparisons regardless of values (it was designed for a different purpose). NULL comparisons are not handled because NULL cannot be reliably identified in the field's key image.

### 4.3 Redundancy Decision Logic

```cpp
bool can_remove = true;
if (cmp_with_max) {
  if (is_key_max_set && !key_max_is_null && !(range_flag & NO_MAX_RANGE)) {
    can_remove &= cmp(cmp_with_max, key_max);
  } else {
    can_remove &= false;  // max not available, cannot verify
  }
}
if (cmp_with_min) {
  if (is_key_min_set && !key_min_is_null && !(range_flag & NO_MIN_RANGE)) {
    can_remove &= cmp(cmp_with_min, key_min);
  } else {
    can_remove &= false;  // min not available, cannot verify
  }
}
if (can_remove) {
  *reduced = nullptr;  // Remove this predicate entirely
  return false;
}
```

### 4.4 IS NULL Handling

For `IS NULL` predicates on nullable fields:

```cpp
if (is_null && key_part.field->is_nullable()) {
  if (key_max_is_null && key_min_is_null) {
    *reduced = nullptr;  // Range guarantees NULL, remove predicate
    return false;
  }
}
```

### 4.5 BETWEEN Handling

For BETWEEN, both bounds must be constants; NOT BETWEEN is excluded:

```cpp
if (path_1 && op_type == Item_func::BETWEEN) {
  negated = down_cast<Item_func_between *>(func)->negated;
  Item *right_item_2 = func->arguments()[2]->real_item();  // upper bound
  if (!right_item_2->const_item()) {
    // Both bounds must be constants
    cmp_with_max = nullptr;
    cmp_with_min = nullptr;
  } else {
    cmp_with_max = right_item_2;    // upper bound vs key_max
    cmp_with_min = right_item;      // lower bound vs key_min
  }
}
```

### 4.6 Supported Operator Types

| Operator | Item_func type | Comparison |
|----------|---------------|------------|
| `col < const` | `LT_FUNC` | Compare against key_max |
| `col > const` | `GT_FUNC` | Compare against key_min |
| `col = const` | `EQ_FUNC` | Compare against both |
| `col <= const` | `LE_FUNC` | Compare against key_max |
| `col >= const` | `GE_FUNC` | Compare against key_min |
| `col BETWEEN low AND high` | `BETWEEN` | low vs key_min, high vs key_max |
| `col IS NULL` | `ISNULL_FUNC` | Check both bounds are NULL |

---

## 5. Execution Flow

### 5.1 Optimization Phase

```
JOIN::optimize()
  -> JOIN::finalize_table_conditions()
       For each non-const table:
         -> reduce_cond_for_table(thd, condition, null_extended, &condition)
              Recursively walk the condition tree:
              - AND list: process each child; remove nullptr (redundant) children
              - OR list: process each child with feature DISABLED
              - TRIG_COND: add inner tables to null_extended; recurse
              - FUNC_ITEM (comparison):
                  1. If rds_empty_redundant_check_in_range_scan is ON:
                     Check if the comparison is guaranteed by range scan
                  2. If EQ_FUNC:
                     Check if the equality is guaranteed by ref access
              If condition reduced to nullptr: remove predicate
         -> cache_const_expr() on remaining condition
       Cache constant expressions in HAVING clause
```

### 5.2 Range Scan Redundancy Check Algorithm (Detailed)

```
For a comparison predicate (e.g., col OP const):

1. Determine path_1 (col, const) or path_2 (const, col)
2. Get the field from the column side
3. Get the JOIN_TAB for the field's table
4. Verify: join_tab->type() == JT_RANGE
5. Verify: not MYSQL_TYPE_BIT field
6. Verify: not QS_DYNAMIC_RANGE
7. Verify: comparable_in_index() passes (except for IS NULL)
8. Get the range scan AccessPath
9. Verify: type == INDEX_RANGE_SCAN
10. Get used_fields bitmap from range scan (via get_fields_used())
11. Verify: field is in used_fields
12. Verify: num_ranges == 1 (single interval only)
13. Get QUICK_RANGE from the range scan
14. Locate the field's key part in used_key_part
15. Compute key_offset into key_min/key_max
16. Handle nullable fields (skip NULL byte)
17. Verify: not a prefix index (HA_PART_KEY_SEG not set)
18. Set up comparisons based on operator type
19. Handle strict inequality with NEAR_MAX/NEAR_MIN flags
20. Handle descending key parts (swap min/max)
21. Compare constant against key_min and/or key_max using cmp lambda
22. If both comparisons match: remove predicate
```

### 5.3 OR Clause Handling

Redundant condition removal is **disabled** for OR clauses because OR clauses produce disjoint ranges, and a comparison that matches one range may not match another:

```cpp
} else {  // Or list
  auto stored_variable = thd->variables.rds_empty_redundant_check_in_range_scan;
  thd->variables.rds_empty_redundant_check_in_range_scan = false;
  auto cleanup_guard = create_scope_guard([thd, stored_variable] {
    thd->variables.rds_empty_redundant_check_in_range_scan = stored_variable;
  });
  // ... process OR children with the feature temporarily disabled ...
}
```

This safeguard was initially missing (commit #1) and added as a bug fix (commit #2) to handle cases like `WHERE c=3 OR c>2` where the range optimizer builds a single range `c>2` with no max value.

---

## 6. System Variables

| Variable | Type | Scope | Default | Hint Updateable | Description |
|----------|------|-------|---------|-----------------|-------------|
| `rds_empty_redundant_check_in_range_scan` | bool | session | true | No | Remove redundant comparisons from SQL-layer filters when they are already guaranteed by the range index scan |

Usage:
```sql
-- Enable (default)
SET SESSION rds_empty_redundant_check_in_range_scan = ON;

-- Disable
SET SESSION rds_empty_redundant_check_in_range_scan = OFF;
```

---

## 7. EXPLAIN Output

### Traditional EXPLAIN

The "Extra" column may change from showing "Using where" to showing nothing (or fewer conditions):

**Before** (feature OFF):
```
type: range
Extra: Using where
```

**After** (feature ON):
```
type: range
Extra: (empty - condition fully removed)
```

### EXPLAIN FORMAT=TREE

The filter node may be removed entirely if all its conditions are redundant:

**Before** (feature OFF):
```
-> Filter: (a > 2)  (cost=X rows=X)
    -> Index range scan on t1 using idx over (2 < a)  (cost=X rows=X)
```

**After** (feature ON):
```
-> Index range scan on t1 using idx over (2 < a)  (cost=X rows=X)
```

### EXPLAIN FORMAT=JSON

The `attached_condition` and `index_condition` fields may be removed when the condition is proven redundant:

**Before** (feature OFF):
```json
"attached_condition": "(`test`.`t1`.`a` > 10)"
```

**After** (feature ON):
```json
// attached_condition field absent
```

---

## 8. Error Handling

### 8.1 NULL Handling

Comparisons involving NULL values are carefully handled:
- If the constant side of the comparison is NULL, the predicate is NOT removed (`can_remove = false`).
- For `IS NULL` predicates, the code checks that both `key_max_is_null` and `key_min_is_null` are true.
- The `key_max_is_null`/`key_min_is_null` flags are determined by checking the NULL byte (byte 0) of the key value.

### 8.2 Prefix Index

Prefix indexes are excluded from optimization because a prefix may match more rows than the full column value:

```cpp
if (!(key_part.flag & HA_PART_KEY_SEG)) {
  // ... proceed with comparison ...
}
```

### 8.3 Type Conversion

The `comparable_in_index()` function is used to verify that the comparison can be handled by the range optimizer. For equality conditions, this function has a permissive case for binary collations, so the code re-calls it with `UNKNOWN_FUNC` (not equality) to get a stricter check:

```cpp
(is_null ||
 comparable_in_index(cond, field, Field::imagetype::itRAW,
                     Item_func::UNKNOWN_FUNC, right_item))
```

### 8.4 BETWEEN Non-Constant Bug (Fixed in Commit #3)

The third argument of BETWEEN (`arguments()[2]`) was not checked for being a constant, allowing the optimization to incorrectly fire when the upper bound was a non-constant column reference. The fix adds:

```cpp
Item *right_item_2 = func->arguments()[2]->real_item();
if (!right_item_2->const_item()) {
  cmp_with_max = nullptr;
  cmp_with_min = nullptr;
}
```

This was discovered because queries like `WHERE t2.c1 BETWEEN 'true' AND t2.c2` produced incorrect results: the upper bound `t2.c2` is a column reference (not a constant), but the original code did not check for this.

### 8.5 OR Clause Disjoint Range Bug (Fixed in Commit #2)

The initial implementation did not disable the feature when processing OR children. For conditions like `WHERE c=3 OR c>2`, the range optimizer may build a single range `c>2` which has no max value. Processing `c=3` against this range would incorrectly try to compare 3 with an undefined max, leading to wrong results. The fix temporarily sets `rds_empty_redundant_check_in_range_scan = false` while processing OR children.

### 8.6 stored_field_cmp_to_item Dependency

The feature relies on the current behavior of `stored_field_cmp_to_item()` and effectively "canonizes" it. Changes to that function could break correctness of this feature. Integer and NULL comparisons are handled outside this function because it does not produce meaningful results for those types.

---

## 9. Test Coverage

### Test Files

| Test File | Lines | Description |
|-----------|-------|-------------|
| `mysql-test/t/range_scan_remove_redundant_condition.test` | 697 | Main test suite |
| `mysql-test/r/range_scan_remove_redundant_condition.result` | - | Main expected results |
| `mysql-test/r/range_scan_remove_redundant_condition.result-pq` | - | Parallel query variant |
| `mysql-test/suite/dstore_main/t/range_scan_remove_redundant_condition-dstore.test` | - | DStore variant |
| `mysql-test/suite/dstore_main/r/range_scan_remove_redundant_condition-dstore.result` | - | DStore expected results |

### Test Categories

| Category | Test Cases |
|----------|-----------|
| Feature ON/OFF | Compare EXPLAIN and results with feature ON vs OFF |
| Simple comparison | `col > const`, `col < const`, `col = const`, `col >= const`, `col <= const` |
| BETWEEN | `col BETWEEN const AND const`, with type conversions |
| IS NULL | `col IS NULL` with nullable fields |
| Type conversion | String-to-int, decimal comparisons |
| Multi-column index | `WHERE a = 2 AND b < 2` with index on (a, b) |
| Column order | `col OP const` vs `const OP col` |
| Covering index | ICP interaction, `rds_empty_redundant_check_in_range_scan` ON/OFF |
| Descending index | DESC key parts with NEAR_MIN/NEAR_MAX |
| OR clause | Verify conditions are NOT removed for OR (bug fix #2) |
| Outer join | null_extended tables, FOUND_MATCH trigger conditions |
| Ref access | Equality predicate removal for ref/eq_ref access |
| Non-constant BETWEEN | BETWEEN with non-constant third argument (bug fix #3) |

### Interaction with Offset Pushdown

The `rds_empty_redundant_check_in_range_scan` feature is a prerequisite for offset pushdown with covering indexes. When ICP is not active (covering index), the condition remains at the SQL layer. By removing redundant conditions, the SQL layer has no filter left, allowing offset pushdown to activate.

---

## 10. Limitations

1. **Only INDEX_RANGE_SCAN access**: Does not work with INDEX_MERGE, ROWID_INTERSECTION, ROWID_UNION, INDEX_SKIP_SCAN, or GROUP_INDEX_SKIP_SCAN.

2. **Single range interval only**: Requires `num_ranges == 1`; disjoint ranges from OR clauses are excluded.

3. **No MYSQL_TYPE_BIT fields**: BIT type fields are excluded due to comparison complexity.

4. **No prefix indexes**: Prefix indexes (`HA_PART_KEY_SEG`) are excluded because the prefix may match more rows than the full column value.

5. **No dynamic range**: `QS_DYNAMIC_RANGE` is not supported because the range is determined at execution time.

6. **NULL comparison limitations**: Comparisons to NULL values are not handled because NULL cannot be reliably identified in the field's key image.

7. **Strict inequalities only for last key part**: For strict inequalities (`<`, `>`), the predicate can only be removed if the key part is the last one used by the range scan and the `NEAR_MAX`/`NEAR_MIN` flag is set.

8. **No NOT BETWEEN**: NOT BETWEEN produces disjoint intervals and is not supported.

9. **stored_field_cmp_to_item dependency**: The feature relies on the current behavior of `stored_field_cmp_to_item()`. Changes to that function could break correctness.

10. **Integer special case**: Integer comparisons are handled outside `stored_field_cmp_to_item()` because that function returns 0 for all integer comparisons regardless of values.

---

## 11. Implementation Order

1. **System variable** - Add `rds_empty_redundant_check_in_range_scan` to `sql/system_variables.h` and `sql/sys_vars.cc`.

2. **Helper function** - Add `is_column_then_constant()` inline function to `sql/sql_optimizer.cc`.

3. **Core algorithm** - Extend `reduce_cond_for_table()` with the range scan redundancy check block, including:
   - Field/constant identification (path_1/path_2)
   - Range scan verification (JT_RANGE, not dynamic, comparable in index)
   - Key part location and key_offset computation
   - Comparison lambda (INT special case, NULL handling)
   - Operator-specific comparison setup (LT/GT/EQ/LE/GE/BETWEEN/ISNULL)
   - Strict inequality handling (NEAR_MAX/NEAR_MIN)
   - Descending key part handling (swap min/max)
   - Redundancy decision and predicate removal

4. **OR clause safeguard** - Temporarily disable the feature when processing OR children by saving/restoring the session variable.

5. **Trigger condition handling** - Add `FOUND_MATCH` inner tables to `null_extended` map in the `TRIG_COND_FUNC` handler.

6. **Test suite** - Port MTR test cases and update all affected result files.

7. **Bug fix #2 (OR clause disjoint range)** - Verify that OR clause handling correctly disables the feature to prevent incorrect comparisons with disjoint ranges.

8. **Bug fix #3 (BETWEEN non-constant third argument)** - Add `const_item()` check for BETWEEN's third argument (`arguments()[2]`) to prevent applying the optimization when the upper bound is a non-constant.
