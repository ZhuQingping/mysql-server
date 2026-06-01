# Query Plan Cache Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL re-optimizes every execution of a prepared statement (PREPARE/EXECUTE), even when the execution plan would be identical. For OLTP workloads (e.g., TPC-C) where the same query is executed repeatedly with different parameter values, this repeated optimization wastes CPU.

### 1.2 Solution

Implement a session-level plan cache that stores the optimized execution plan after the first execution of a prepared statement. On subsequent executions, if the execution environment has not changed (optimizer switches, table statistics, DDL), the cached plan is reused directly, skipping the full optimization phase.

### 1.3 Scope

- **Session-level only**: each session has its own cache, no cross-session sharing
- **Prepared statements only**: works with `PREPARE/EXECUTE` and COM_STMT_PREPARE/COM_STMT_EXECUTE protocol
- **Single-table queries only**: only queries accessing one table are cached
- **Non-correlated scalar subqueries**: supported in restricted form

### 1.4 Lifecycle

```
Prepared_statement::execute_loop
  Prepared_statement::execute
    ...
    make_join_readinfo
    cache_plan()                     ← plan is cached
    Query_block::cached_plan points to cached plan

  ======================================
  Next Prepared_statement::execute
    ...
    exec_cached_plan()
      if apply_cached_plan_if_suitable() says cannot:
        invalidate_cached_plan()     ← discard cached plan
        re-optimize from scratch
      else:
        apply_cached_plan()          ← restore cached plan
        Query_block::join == Query_block::cached_plan
    execute cached plan
    Query_block::cleanup
      Query_block::join->destroy()  ← partial cleanup, plan stays cached

  ======================================
  Life end:
  Prepared_statement::~Prepared_statement
    Lex::destroy → JOIN::~JOIN → cached plan ends life
```

---

## 2. New Files

| File | Lines | Purpose |
|------|-------|---------|
| `sql/sql_plan_cache.h` | 149 | Public API: `cache_plan()`, `exec_cached_plan()`, `Exec_context`, `clone_if_transient()` |
| `sql/sql_plan_cache.cc` | 2098 | Full implementation: plan caching, plan application, invalidation, Item cloning, QEP_TAB reinit |

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/sql_optimizer.h` (JOIN class)

```c++
// New members in JOIN:
plan_cache::Exec_context *plan_cache_exec_context{nullptr};
bool cached_plan_hit{false};

// New methods:
void set_cached_plan_hit();
bool is_cached_plan_hit() const;

// Destructor change:
~JOIN() { ::destroy(plan_cache_exec_context); }

// New method:
bool shallow_clone(JOIN *orig);  // copy JOIN members for cached plan
```

### 3.2 `sql/sql_lex.h` (Query_block)

```c++
// New members in Query_block:
JOIN *cached_plan{nullptr};
plan_cache::plan_cache_state plan_cache_state{
    plan_cache::plan_cache_state::NONE};
```

### 3.3 `sql/sql_const.h`

```c++
namespace plan_cache {
enum class plan_cache_state {
  NONE,        // Initial state
  START,       // We are copying a plan for storing into the cache
  READY,       // Plan is cached and can be reused
  UNCACHEABLE  // This plan cannot be cached (permanent decision)
};
}
```

### 3.4 `sql/sql_optimizer.cc`

```
Line 1059: plan_cache::cache_plan(this);      ← after make_join_readinfo()
Line 10959: plan_cache::collect_item_params() ← before reduce_cond_for_table()
Line 10977: plan_cache::cmp_item_params_after_reduce_cond() ← after condition reduction
```

### 3.5 `sql/sql_select.cc`

```
Line 2016: if (plan_cache::exec_cached_plan(this)) { ← at start of JOIN::optimize(),
           if exec_cached_plan returns true, fall through to normal optimization
```

### 3.6 `sql/sql_prepare.cc`

```
Line 3543: plan_cache::invalidate_cached_plan(copy.m_lex->query_block);
           ← when re-preparing a statement
```

### 3.7 `sql/sys_vars.cc`

New system variables (see Section 7).

### 3.8 `sql/mysqld.cc`

Status variable registration: `Cached_plan_hits`.

### 3.9 `sql/CMakeLists.txt`

Add `sql_plan_cache.cc` to the build.

---

## 4. Core Data Structures

### 4.1 Exec_context

Stores all information needed to validate and re-apply a cached plan. Allocated in a dedicated MEM_ROOT that lives as long as the cached plan.

```c++
struct Exec_context {
  // Optimizer switch flags that affect plan choice
  static const ulonglong interested_optimizer_switch_flags =
      OPTIMIZER_SWITCH_INDEX_MERGE | OPTIMIZER_SWITCH_INDEX_MERGE_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_SORT_UNION |
      OPTIMIZER_SWITCH_INDEX_MERGE_INTERSECT |
      OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN |
      OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN | OPTIMIZER_SWITCH_MRR |
      OPTIMIZER_SWITCH_MRR_COST_BASED | OPTIMIZER_SWITCH_USE_INVISIBLE_INDEXES |
      OPTIMIZER_SKIP_SCAN | OPTIMIZER_SWITCH_PREFER_ORDERING_INDEX |
      OPTIMIZER_SWITCH_HYPERGRAPH_OPTIMIZER;

  Key_map quick_keys_map{0};              // indexes used by range access
  decltype(AccessPath().index_range_scan())
      index_range_scan_props;             // saved INDEX_RANGE_SCAN properties
  uint quick_used_key_parts{0};
  Temp_table_param *tmp_table_param{nullptr};
  ulonglong optimizer_switch{0};
  const CHARSET_INFO *character_set_client;
  uint pushed_idx_cond_keyno{MAX_KEY};
  Item *pushed_idx_cond{nullptr};
  bool key_read{false};
  double current_query_cost{0};
  ORDER_with_src *distinct_group_list{nullptr};
  ORDER_with_src *order_list{nullptr};
  Item *where_cond{nullptr};
  Item *having_cond{nullptr};
  ha_rows table_records{0};
  ulonglong table_version;
  Key_map covering_keys{0};
  Item *tab_condition{nullptr};
  int quick_type{-1};                     // -1 means "no range access"
  Explain_format_flags explain_flags;
  bool calc_found_rows;
  ha_rows m_select_limit;
  bool grouped;
  bool streaming_aggregation;
  bool select_distinct;
  double best_read;
  Query_arena *arena{nullptr};            // owns the MEM_ROOT for this plan

  // Map: transient Item → its clone (or nullptr if not yet cloned)
  using allocator_for_map = Mem_rootAllocator<std::pair<Item *const, Item *>>;
  std::map<Item *, Item *, std::less<Item *>, allocator_for_map> transient_items;

  // Recorded item_tree changes: {place_to_change, new_value}
  using allocator_for_vector = Mem_rootAllocator<std::pair<Item **, Item *>>;
  std::vector<std::pair<Item **, Item *>, allocator_for_vector> new_change_list;

  bool fill(THD *thd, JOIN *join, QEP_TAB *cached_qep_tab);
  bool is_environment_changed(THD *thd);
  bool is_table_stats_changed_sharply(ha_rows new_rows, double ratio);
  ~Exec_context();
};
```

### 4.2 plan_cache_state

```
NONE        → Initial state, no plan cached
START       → Currently copying a plan into cache
READY       → Plan is cached and available for reuse
UNCACHEABLE → Permanent: this query block will never cache a plan
```

### 4.3 JOIN extensions for plan cache

```c++
// In JOIN class:
plan_cache::Exec_context *plan_cache_exec_context{nullptr};  // saved context
bool cached_plan_hit{false};  // true after at least one successful reuse
```

### 4.4 Query_block extensions

```c++
JOIN *cached_plan{nullptr};                               // the cached JOIN
plan_cache::plan_cache_state plan_cache_state{NONE};      // current state
```

---

## 5. Execution Flow

### 5.1 Caching a Plan (`cache_plan`)

Called at the end of `JOIN::optimize()`, after `make_join_readinfo()`.

```
cache_plan(JOIN *orig_join):
  1. Early exits:
     - if plan_cache_state == READY: already cached
     - if plan_cache_state == UNCACHEABLE: permanently not cacheable
     - if !check_query_plan_cachable(): not eligible

  2. Switch to dedicated MEM_ROOT (not the execution MEM_ROOT,
     which is freed at end of execution; not the statement's permanent
     MEM_ROOT, which would grow indefinitely with invalidations):
     - MEM_ROOT mem_root(key_memory_plan_cache_mem_root, 1024);
     - Query_arena arena(&mem_root, STMT_PREPARED);
     - thd->swap_query_arena(arena, &arena_backup);

  3. Build transient items map:
     - Scan THD::item_list(), identify items in transient MEM_ROOT
     - transient_items[item] = nullptr  (no clone yet)

  4. Record item tree changes:
     - Scan THD::change_list (registered by change_item_tree())
     - For each change to a permanent item: save {place, new_value}
     - If new_value is transient: clone it via clone_if_transient()
     - Store in new_change_list (reversed to chronological order)

  5. Check Item_param stability:
     - some_item_param_are_gone(): verify no Item_param was optimized away
       or had its type transformed (e.g., <= ? becoming = ?)
     - If so: set UNCACHEABLE and return

  6. Clone the plan:
     - new JOIN(thd, query_block) into dedicated MEM_ROOT
     - join_local->shallow_clone(orig_join)
     - pq_dup_tabs(join_local, orig_join, false) — clone QEP_TABs
     - Exec_context::fill() — save all environment and plan properties

  7. Set qep_tab[0].set_table(nullptr) — avoid dangling TABLE pointer

  8. Move MEM_ROOT from stack to heap (so it survives function return):
     - Allocate MEM_ROOT and Query_arena inside the MEM_ROOT itself
     - Store arena pointer in Exec_context::arena

  9. Set plan_cache_state = READY, increment cached_plan_count
```

### 5.2 Reusing a Cached Plan (`exec_cached_plan`)

Called at the start of `JOIN::optimize()`, before any optimization work.

```
exec_cached_plan(Query_block *sl):
  1. Check prerequisites:
     - rds_plan_cache is ON
     - plan_cache_state == READY (a cached plan exists)
     - set_limit() succeeds

  2. apply_cached_plan_if_suitable():
     a. Verify table version unchanged (no DDL)
     b. Verify optimizer_switch/charset unchanged (is_environment_changed)
     c. Verify table stats not changed sharply (is_table_stats_changed_sharply)
     d. Verify not using secondary engine
     e. Verify THD::change_list is empty (no conflicting changes from other query blocks)
     f. If any check fails: invalidate_cached_plan() and return true

  3. apply_cached_plan():
     a. bind_fields() — rebind Items to current table instance
     b. reset_cached_plan() — clear per-execution state
     c. Reinitialize tmp_table_param from saved context
     d. Replay item tree changes from new_change_list:
        - thd->change_item_tree(place, new_value) for each recorded change
        - Mark generated columns in read_set
     e. Restore GROUP BY / ORDER BY lists from context
     f. Restore where_cond, having_cond from context (with copy_andor_structure)
     g. Restore JOIN boolean members (explain_flags, calc_found_rows, etc.)

  4. refix_table():
     - Restore covering_keys, pushed ICP condition, key_read flag

  5. reinit_qep_tab_properties():
     Per QEP_TAB, reinitialize based on access type:
     - JT_SYSTEM: verify still system table, evaluate WHERE
     - JT_CONST: replace_cache_key(), read const row, evaluate WHERE
     - JT_REF/JT_EQ_REF/JT_REF_OR_NULL: replace_cache_key()
     - JT_RANGE/JT_INDEX_MERGE: call test_quick_select() with saved
       keys_map, verify same quick_type and keys, apply saved
       INDEX_RANGE_SCAN properties
     - JT_INDEX_SCAN/JT_ALL: no action needed

  6. Reinit tmp table QEP_TABs: rerun_constructor_for_tmp_table()

  7. Partition pruning if needed

  8. Set join as optimized, increment cached_plan_hits
```

### 5.3 Invalidating a Cached Plan (`invalidate_cached_plan`)

```
invalidate_cached_plan(Query_block *select):
  1. If plan has been hit (cached_plan_hit), JOIN::destroy() was already
     called at end of previous execution — skip destroy()
  2. If not yet hit, call plan->destroy()
  3. destroy_cached_plan():
     - Decrement cached_plan_count
     - Destroy JOIN (calls ~Exec_context)
     - Cleanup and free arena items
     - Free dedicated MEM_ROOT
  4. Set cached_plan = nullptr, plan_cache_state = NONE
  5. Increment cached_plan_invalidations
```

### 5.4 Plan Cleanup at End of Execution

```
Query_block::cleanup():
  join->destroy()
  if using cached plan:
    reset cached plan status (for next reuse)
    do NOT destroy the cached plan itself
  else:
    destroy join normally
```

---

## 6. Item Cloning Strategy (`clone_if_transient`)

### 6.1 Core Principle

In a prepared statement, Items are either:
- **Long-lived (permanent)**: allocated in the statement's permanent MEM_ROOT, survive across executions. Can be reused directly — no clone needed.
- **Short-lived (transient)**: allocated in the execution MEM_ROOT, freed at end of execution. Must be cloned for the cached plan.

### 6.2 The SL-in-LL Problem

When a long-lived Item has had one of its arguments replaced with a short-lived Item (via `change_item_tree()`), simply storing the LL Item in the cache doesn't work: the replacement is rolled back at end of execution.

**Solution** (two-phase):
1. **During caching**: Record all `change_item_tree()` calls affecting LL items. Clone SL replacement items. Store pairs `{LL_place, LL_or_cloned_SL_value}` in `new_change_list`.
2. **During plan application**: Replay these changes via `thd->change_item_tree()`. Changes are tracked and will be rolled back normally at end of execution.

### 6.3 Supported Item Types for Cloning

The optimizer creates only a restricted set of Item types. `clone_if_transient()` handles:

| Item Type | Clone Method |
|-----------|-------------|
| `Item_func_eq/lt/gt/le/ge/ne` | `make_cmp_op<T>(thd, op)` — clone args recursively |
| `Item_func_isnull/isnotnull` | `make_cmp_op<T>(thd, op)` |
| `Item_func_if` | `make_cmp_op<Item_func_if>(thd, op)` |
| `Item_func_true/false` | `new Item_func_true/false()` |
| `Item_cond_and/or` | Recursive clone of argument list |
| `Item_typecast_*` (datetime/date/time/double) | `new Item_typecast_*(arg)` |
| `Item_datetime_literal` | Copy MYSQL_TIME, create new literal |
| `Item_time_literal` | Copy MYSQL_TIME, create new literal |
| `Item_date_literal` | Copy MYSQL_TIME, create new literal |
| `Item_cache` | `Item_cache::get_cache()`, setup, store |
| `Item_field` (generated column) | `new Item_field(field)` |
| `Item_int` and similar constants | `i->clone_item()` |

Any unhandled Item type triggers an assertion failure (for internal discovery during testing).

### 6.4 `get_item_and_refresh`

```c++
static inline void get_item_and_refresh(Item *&dst, Item *src, bool is_cond) {
  dst = src;
  if (!src) return;
  if (is_cond) dst = dst->copy_andor_structure(current_thd);
  dst->update_used_tables();
}
```

When applying a cached plan, conditions must be copied with `copy_andor_structure()` because `make_tmp_tables_info()` may modify AND/OR structures in-place (dissolving nested ANDs). `update_used_tables()` is needed so Items know they are constant after replaying changes.

---

## 7. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `rds_plan_cache` | bool | session | OFF | Enable/disable plan cache |
| `rds_plan_cache_allow_change_ratio` | double | session | 0.0 | If > 0, invalidate cached plan when table row count changes by this fraction |

---

## 8. Status Variables

| Variable | Scope | Description |
|----------|-------|-------------|
| `Cached_plan_hits` | session | Number of times a cached plan was successfully reused |
| `Cached_plan_count` | global | Number of plans currently cached (atomic) |
| `Cached_plan_invalidations` | global | Number of plan cache invalidations (atomic) |

---

## 9. Eligibility Rules (`check_query_plan_cachable`)

A query plan is cacheable only if ALL of the following are true:

| Check | Reason |
|-------|--------|
| Single table only (`leaf_table_count == 1`) | Multi-table join plan caching is not supported |
| Is a SELECT command | DML not supported |
| Is a prepared statement (`is_stmt_prepared_or_executed`) | Regular queries are not cached |
| `rds_plan_cache` is ON | User opt-in |
| No user/system variables in query (`!has_user_or_system_var`) | `WHERE col=@a` may optimize to WHERE FALSE |
| No ROLLUP | Item_rollup items cannot be properly handled |
| Not in stored procedure (`!sp_runtime_ctx`) | Not supported |
| Not in stored function (`sroutines_list.elements == 0`) | Not supported |
| Not under LOCK TABLES | Clone needs open table, conflicts with lock mode |
| Not a set operation (UNION/EXCEPT/INTERSECT) | Not supported |
| Subquery (if any) must be: scalar, non-correlated, non-maxmin, cacheable | Simplifies caching logic |
| `THD::change_list` must be empty (for subquery cases) | Cannot identify which changes to replay |
| No fulltext search (`type != JT_FT`) | Not supported |
| Not secondary engine | Not supported |
| Not temporary table | Not supported |
| Not information_schema / performance_schema | Not supported |
| Not system view | Not supported |
| `hidden_items_from_optimization == 0` | Generated column substitution adds hidden fields we cannot recreate |
| `zero_result_cause == nullptr` | Impossible WHERE should not be cached |
| Not using hypergraph optimizer | Plan cache relies on QEP_TAB which hypergraph doesn't create |

### Additional Checks Before Applying a Cached Plan

| Check | Reason |
|-------|--------|
| Table version unchanged | DDL was done, plan is stale |
| Optimizer switch flags unchanged | Plan choice depends on these flags |
| Character set client unchanged | Comparison rules may differ |
| Table stats not changed sharply | Cost-based decisions may differ |
| Not using secondary engine | Secondary engine optimization differs |
| Item_param not optimized away | e.g., `?<=col` → `?=col` depends on param value |
| Item type not transformed | e.g., `<=` → `=` depends on param value |
| `test_quick_select()` produces same quick_type and keys | Index selection may differ with new stats |

---

## 10. Key Helper Functions

### 10.1 `QEP_TAB::replace_cache_key()`

Simplified version of `create_ref_for_key()`. Rebuilds ref access key values for JT_CONST/JT_REF scans when reusing a cached plan. Re-evaluates `store_key` objects with current parameter values. Returns error if key value is truncated (param value out of index range), which causes plan invalidation.

### 10.2 `QEP_TAB::rerun_constructor_for_tmp_table()`

Uses placement-new to reset QEP_TAB for optimizer-internal temporary tables. Between executions, `make_tmp_tables_info()` fills these with short-lived data that must be cleared.

### 10.3 `JOIN::shallow_clone(JOIN *orig)`

Copies ~30 member variables from the original JOIN to the cached plan JOIN. Does NOT deep-clone Items or QEP_TABs (those are handled separately). Must be updated when new members are added to JOIN (marked with `HUAWEI_ASSERT_MATCH_CANON sql_class_JOIN`).

### 10.4 `reinit_qep_tab_properties(QEP_TAB *qt)`

Reinitializes QEP_TAB based on its access type when reusing a cached plan. For range/index merge scans, calls `test_quick_select()` with saved keys to regenerate the AccessPath, then verifies the new path matches the cached one in type and index usage.

### 10.5 `make_group_order_list()`

Clones ORDER/GROUP BY lists for the cached plan. Uses `pq_dup_order()` from the parallel query code. Skips `fix_fields` for virtual column replacements during caching.

### 10.6 `collect_item_params()` / `cmp_item_params_after_reduce_cond()`

Tracks Item_param objects in conditions before and after optimization. If any Item_param is optimized away (e.g., `WHERE ?=1` with ?=1 becomes WHERE TRUE, then ?=2 would be wrong), marks the plan as UNCACHEABLE.

### 10.7 `emulates_hint()`

When a cached plan used index merge or skip scan, this function tells the optimizer to pretend that the corresponding hint was set. This ensures `test_quick_select()` picks the same access method when re-applying the cached plan.

---

## 11. Memory Management

### 11.1 Dedicated MEM_ROOT

Each cached plan has its own MEM_ROOT, separate from both:
- The execution MEM_ROOT (freed after each execution)
- The statement's permanent MEM_ROOT (would grow indefinitely with invalidations)

The MEM_ROOT is allocated on the stack during `cache_plan()`, then moved to heap storage within itself so it survives function return.

### 11.2 Lifecycle

```
cache_plan():
  MEM_ROOT mem_root on stack (1024 byte initial)
  → all plan allocations go into mem_root
  → move mem_root to heap (allocated within itself)
  → Exec_context::arena holds pointer to heap MEM_ROOT

destroy_cached_plan():
  → move MEM_ROOT to stack object (reverse of cache_plan)
  → stack destructor frees all memory
```

### 11.3 Why Not Permanent MEM_ROOT

If cached plans were allocated in the statement's permanent MEM_ROOT, each invalidation+re-cache cycle would grow that MEM_ROOT indefinitely (the old allocations are never freed until the prepared statement is deallocated). The dedicated MEM_ROOT allows proper cleanup on invalidation.

---

## 12. Optimizer Trace Support

Plan cache operations are traced:

```
cache_plan:
  "cache_plan": { "cached": true/false }

apply_cached_plan:
  "apply_cached_plan": {
    "used": true/false,
    "cause": "Optimizer_switch or character set was changed" / ...
  }

invalidate_cached_plan:
  "invalidate_cached_plan": { "invalidate": true }
```

---

## 13. Relationship with Parallel Query (PQ)

The plan cache implementation reuses several PQ functions:
- `pq_dup_tabs()` — clone QEP_TAB structures
- `pq_dup_order()` — clone ORDER objects
- `Temp_table_param::pq_copy()` — copy Temp_table_param

However, the Item cloning strategy is different:
- **PQ**: Clones all Items for each worker thread (needs full deep clone, workers evaluate concurrently)
- **Plan Cache**: Only clones short-lived Items (reuses permanent Items across executions)

The function `clone_if_transient()` replaces the PQ `pq_clone()` approach for Items. When PQ is ported, this design could be reconsidered (as noted in the source code).

---

## 14. Bug Fixes (Historical)

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| Sysbench range query perf fluctuation | Plan cache changing execution path | Performance fix in test_quick_select interaction |
| Window function + ORDER BY wrong result | Cached plan missing sort state | Restore ORDER BY properties correctly |
| Scalar subquery + length(BLOB)<? wrong result | Item_cache not properly re-bound | Fix Item_cache clone and re-binding |
| DStore assert failure | DStore handler differences | Handle DStore-specific code paths |
| ORDER BY int, tinytext coredump | Missing Item type in clone_if_transient | Add Item_typecast support |
| Many aggregate functions coredump | sum_funcs pointer not reset | Reset sum_funcs before make_tmp_tables_info |
| Time/timestamp type wrong result | Temporal literal Items not cloned | Add Item_time_literal / Item_date_literal cloning |
| CTE ASan coredump | Dangling pointer after plan invalidation | Proper cleanup of cached plan on invalidation |
| Tinyblob IS NOT UNKNOWN wrong sort | Missing comparison operator cloning | Add IS NULL/IS NOT NULL/>/</>=/<=/<> cloning |
| Memory growth over time | Plan cache using permanent MEM_ROOT | Switch to dedicated MEM_ROOT per plan |

---

## 15. Test Coverage

| Test File | Description |
|-----------|-------------|
| `mysql-test/include/session_plan_cache.inc` | Main test body (shared between InnoDB and DStore) |
| `mysql-test/t/session_plan_cache.test` | InnoDB run of main test |
| `mysql-test/suite/dstore_main/t/session_plan_cache.test` | DStore run of main test |
| `mysql-test/include/session_plan_cache_debug.inc` | Debug-only test body |
| `mysql-test/t/session_plan_cache_debug.test` | InnoDB debug test |
| `mysql-test/suite/dstore_main/t/session_plan_cache_debug.test` | DStore debug test |
| `mysql-test/suite/sys_vars/t/rds_plan_cache_allow_change_ratio.test` | System variable test |

Test scenarios covered:
- Basic caching and reuse of single-table SELECT
- Different access types: table scan, index scan, ref scan, range scan, index merge
- Prepared statement parameter changes (different values, NULL, out-of-range)
- Optimizer switch changes causing invalidation
- DDL causing invalidation (table version change)
- Table statistics changes causing invalidation (rds_plan_cache_allow_change_ratio)
- Character set changes causing invalidation
- Subquery support (scalar, non-correlated)
- Window function + ORDER BY
- Aggregation queries (GROUP BY, DISTINCT, COUNT, SUM, AVG)
- ICP (Index Condition Pushdown) with cached plan
- Covering index with key_read
- ORDER BY / GROUP BY with const elimination
- INSERT SELECT (not supported)
- LIMIT without ORDER BY
- ROLLUP (not supported)
- Temporary tables (not supported)
- Fulltext search (not supported)
- UNION (not supported)
- Stored procedures (not supported)
- DBUG test hooks: `apply_cached_plan_fail`, `plan_cache_debug_test_quick_select_fail`

---

## 16. Limitations

- Single-table queries only
- SELECT statements only
- Prepared statements only (no plan caching for regular queries)
- No UNION / EXCEPT / INTERSECT support
- No multi-table join support
- No ROLLUP support
- No stored procedure / function support
- No temporary table or information_schema support
- No fulltext search support
- Not compatible with hypergraph optimizer (relies on QEP_TAB)
- Subquery support restricted to non-correlated scalar subqueries
- Generated column substitution in ORDER BY may prevent caching (`hidden_items_from_optimization`)
- Session-level only (no cross-session sharing)
- Does not support `WHERE col=@variable` (user variables make plan unpredictable)

---

## 17. Implementation Order

1. **sql_const.h** — Define `plan_cache_state` enum
2. **sql_lex.h** — Add `cached_plan` and `plan_cache_state` to Query_block
3. **sql_optimizer.h** — Add `plan_cache_exec_context`, `cached_plan_hit`, `shallow_clone()` to JOIN
4. **sql_plan_cache.h** — Define `Exec_context`, public API functions, `clone_if_transient()`
5. **sql_plan_cache.cc** — Full implementation
6. **sql_optimizer.cc** — Integration points: `cache_plan()`, `collect_item_params()`, `cmp_item_params_after_reduce_cond()`
7. **sql_select.cc** — `exec_cached_plan()` call at start of `JOIN::optimize()`
8. **sql_prepare.cc** — `invalidate_cached_plan()` on re-preparation
9. **sys_vars.cc** — Define `rds_plan_cache`, `rds_plan_cache_allow_change_ratio`
10. **mysqld.cc** — Register `Cached_plan_hits` status variable
11. **CMakeLists.txt** — Add `sql_plan_cache.cc`
12. **Test suite** — Port and adapt MTR test cases
