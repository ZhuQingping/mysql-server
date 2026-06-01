# IN-List to Subquery Conversion - Feature Specification

> Version: 3.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main commit: 8eb87e1268aa537fed7f7e5f404686b1ea67a249
> Author: zhangxinyu
> Commits: 5 (1 main + 4 follow-ups)
> Files changed: 31, +4,618 / -14

---

## 1. Overview

### Problem Statement

MySQL evaluates `IN (v1, v2, ..., vN)` predicates using a linear scan through
the value list. When N is large, this is inefficient because:

- The optimizer cannot use index lookups or semi-join strategies on the value
  list itself
- The value list is evaluated item by item without leveraging database access
  paths
- Large IN-lists cause long parse/resolve times and poor execution plans
- The range optimizer creates separate range intervals for each value, which
  becomes expensive at scale

### Solution

Transform large IN-predicates into IN-subqueries over a temporary table. The
temporary table contains the IN-list values (with duplicates removed) and has
an auto-created distinct key, enabling the optimizer to use index-based access
methods (ref, eq_ref, range, semi-join) for the subquery. This transformation
is controlled by the session variable `rds_in_predicate_conversion_threshold`.

### Transformation Example

```sql
-- Before transformation:
SELECT * FROM t1 WHERE t1.a IN (1, 2, 3, ..., 1000)

-- After transformation:
SELECT * FROM t1 WHERE t1.a IN (
  SELECT * FROM <in_predicate_2>  -- temporary table with values 1..1000
)
-- The optimizer can now use semi-join, ref access, etc.
```

For row-value IN-predicates:

```sql
-- Before:
SELECT * FROM t1 WHERE (t1.a, t1.b) IN ((1,'x'), (2,'y'), ...)

-- After:
SELECT * FROM t1 WHERE (t1.a, t1.b) IN (
  SELECT * FROM <in_predicate_2>  -- temp table with (a,b) columns
)
```

### Execution Model

The transformation occurs during `Item_func_in::fix_fields()`, in the
resolution phase. If the IN-predicate is eligible and the threshold is met,
the predicate is replaced with an `Item_in_subselect` wrapping a subquery over
a temporary table. If anything goes wrong during transformation, the function
returns the original `Item_func_in`, and the query proceeds normally.

### Code Volume

| Metric | Value |
|--------|-------|
| Commits | 5 (1 main + 2 feature extensions + 2 bugfixes) |
| Files changed (main commit) | 31 |
| Lines added (main commit) | +4,618 |
| Lines removed (main commit) | -14 |
| New test files | 5 |

---

## 2. New Files

No new source files are created. All changes are modifications to existing
files.

### 2.1 Test Files

| File | Lines | Purpose |
|------|-------|---------|
| `mysql-test/include/explain_and_query.inc` | 45 | Helper for EXPLAIN in multiple formats and query execution |
| `mysql-test/t/in_predicate_to_in_subs_basic.test` | 38 | Basic functionality: on/off, threshold values |
| `mysql-test/t/in_predicate_to_in_subs_part1.test` | 468 | Scalar and row IN-lists, type handling, index usage |
| `mysql-test/t/in_predicate_to_in_subs_part2.test` | 519 | Advanced: subqueries, views, PS protocol |
| `mysql-test/t/in_predicate_to_in_subs_bugs.test` | 59 | Bug regression tests |

Result files:
- `mysql-test/r/in_predicate_to_in_subs_basic.result` (38 lines)
- `mysql-test/r/in_predicate_to_in_subs_part1.result` (2,165 lines)
- `mysql-test/r/in_predicate_to_in_subs_part2.result` (642 lines)
- `mysql-test/r/in_predicate_to_in_subs_bugs.result` (67 lines)

---

## 3. Modifications to Existing Files

### 3.1 `sql/item_cmpfunc.cc` (Main Implementation, ~450 new lines)

New static functions:

| Function | Purpose |
|----------|---------|
| `convert_in_list_to_temp_table()` | Creates a temporary TABLE from IN-list values with auto key |
| `is_args_enum_type_valid_value()` | Validates ENUM values against the field's type library |
| `is_const_or_param_with_same_type()` | Checks all IN-list items are constants/params with matching result types |
| `is_args_in_key_parts()` | Checks if at least one IN-predicate argument has a usable index |
| `can_index_used_for_ref()` | Checks if a field's key can be used for ref access |
| `add_table_and_select_list()` | Fills Table_ref and SELECT list into the generated Query_block |

New member functions in `Item_func_in`:

| Function | Purpose |
|----------|---------|
| `is_applicable_to_convert_into_subq()` | Eligibility check for conversion |
| `in_predicate_to_in_subs_transformer()` | Main transformation entry point |
| `fill_transform_tmp_table()` | Populates the temporary table with IN-list values |

Modified function:

```c++
bool Item_func_in::fix_fields(THD *thd, Item **ref) {
  if (Item_func_opt_neg::fix_fields(thd, ref)) return true;
  update_not_null_tables();
  if (thd->lex->current_query_block()->resolve_place ==
      Query_block::RESOLVE_CONDITION) {
    *ref = in_predicate_to_in_subs_transformer(thd);
  }
  return false;
}
```

### 3.2 `sql/item_cmpfunc.h`

New method declarations in `Item_func_in`:

```c++
bool is_applicable_to_convert_into_subq(THD *thd,
    Opt_trace_object *in_predicate_work);
Item *in_predicate_to_in_subs_transformer(THD *thd);
bool fill_transform_tmp_table(THD *thd, TABLE *tmp_table);
```

### 3.3 `sql/table.h`

New member and methods in `Table_ref`:

```c++
Item *transformed_in_predicate{nullptr};

bool is_transformed_in_predicate() const {
  return transformed_in_predicate != nullptr;
}
void set_transformed_in_predicate(Item *in_predicate);
Item *get_transformed_in_predicate() const;
```

The `is_transformed_in_predicate()` flag is used to:
- Skip `optimize_derived()` on the pseudo-table
- Show "IN-list converted" in EXPLAIN output
- Properly handle the table reference in `is_dependent_in_subquery()`

### 3.4 `sql/sql_lex.h`

New member in `LEX`:

```c++
bool disable_in_predicate_transform_to_subq{false};
```

Set to `true` when transformation fails during execution, causing re-prepare
to skip the transformation.

### 3.5 `sql/sql_class.cc`

Reset the disable flag for new statements:

```c++
lex->disable_in_predicate_transform_to_subq = false;
```

### 3.6 `sql/system_variables.h`

New session variable:

```c++
uint rds_in_subquery_conversion_threshold;
```

### 3.7 `sql/sys_vars.cc`

New system variable definition:

```c++
static Sys_var_uint Sys_in_subquery_conversion_threshold(
    "rds_in_predicate_conversion_threshold",
    "The minimum number of scalar elements in the value list of "
    "IN predicate that triggers its conversion to IN subquery. Set to "
    "0 to disable the conversion.",
    HINT_UPDATEABLE SESSION_VAR(rds_in_subquery_conversion_threshold),
    CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, UINT_MAX), DEFAULT(0),
    BLOCK_SIZE(1));
```

### 3.8 `sql/opt_explain.cc`

Add EXPLAIN "Extra" column tag for converted IN-predicates:

```c++
if (table_ref && table_ref->is_transformed_in_predicate()) {
  if (push_extra(ET_IN_LIST_CONVERTED)) return true;
}
```

### 3.9 `sql/opt_explain_format.h`

New enum value: `ET_IN_LIST_CONVERTED`

### 3.10 `sql/opt_explain_traditional.cc` and `sql/opt_explain_json.cc`

New EXPLAIN Extra string: `"IN-list converted"` for `ET_IN_LIST_CONVERTED`

### 3.11 Other Modified Files

- `sql/sql_base.cc`: Handle temp table open/close for converted IN-predicates
- `sql/sql_optimizer.cc`: Handle pseudo-table during optimization
- `sql/sql_resolver.cc`: Properly resolve the generated subquery's query block
- `sql/sql_union.cc`: Handle the generated subquery within UNION execution
- `sql/sql_prepare.cc` / `sql/sql_prepare.h`: PS protocol support

---

## 4. Core Data Structures

### 4.1 Temporary Table Structure

The temporary table created by `convert_in_list_to_temp_table()`:

| Property | Value |
|----------|-------|
| Columns | One column per element of args[0] (matching types) |
| Key | One auto-created key covering all visible fields |
| Key flags | `HA_NULL_PART_KEY` set for nullable columns |
| Key visibility | `keys_in_use` and `visible_indexes` set |
| Field flags | `key_start`, `part_of_key`, `PART_KEY_FLAG` set for all fields |
| Table type | `NON_TRANSACTIONAL_TMP_TABLE` |
| Creation | `create_tmp_table()` with `skip_create_table = true` |

### 4.2 Table_ref for the Pseudo-Table

| Property | Value |
|----------|-------|
| table_name | `<in_predicate_N>` (N = select_number) |
| db_name | Current DB, or `<in_predicate_db>` for information_schema |
| table | Direct pointer to the TABLE object |
| transformed_in_predicate | Points to the original `Item_func_in` |
| Privileges | SELECT_ACL |

### 4.3 Generated Subquery Structure

```c++
Query_block *sq_select:
  - Created with lex->new_query(current_select)
  - Contains one Table_ref (the pseudo-table)
  - SELECT list: Item_asterisk (SELECT *)
  - parsing_place: same as current query block's
  - context: properly set for name resolution
```

### 4.4 Item_in_subselect

```c++
Item_in_subselect *in_subs:
  - Created with args[0] (left side of IN) and sq_select
  - If negated: in_subs->value_transform = BOOL_NEGATED
  - If need_apply_is_true: in_subs->apply_is_true()
  - fix_fields() is called, which may trigger semi-join flattening
```

---

## 5. Execution Flow

### 5.1 Overall Flow

```
Item_func_in::fix_fields()
  -> if resolve_place == RESOLVE_CONDITION:
      *ref = in_predicate_to_in_subs_transformer(thd)

in_predicate_to_in_subs_transformer(thd):
  1. Check rds_in_predicate_conversion_threshold != 0
  2. is_applicable_to_convert_into_subq() - eligibility check
  3. Push Dummy_error_handler (suppress errors)
  4. convert_in_list_to_temp_table() - create temp table
  5. lex->new_query() - create subquery Query_block
  6. fill_transform_tmp_table() - populate values (if regular query)
  7. add_table_and_select_list() - set up FROM and SELECT
  8. new Item_in_subselect() - create the subquery item
  9. Set negation/apply_is_true flags
  10. in_subs->fix_fields() - resolve the subquery
  11. If success: release cleanup guard, return in_subs
  12. If failure at any step: cleanup_and_restore_guard cleans up,
      return this (original Item_func_in)
```

### 5.2 Eligibility Check (is_applicable_to_convert_into_subq)

The following conditions are checked in order:

1. **Disable flag**: If `disable_in_predicate_transform_to_subq` is set, skip
   (for re-prepare after previous failure).
2. **OR in WHERE**: If the WHERE clause is an OR condition, skip (cannot use
   semi-join, would degrade performance).
3. **IS NULL / IS NOT NULL on whole predicate**: Skip because
   `x IN (...) IS NULL` is not equivalent to `EXISTS(SELECT ...) IS NULL`.
4. **Threshold**: `values_count >= rds_in_subquery_conversion_threshold`
   (where `values_count = (arg_count - 1) * cols` for row-value lists).
5. **Statement type**: Only SELECT, INSERT SELECT, REPLACE SELECT.
6. **Arena type**: Only regular statements or prepared statement prepare
   (not stored procedures or triggers).
7. **NOT IN**: Not supported (negated = true skips conversion).
8. **Constant/param check**: All IN-list values must be constants or
   parameters with the same result type as args[0].
9. **Index availability**: At least one argument must have a usable index
   key part.

### 5.3 Temporary Table Creation (convert_in_list_to_temp_table)

1. Collect field items from `args[0]` (left side of IN).
2. Create `Temp_table_param` with `skip_create_table = true`.
3. Call `count_field_types()` to determine column types.
4. Call `create_tmp_table()` with `TMP_TABLE_ALL_COLUMNS`.
5. Register key for each visible field: set `key_start`, `part_of_key`,
   `PART_KEY_FLAG`, and `HA_NULL_PART_KEY` for nullable columns.
6. Make key visible: set `keys_in_use` and `visible_indexes`.
7. Set table type to `NON_TRANSACTIONAL_TMP_TABLE`.

### 5.4 Fill Temporary Table (fill_transform_tmp_table)

1. If `is_created()`, skip (PS re-execution path).
2. Reset table status: `set_not_started()`.
3. Instantiate the table: `instantiate_tmp_table()`.
4. Insert each IN-list value row by row:
   - For row values: save each column element via `save_in_field()`
   - For scalar: save single value
   - Special ENUM type validation via `is_args_enum_type_valid_value()`
   - Ignore duplicate key errors (`HA_ERR_FOUND_DUPP_KEY`)
5. Restore key info for PS protocol.
6. On error: `ask_to_reprepare(thd)` and set
   `disable_in_predicate_transform_to_subq = true`.

### 5.5 Subquery Construction (add_table_and_select_list)

1. Create `Table_ident` with generated name `<in_predicate_N>`.
2. For information_schema DB, use `<in_predicate_db>` as database name.
3. Add table to Query_block via `add_table_to_list()`.
4. Set `values_tab->table = tmp_table` and `set_transformed_in_predicate()`.
5. Add as joined table.
6. Set name resolution context.
7. Add `Item_asterisk` (SELECT *) to select list.

---

## 6. System Variables

### rds_in_predicate_conversion_threshold

| Property | Value |
|----------|-------|
| Type | `uint` |
| Scope | SESSION (hint-updateable) |
| Default | 0 (disabled) |
| Valid range | 0 to UINT_MAX |
| Block size | 1 |
| Command line | `--rds_in_predicate_conversion_threshold=N` |
| Description | The minimum number of scalar elements in the IN-list to trigger conversion. Set to 0 to disable. |

For row-value IN-predicates, the scalar element count is:
`values_count = (arg_count - 1) * cols_per_row`

---

## 7. EXPLAIN Output

### Traditional EXPLAIN

A new Extra tag `IN-list converted` is shown for the temporary table row:

```
| id | select_type | table             | type   | key                 | Extra              |
|----|-------------|-------------------|--------|---------------------|--------------------|
|  1 | SIMPLE      | t1                | ALL    | PRIMARY             | NULL               |
|  1 | SIMPLE      | <in_predicate_2>  | eq_ref | <auto_distinct_key> | IN-list converted  |
```

### JSON EXPLAIN

Same tag appears as `"IN-list converted"`.

### Optimizer Trace

```json
{
  "in_to_subquery_conversion": {
    "item": "t1.a in (1,2,3,...)",
    "steps": [ ... ],
    "done": true
  }
}
```

When conversion is not done, a `reason` field is included:

```json
{
  "in_to_subquery_conversion": {
    "done": false,
    "reason": "In-list threshold is too big"
  }
}
```

---

## 8. Error Handling

### 8.1 Transformation Failure (Static)

If the transformation fails at any step during `fix_fields()`, the function
returns `this` (the original `Item_func_in`), and the IN-predicate is
evaluated normally. All allocations are cleaned up via scope guards:

```c++
auto cleanup_and_restore_guard = create_scope_guard([...]) {
  if (is_add_table_succeed) {
    sq_select->master_query_expression()->exclude_level();
  } else {
    if (sq_select)
      sq_select->master_query_expression()->exclude_level();
    if (tmp_table) {
      close_tmp_table(tmp_table);
      free_tmp_table(tmp_table);
    }
  }
};
```

A `Dummy_error_handler` suppresses errors during transformation so that
failure is transparent to the user.

### 8.2 Fill Failure (Runtime)

If `fill_transform_tmp_table()` fails during execution:

1. Calls `ask_to_reprepare(thd)` to trigger a re-prepare
2. Sets `disable_in_predicate_transform_to_subq = true`
3. The re-prepare evaluates the IN-predicate normally without conversion

### 8.3 ENUM Value Validation

If an IN-list value is not valid for an ENUM column, the fill fails and
triggers re-prepare without transformation.

### 8.4 NULL Semantics

The transformation preserves NULL semantics:
- The temporary table includes NULL values
- The key is created with `HA_NULL_PART_KEY` flag for nullable columns
- The optimizer uses `ref_or_null` access when appropriate
- `x IN (..., NULL, ...)` evaluates correctly to NULL when x is not matched

### 8.5 Bug Fixes

| Bugfix | Issue | Resolution |
|--------|-------|------------|
| #2 | Temporary table support | Added support for IN-predicate conversion when the left-side table is a temporary table |
| #3 | Crash with PS protocol | Fixed crash when IN-predicate to temporary table conversion was used with prepared statement protocol; temporary table was not properly re-filled on re-execution |
| #4 | Max tuple length test fix | Adjusted test cases after the max tuple length was changed to 950 |
| #5 | AT pipeline test fix | Additional test adjustment for AT pipeline |

---

## 9. Test Coverage

### 9.1 Test Files

| File | Lines | Description |
|------|-------|-------------|
| `in_predicate_to_in_subs_basic.test` | 38 | Basic on/off, threshold check, variable validation |
| `in_predicate_to_in_subs_part1.test` | 468 | Scalar and row IN-lists, type handling, index usage |
| `in_predicate_to_in_subs_part2.test` | 519 | Subqueries, views, PS protocol, complex scenarios |
| `in_predicate_to_in_subs_bugs.test` | 59 | Bug regression tests |

### 9.2 Test Categories

| Category | Test Cases |
|----------|-----------|
| Basic functionality | On/off switch, threshold values, conversion verification |
| Scalar IN-list | Integer, string, date, decimal types |
| Row-value IN-list | Multi-column comparisons, composite key lookups |
| NOT IN | Not supported (verified to not transform) |
| Type compatibility | Same result type required, mixed types |
| Index usage | ref, eq_ref, range, index scan on temp table |
| Semi-join | Transformation enables semi-join optimization |
| NULL handling | NULL values in IN-list, IS NULL behavior |
| Prepared statements | PS protocol, re-prepare on failure |
| Stored procedures | Transformation in SP context |
| ENUM type | Valid/invalid enum values |
| Views | IN-predicate in view definitions |
| Subquery context | IN-predicate in WHERE, HAVING, ON |
| Temporary tables | Support for temp table on left side |
| EXPLAIN output | "IN-list converted" Extra tag |
| Performance_schema | Conversion on P_S tables |
| DBUG hooks | Simulated failures for error path testing |

### 9.3 DBUG Hooks

Key DBUG evaluate-if hooks for testing error paths:

| Hook | Purpose |
|------|---------|
| `simulate_in_predicate_to_in_sub_oom` | Simulate OOM |
| `simulate_in_predicate_to_in_sub_create_tmp_table_failed` | Temp table creation failure |
| `simulate_in_predicate_to_in_sub_fill_tmp_table_failed` | Fill failure |
| `simulate_in_predicate_to_subs_new_query_failed` | Query_block creation failure |
| `simulate_in_predicate_to_subs_add_table_failed` | add_table failure |
| `simulate_in_predicate_to_in_subs_fix_in_subs_failed` | fix_fields failure |
| `simulate_in_predicate_to_new_Table_ident_failed` | Table_ident creation failure |
| `simulate_in_predicate_to_Item_asterisk_failed` | Asterisk creation failure |
| `simulate_in_predicate_to_subs_add_table_to_list_failed` | add_table_to_list failure |

---

## 10. Limitations

1. **NOT IN is not supported**: Conversion would degrade performance due to
   anti-join overhead compared to the original NOT IN evaluation.

2. **Only SELECT, INSERT SELECT, REPLACE SELECT**: Other statements (UPDATE,
   DELETE) are not supported.

3. **Not supported in stored procedures or triggers**: Only regular statement
   execution and prepared statement prepare are supported.

4. **All IN-list values must be constants or parameters**: No subqueries or
   column references in the value list.

5. **Result types must match**: The left side and IN-list values must have the
   same result type (no implicit type conversion).

6. **At least one argument must have a usable index**: Otherwise conversion
   would not improve performance.

7. **WHERE with OR conditions prevents conversion**: Cannot use semi-join
   optimization, which is the main benefit of the transformation.

8. **IS NULL / IS NOT NULL on the predicate prevents conversion**: NULL
   semantics differ between IN-predicate and EXISTS subquery.

9. **HAVING context not yet supported**: Noted as TODO in the code.

10. **ENUM type values must be valid**: Otherwise conversion falls back to
    normal evaluation via re-prepare.

11. **Information_schema database**: Special handling required for the
    generated database name (`<in_predicate_db>`).

---

## 11. Implementation Order

| Step | Component | Description |
|------|-----------|-------------|
| 1 | `sql/system_variables.h` | Add `rds_in_subquery_conversion_threshold` member |
| 2 | `sql/sys_vars.cc` | Define `Sys_in_subquery_conversion_threshold` system variable |
| 3 | `sql/table.h` | Add `transformed_in_predicate`, `is_transformed_in_predicate()`, `set_transformed_in_predicate()`, `get_transformed_in_predicate()` |
| 4 | `sql/sql_lex.h` | Add `disable_in_predicate_transform_to_subq` to LEX |
| 5 | `sql/sql_class.cc` | Reset `disable_in_predicate_transform_to_subq` in cleanup |
| 6 | `sql/item_cmpfunc.h` | Declare `is_applicable_to_convert_into_subq()`, `in_predicate_to_in_subs_transformer()`, `fill_transform_tmp_table()` |
| 7 | `sql/item_cmpfunc.cc` | Implement the full transformation (main body of work) |
| 8 | `sql/opt_explain_format.h` | Add `ET_IN_LIST_CONVERTED` enum |
| 9 | `sql/opt_explain_traditional.cc` | Add "IN-list converted" string |
| 10 | `sql/opt_explain_json.cc` | Add "IN-list converted" string |
| 11 | `sql/opt_explain.cc` | Push `ET_IN_LIST_CONVERTED` for transformed tables |
| 12 | `sql/sql_base.cc` | Handle temp table open/close for converted IN-predicates |
| 13 | `sql/sql_optimizer.cc` | Handle pseudo-table during optimization |
| 14 | `sql/sql_resolver.cc` | Properly resolve the generated subquery |
| 15 | `sql/sql_union.cc` | Handle within UNION execution |
| 16 | `sql/sql_prepare.cc` / `sql/sql_prepare.h` | PS protocol support |
| 17 | Test cases | Create comprehensive MTR tests |
| 18 | Result updates | Update affected test results |
