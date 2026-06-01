# Range Scan Optimization Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

When MySQL uses an index range scan to access a table, the WHERE clause may contain conditions that are already fully satisfied by the range scan bounds. For example, if the range optimizer determines that `col BETWEEN 5 AND 10` is the scan range, a WHERE condition `col = 7` is redundant because the range scan already guarantees `col` is between 5 and 10. However, MySQL still evaluates such conditions at the SQL layer, causing unnecessary CPU overhead.

More critically, when a table is discovered to be constant (e.g., after const table optimization), sargable conditions referencing that table should have their const-item flags refreshed. Without this refresh, the range optimizer may build suboptimal ranges, scanning far more index cells than necessary.

### 1.2 Solution

Two complementary optimizations:

1. **Const-item refresh**: After discovering const tables, refresh the `const_item()` flag on sargable condition arguments. This allows the range optimizer to build tighter ranges when expressions like `t2.a >= t1.b - 20` become constant after `t1` is resolved.

2. **Redundant condition removal**: When a table uses an index range scan, compare the WHERE clause predicates against the range scan's min/max key values. If a predicate is provably satisfied by the range bounds, remove it from the filtered condition to avoid redundant evaluation.

### 1.3 Performance Impact

The TPC-C "stocklevel" transaction showed a significant improvement:
- Before: `Index range scan on order_line1 over (ol_w_id = 23 AND ol_d_id = 3 AND ol_o_id < 3001)` -- cost 11399, rows 56602
- After: `Index range scan on order_line1 over (ol_w_id = 23 AND ol_d_id = 3 AND 2981 <= ol_o_id < 3001)` -- cost 43.1, rows 209
- TPMC improvement: 140k to 730k

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| Core optimization (`sql/sql_optimizer.cc`) | 1 | ~68 net new |
| System variable | 1 (sys_vars.cc) + 1 (system_variables.h) | ~10 |
| MTR tests | 2 test files + result files | ~250 |
| **Total** | ~5 | ~328 |

---

## 2. New Files

No new source files are created. All changes are modifications to existing files.

---

## 3. Modifications to Existing Files

### 3.1 `sql/sql_optimizer.cc`

**Change 1: Const-item refresh in `update_ref_and_keys()`**

After const table discovery, refresh `const_item()` flags on sargable key parts:

```c++
// In update_ref_and_keys(), after const table optimization:
// Refresh const_item() on sargable arguments
for (uint kp = 0; kp < s->key_parts; kp++) {
  Key_part *key_part = s->key_part + kp;
  if (key_part->val) {
    key_part->val->update_used_tables();
    // After update_used_tables(), const_item() may now return true
    // for expressions that reference only const tables
  }
}
```

This ensures that when `t1` becomes const, expressions like `t1.b - 20` are recognized as constant by the range optimizer.

**Change 2: Redundant condition removal in `reduce_cond_for_table()`**

A new static function `reduce_cond_for_table()` walks the WHERE condition tree and removes predicates that are provably satisfied by the range scan bounds. It is called from the condition simplification loop in the optimizer:

```c++
// Called from the condition simplification pass in
// JOIN::optimize() / make_join_query_block()
for (uint i = const_tables; i < tables; i++) {
  Item *condition = best_ref[i]->condition();
  table_map null_extended = query_block->outer_join &
                            ~best_ref[i]->table_ref->map();
  if (reduce_cond_for_table(thd, condition, null_extended, &condition))
    return true;
  best_ref[i]->set_condition(condition);
}
```

### 3.2 `sql/system_variables.h`

```c++
// New member in System_variables:
bool rds_empty_redundant_check_in_range_scan;
```

### 3.3 `sql/sys_vars.cc`

```c++
static Sys_var_bool Sys_var_rds_empty_redundant_check_in_range_scan(
    "rds_empty_redundant_check_in_range_scan",
    "Remove redundant WHERE conditions that are already satisfied "
    "by index range scan bounds",
    SESSION_VAR(rds_empty_redundant_check_in_range_scan),
    CMD_LINE(OPT_ARG), DEFAULT(true));
```

### 3.4 Test Files

- `mysql-test/t/range_bugs.test` + `mysql-test/r/range_bugs.result`: Core test for const-item refresh
- `mysql-test/t/range_scan_remove_redundant_condition.test` + result: Comprehensive test for redundant condition removal

---

## 4. Core Data Structures

No new data structures are introduced. The optimization operates on existing MySQL structures:

### 4.1 QUICK_RANGE

The range optimizer produces `QUICK_RANGE` objects containing `min_key` and `max_key` byte arrays. These represent the lower and upper bounds of the index range scan. The optimization compares WHERE clause constants against these bounds.

```c++
// From sql/range_optimizer/range_optimizer.h:
class QUICK_RANGE {
  uchar *min_key;      // Lower bound key value
  uchar *max_key;      // Upper bound key value
  uint16 flag;         // NEAR_MIN, NEAR_MAX, NO_MIN_RANGE, NO_MAX_RANGE
  key_part_map min_keypart_map;
  key_part_map max_keypart_map;
  uint min_length;
  uint max_length;
};
```

### 4.2 KEY_PART

Describes a key part within an index. Used to locate the position of a specific column's value within `min_key`/`max_key`:

```c++
// From sql/range_optimizer/range_optimizer.h:
struct KEY_PART {
  Field *field;        // The field for this key part
  uint offset;         // Offset in key buffer
  uint length;         // Length of key part
  uint store_length;   // Length including length bytes
  key_part_map key_part_map;
  uint flag;           // HA_REVERSE_SORT, HA_PART_KEY_SEG, etc.
};
```

### 4.3 AccessPath::INDEX_RANGE_SCAN

The access path for index range scan, which contains the range information used by the optimization:

```c++
// In sql/join_optimizer/access_path.h:
struct IndexRangeScanAccessPath {
  QUICK_SELECT_I *quick;
  TABLE *table;
  ha_rows num_rows;
  // From the QUICK_RANGE_SELECT:
  uint num_ranges;           // Number of QUICK_RANGE objects
  QUICK_RANGE **ranges;      // Array of range descriptors
  KEY_PART *used_key_part;   // Key parts used in range
  uint num_used_key_parts;   // Number of key parts used
};
```

---

## 5. Execution Flow

### 5.1 Const-Item Refresh

```
update_ref_and_keys() (sql/sql_optimizer.cc)
  |
  v
After const table discovery:
  For each sargable key_part with a val (value expression):
    key_part->val->update_used_tables()
      // This refreshes the const_item() flag
      // After this call, expressions like t1.b - 20
      // where t1 is now const will return const_item() == true
  |
  v
Range optimizer sees more sargable conditions as constant
  -> Builds tighter ranges (e.g., 2981 <= ol_o_id < 3001
     instead of ol_o_id < 3001)
```

### 5.2 Redundant Condition Removal

```
reduce_cond_for_table(thd, cond, null_extended, &reduced)
  |
  +-- COND_ITEM (AND):
  |     Process each child:
  |       If child reduces to nullptr (always true): remove from AND list
  |       If AND list becomes empty: *reduced = nullptr (entire condition true)
  |       If AND list has 1 element: *reduced = that element
  |
  +-- COND_ITEM (OR):
  |     Deactivate redundant check for OR children:
  |       saved = rds_empty_redundant_check_in_range_scan
  |       rds_empty_redundant_check_in_range_scan = false
  |       Process each child
  |       Restore variable
  |     If any child reduces to nullptr: entire OR is true
  |
  +-- TRIGCOND_FUNC:
  |     If FOUND_MATCH: add inner tables to null_extended
  |     Process the trigger's condition argument
  |
  +-- FUNC_ITEM (comparison operators):
        If rds_empty_redundant_check_in_range_scan is ON
        AND not null_extended
        AND operator is LT, GT, EQ, LE, GE, BETWEEN, or ISNULL:
          |
          v
        Check if condition is column-then-constant:
          path_1: left=field, right=constant
          path_2: left=constant, right=field
          |
          v
        If field's table uses index range scan (JT_RANGE):
          Get QUICK_RANGE from join_tab->range_scan()
          If num_ranges == 1 AND field is used in range:
            |
            v
          Locate field's key part in range bounds:
            Calculate key_offset by summing key_part lengths
            |
            v
          Compare condition value with range bounds:
            For EQ: compare with both min_key and max_key
            For LT/LE: compare with max_key
            For GT/GE: compare with min_key
            For BETWEEN: compare lower with min, upper with max
            For ISNULL: check both bounds are NULL
            |
            v
          If comparison succeeds (condition is redundant):
            *reduced = nullptr  // Remove condition
            return false
```

### 5.3 Key Comparison Logic

The core comparison function:

```c++
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

The comparison sets the field's key image from the range bound, then compares the field value with the constant item. For integers, it uses direct integer comparison. For other types, it uses `stored_field_cmp_to_item()`.

### 5.4 Strict Inequality Handling

When the condition uses strict inequality (< or >) but the range has inclusive bounds (<= or >=), the condition cannot be removed unless the NEAR_MAX/NEAR_MIN flag is set:

```c++
bool is_strict_inequality = (op_type == Item_func::LT_FUNC ||
                              op_type == Item_func::GT_FUNC);
bool valid_cmp = is_strict_inequality
    ? (last_key_part
        ? (descending_key_part
            ? (range_flag & NEAR_MIN)   // DESC index: check NEAR_MIN
            : (range_flag & NEAR_MAX))  // ASC index: check NEAR_MAX
        : false)                          // Not last key part: cannot remove
    : true;                               // Non-strict: always valid
```

For DESC key parts, min and max comparisons are swapped because the storage order is reversed.

### 5.5 BETWEEN Condition Handling

For BETWEEN conditions, both bounds must be constant:

```c++
if (path_1 && op_type == Item_func::BETWEEN) {
  negated = down_cast<Item_func_between *>(func)->negated;
  Item *right_item_2 = func->arguments()[2]->real_item();
  if (!right_item_2->const_item()) {
    // Third argument is not constant, cannot optimize
    cmp_with_max = nullptr;
    cmp_with_min = nullptr;
  } else {
    cmp_with_max = right_item_2;  // Upper bound of BETWEEN
    cmp_with_min = right_item;    // Lower bound of BETWEEN
  }
}
```

**Bug fix (commit #2)**: The original implementation only checked that `arguments()[0]` and `arguments()[1]` were constants for BETWEEN, but failed to verify `arguments()[2]`. This allowed the optimization to incorrectly remove BETWEEN conditions with non-constant third arguments, producing wrong query results.

---

## 6. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `rds_empty_redundant_check_in_range_scan` | bool | session | true | Enable removal of WHERE conditions already satisfied by index range scan bounds |

---

## 7. EXPLAIN Output

The optimization does not add new EXPLAIN output. However, it may change the plan in observable ways:

- The `filtered` column may show higher percentages (fewer rows need post-filtering)
- The `Extra` column may show fewer `Using where` conditions
- The `rows` estimate may decrease because tighter ranges are built

Example change visible in EXPLAIN:
```
Before: Index range scan on t2 using a over (NULL < a < 100)
After:  Index range scan on t2 using a over (80 <= a < 100)
```

---

## 8. Error Handling

- The optimization is purely a condition simplification; it cannot cause runtime errors
- If any comparison fails (e.g., type mismatch), the condition is kept (not removed), ensuring correctness
- NULL comparisons are handled conservatively: if `cmp_item->null_value` is true after comparison, the condition is kept
- The `OR` clause disables redundant checking because OR produces disjoint ranges, making min/max comparison invalid

### Bug Fix: Incorrect BETWEEN Handling

The second commit (ea82404780ae) fixes a correctness issue:
- **Problem**: BETWEEN conditions with non-constant third argument (`arguments()[2]`) were incorrectly optimized away
- **Root cause**: Only `arguments()[0]` and `arguments()[1]` were checked for const-ness
- **Fix**: Added explicit check that `arguments()[2]->const_item()` is true before applying the optimization to BETWEEN
- **Test case**: `t2.c1 BETWEEN 'true' AND t2.c2` where `t2.c2` is not constant -- must not be removed

---

## 9. Memory Management

No new memory allocations. The optimization operates on existing objects in the Item tree and reads from existing QUICK_RANGE arrays. The `set_key_image()` call temporarily modifies a Field's buffer but the optimizer's column map save/restore mechanism (`dbug_tmp_use_all_columns` / `dbug_tmp_restore_column_maps`) ensures this does not interfere with normal execution.

---

## 10. Test Coverage

| Test File | Description |
|-----------|-------------|
| `mysql-test/t/range_bugs.test` | Tests const-item refresh: `t2.a >= t1.b - 20 AND t2.a < t1.b` after t1 becomes const |
| `mysql-test/t/range_scan_remove_redundant_condition.test` | Comprehensive tests for redundant condition removal covering: EQ, LT, GT, LE, GE, BETWEEN, IS NULL, DESC indexes, prefix indexes, multiple ranges, OR clauses, outer joins |
| `mysql-test/suite/dstore_main/t/range_scan_remove_redundant_condition-dstore.test` | DStore variant of the main test |

### Key Test Scenarios

1. **Const-item refresh**: `SELECT * FROM t1, t2 WHERE t1.a=100 AND t2.a>=t1.b-20 AND t2.a<t1.b` -- verifies tighter range after t1 resolved
2. **Simple equality**: `col = constant` with matching range min=max
3. **Inequality**: `col > constant` when range min >= constant
4. **BETWEEN**: `col BETWEEN a AND b` when range covers [a, b]
5. **IS NULL**: `col IS NULL` when range bounds are NULL
6. **DESC index**: Same comparisons but with descending key parts
7. **Prefix index**: Skipped (HA_PART_KEY_SEG check)
8. **OR clause**: Redundant check disabled inside OR branches
9. **Multiple ranges**: When num_ranges > 1, optimization is not applied
10. **Dynamic range**: When use_quick == QS_DYNAMIC_RANGE, optimization is not applied
11. **Non-constant BETWEEN third argument**: Bug fix verification

---

## 11. Limitations

- Only applies to single-range index range scans (`num_ranges == 1`); disjoint ranges from OR conditions are not handled
- Does not apply to dynamic range scans (`QS_DYNAMIC_RANGE`)
- Does not handle prefix indexes (`HA_PART_KEY_SEG`)
- Does not remove conditions from OR clauses (only AND clauses)
- Only supports comparison operators: =, <, >, <=, >=, BETWEEN, IS NULL
- Binary collation equality has a special permissive case in `comparable_in_index()` that prevents removal of equality conditions
- BIT type fields are excluded
- NULL comparisons inside conditions are conservatively kept (cannot determine if field is NULL from key bounds)
- The optimization is session-controllable and can be disabled if it causes regressions

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **System variable** (`system_variables.h`, `sys_vars.cc`) -- Add `rds_empty_redundant_check_in_range_scan` session variable
2. **Const-item refresh** (`sql/sql_optimizer.cc`) -- Refresh `const_item()` flags on sargable key parts after const table discovery
3. **Redundant condition removal** (`sql/sql_optimizer.cc`) -- Implement `reduce_cond_for_table()` with full operator support
4. **Integration** (`sql/sql_optimizer.cc`) -- Call `reduce_cond_for_table()` from the condition simplification pass
5. **Bug fix** -- Ensure BETWEEN third argument const-ness check
6. **Test suite** -- Port MTR test cases
