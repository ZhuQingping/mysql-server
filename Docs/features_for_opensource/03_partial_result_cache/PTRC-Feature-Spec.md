# Partial Table Result Cache (PTRC) Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

In nested loop joins and correlated subqueries, the same inner scan is repeatedly executed with identical parameter values. Each rescan incurs the full cost of re-executing the inner plan, even when the result for that parameter combination was already computed in a previous iteration. This is particularly wasteful when the number of distinct parameter values is much smaller than the total number of rescans.

### 1.2 Solution

PTRC inserts a caching node above parameterized inner scans (nested loop join inner side or correlated subqueries). It uses an in-memory hash table with LRU eviction to store results keyed by the outer row's column values. When the same parameter combination is encountered again, cached results are returned directly without re-executing the inner scan.

### 1.3 Execution Model

```
Without PTRC (nested loop join):
  for each row in outer_table:
    rescan inner_table with outer_row values  ← repeated full scan

With PTRC:
  for each row in outer_table:
    if cache_hit(outer_row): return cached results
    else: scan inner_table → cache results → return results

Without PTRC (correlated subquery):
  SELECT * FROM t1 WHERE t1.a IN (SELECT b FROM t2 WHERE t2.c = t1.c)
  → for each t1 row: re-evaluate subquery with t1.c

With PTRC:
  → for each t1 row: if t1.c already seen → return cached subquery result
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| New source (`sql/partial_result_cache.h/.cc`) | 2 | ~1,500 |
| MySQL core modifications | ~10 | ~500 |
| MTR tests (`mysql-test/suite/ptrc/`) | 6 | ~6,700 |
| **Total** | **~18** | **~8,700** |

---

## 2. New Files

### 2.1 `sql/partial_result_cache.h` (275 lines)

Defines the `ptrc` namespace with all PTRC types:

| Type | Purpose |
|------|---------|
| `Instrumentation` | Cache statistics (hits, misses, evictions, overflows, memory) |
| `LRU_node` | Doubly-linked list node storing key and row data |
| `LRU_list_type` | LRU list with head, tail, and free list |
| `PTRC_context` | Stores `item_addr` (Item_uint for LRU node pointer) and `exec_flags` (Item_uint for NO_MATCHED_ROW/WAS_NULL) |
| `PathParameters` | Columns, tmp tables, and flags for access path creation |
| `Memory_objects` | Owns PTRC's MEM_ROOT and PathParameters array for cleanup |
| `Ptrc_status` | State machine enum (NONE, LOOKUP, FETCH_NEXT_TUPLE, FILLING_CACHE, BYPASS_MODE, END_OF_SCAN) |
| `PtrcIterator` | RowIterator subclass implementing the PTRC state machine |

Free functions:

| Function | Purpose |
|----------|---------|
| `CreateAccessPath()` | Decision point: determine whether PTRC is beneficial, create AccessPath if so |
| `SetCostOnPtrcPath()` | Estimate cost for PTRC access path |
| `is_ptrc_enabled()` | Check optimizer_switch flag |
| `get_min_cost_threshold()` | Get cost threshold from session variable |
| `cleanup()` | Free PTRC MEM_ROOT and all objects at statement end |
| `print()` | Format PTRC info for EXPLAIN output |

### 2.2 `sql/partial_result_cache.cc` (1468 lines)

Full implementation including:
- `CreateAccessPath()` — eligibility checks, key/result table construction, cost estimation
- `PtrcIterator::Read()` — state machine for cache lookup, filling, and reading
- `PtrcIterator::LRU_free_mem()` — LRU eviction with batch key removal
- `make_key_cols_tables()` / `make_res_cols_tables()` — TableCollection construction
- `estimate_num_groups()` — distinct value estimation for cost model
- `SetCostOnPtrcPath()` — cost model for nested loop join PTRC

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/sql_class.h`

```c++
// New member in THD class:
ptrc::Memory_objects *ptrc_objects{nullptr};
```

### 3.2 `sql/sql_class.cc`

```c++
// In THD::cleanup_after_query():
if (!in_sub_stmt) ptrc::cleanup(this);
```

Called at end of each statement to free all PTRC allocations. Skipped inside sub-statements (stored functions) because the parent query's PTRC is still in use.

### 3.3 `sql/sql_const.h`

```c++
constexpr const uint64_t OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE(1ULL << 30);
```

### 3.4 `sql/sys_vars.cc`

- Add `OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE` to optimizer_switch bitmap
- Define 4 session variables (see Section 8)

### 3.5 `sql/join_optimizer/access_path.h`

```c++
// New AccessPath type:
PARTIAL_RESULT_CACHE,

// Union member:
struct {
  AccessPath *child;
  ptrc::PathParameters *param;
} partial_result_cache;

// Accessor:
auto &ptrc() { ... }
```

### 3.6 `sql/join_optimizer/access_path.cc`

- Include `partial_result_cache.h`
- `CreateIteratorFromAccessPath()`: PARTIAL_RESULT_CACHE case creates `PtrcIterator`
- `FindTablesToGetRowidFor()`: Handle row ID propagation through PTRC path
- `ComputeRowSizeUpperBound()`: Account for PTRC in row size calculations

### 3.7 `sql/join_optimizer/explain_access_path.cc`

- Include `partial_result_cache.h`
- EXPLAIN FORMAT=TREE: call `ptrc::print()` to display cache keys and statistics

### 3.8 `sql/join_optimizer/join_optimizer.cc`

- Debug string for `PARTIAL_RESULT_CACHE` access path type

### 3.9 `sql/sql_executor.cc`

```c++
// In CreateNestedLoopAccessPath():
if (join) {
  inner = ptrc::CreateAccessPath(thd, nullptr, inner, outer_tables,
                                 inner_tables, join);
}
```

PTRC is inserted between the nested loop join and its inner path.

### 3.10 `sql/sql_union.cc`

```c++
// In Query_expression::create_access_paths():
// For correlated subqueries (UNCACHEABLE_DEPENDENT but not RAND/SIDEEFFECT):
if ((uncacheable & UNCACHEABLE_DEPENDENT) &&
    !(uncacheable & (UNCACHEABLE_RAND | UNCACHEABLE_SIDEEFFECT))) {
  m_root_access_path = ptrc::CreateAccessPath(
      thd, item, m_root_access_path, 0, 0, nullptr);
}
```

### 3.11 `sql/opt_hints.h` / `sql/opt_hints.cc`

```c++
// In Opt_hints_qb:
Mem_root_array<PT_qb_level_hint *> ptrc_hints;
void register_ptrc_hint(PT_qb_level_hint *hint_arg);
void apply_ptrc_hints(JOIN *join, table_map *force_tab_map,
                      table_map *ignore_tab_map, bool *force_subquery_ptrc,
                      bool *force_no_subquery_ptrc, bool *force_join_ptrc,
                      bool *ignore_join_ptrc);
```

### 3.12 `sql/parse_tree_hints.cc`

```c++
// In PT_qb_level_hint::contextualize():
qb->register_ptrc_hint(this);
```

---

## 4. Core Data Structures

### 4.1 PtrcIterator State Machine

```
                  ┌──────────────────────────────────┐
                  │           Init()                  │
                  │    InitRowBuffer, alloc context   │
                  │         ↓                         │
                  │     LOOKUP ──────────────────┐    │
                  │       │                      │    │
                  │   cache hit?             cache miss│
                  │   yes │ no                   │    │
                  │       ↓                      ↓    │
                  │  FETCH_NEXT_TUPLE      FILLING_CACHE
                  │       │                      │    │
                  │   next cached row?       source→Read()
                  │   yes │ null              │    │
                  │       │         buffer full?     │
                  │       │         yes → BYPASS_MODE│
                  │       │         no → continue    │
                  │       ↓                      │    │
                  │  return row            StoreRow() │
                  │       │                      │    │
                  │  FETCH_NEXT_TUPLE ←───── FILLING_CACHE
                  │       │                      │    │
                  │   no more rows           source EOF│
                  │       ↓                      ↓    │
                  │    END_OF_SCAN          END_OF_SCAN
                  └──────────────────────────────────┘
```

### 4.2 Ptrc_status Enum

```c++
enum class Ptrc_status {
  NONE,              // Initialized status
  LOOKUP,            // Attempt to perform a cache lookup
  FETCH_NEXT_TUPLE,  // Get another tuple from the cache
  FILLING_CACHE,     // Read inner node to fill cache
  BYPASS_MODE,       // Bypass mode: read from source without caching
  END_OF_SCAN,       // Ready for rescan
};
```

### 4.3 LRU_node

```c++
struct LRU_node {
  LRU_node *prev{nullptr}, *next{nullptr};
  ImmutableStringWithLength key_data;  // encoded cache key
  size_t key_size = 0;
  LinkedImmutableString row_data;      // encoded result row (linked list)
  size_t row_size = 0;

  void set_data(const char *k, size_t ks, const char *r, size_t rs);
};
```

### 4.4 LRU_list_type

```c++
struct LRU_list_type {
  LRU_node *head{nullptr};   // most recently used
  LRU_node *tail{nullptr};   // least recently used
  std::list<Key_pair *> free_list;  // reusable freed slots
};
```

### 4.5 Hash Table (Reuses HashJoinRowBuffer)

PTRC reuses MySQL's existing `HashJoinRowBuffer` from the hash join implementation for its hash table. The buffer stores key→value mappings where:
- **Key**: serialized outer row columns (`StoreFromTableBuffers`)
- **Value**: linked list of serialized inner result rows (`LinkedImmutableString` chain)

Multiple matching rows for the same key are stored as a linked list via `LinkedImmutableString::next`.

### 4.6 PathParameters

```c++
struct PathParameters {
  TableCollection *key_tables;       // columns used as cache key
  TableCollection *res_tables;       // columns in cached result (includes pseudo_context_table)
  mem_root_deque<Item *> *res_items; // select list items to cache (subquery only)
  TABLE *res_tmp_table;              // tmp table for subquery result field conversion
  TABLE *pseudo_context_table;       // virtual table with LRU_node_ptr + Exec_flags columns
  bool *was_null;                    // pointer to Item_in_subselect::was_null
  bool is_exists_subquery;           // true for EXISTS subquery (no result caching needed)
};
```

### 4.7 Pseudo Context Table

A virtual table with two columns appended to every cached row:

| Column | Type | Purpose |
|--------|------|---------|
| `LRU_node_ptr` | BIGINT (8 bytes) | Pointer to the LRU_node for O(1) list updates |
| `Exec_flags` | TINYINT (1 byte) | Bit flags: `NO_MATCHED_ROW` (bit 0), `WAS_NULL` (bit 1) |

When a cache hit occurs, the LRU_node pointer is read from the cached row to move the node to the head of the LRU list in O(1) time.

### 4.8 Memory_objects

```c++
struct Memory_objects {
  MEM_ROOT root;                           // PTRC's own MEM_ROOT
  MEM_ROOT *saved_root;                    // backup of THD::mem_root
  Mem_root_array<PathParameters *> params; // owned PathParameters for cleanup
};
```

Owned by `THD::ptrc_objects`. The `PtrcMemRoot` RAII class swaps `THD::mem_root` with `ptrc_objects->root` during PTRC operations, ensuring PTRC allocations use the PTRC MEM_ROOT with its size limit.

---

## 5. Execution Flow

### 5.1 PTRC Eligibility Check (in CreateAccessPath)

```
ptrc::CreateAccessPath(thd, sub, inner_path, ptrc_key_tables, ptrc_res_tables, ptrc_join)
  → Check hints (force_to_use / force_not_use)
  → Check optimizer_switch: is_ptrc_enabled()
  → Check deterministic (no RAND() in condition or left_expr)
  → is_ptrc_suitable():
      - Must be SELECT query
      - Must be deterministic (no RAND)
      - No const plan (self or any outer query_block)
      - No ZERO_ROWS inner path
      - Subquery must not reference outer-query aggregation
      - Subquery must not appear in HAVING condition
  → make_key_cols_tables(): construct key TableCollection
      - For subquery: collect dependent parameters (left_expr columns)
      - For NLJ: collect outer tables' read_set columns
      - Must not contain BLOB columns
  → estimate_distinct_val_ratio():
      - Compute distinct_rows and estimate_rows
      - ratio = (estimate_rows - distinct_rows) / estimate_rows
      - If ratio < partial_result_cache_cost_threshold: not beneficial
  → make_res_cols_tables(): construct result TableCollection
      - For NLJ: collect inner tables' columns, disable ref cache
      - For subquery: create tmp table from select list fields
      - Create pseudo_context_table
  → SetCostOnPtrcPath(): estimate cost
  → Reset record buffers for key and result tables
  → Return AccessPath(PARTIAL_RESULT_CACHE)
```

If any check fails, the function returns the original `inner_path` unchanged — PTRC is silently skipped.

### 5.2 Key Column Construction

**For correlated subqueries:**
- Use `Item_subselect::get_item_params()` to collect dependent outer-reference columns
- These are the columns from outer tables that the subquery depends on
- The subquery must be correlated (has non-empty parameters)

**For nested loop joins:**
- Collect all columns in `read_set_internal` of outer_tables
- Use `TablesContainedIn(join, outer_tables)` to find the QEP_TABs
- Note: This may include columns not actually needed for the join condition (over-approximation)

### 5.3 Result Column Construction

**For nested loop joins:**
- Collect all inner table columns
- Disable ref cache on inner tables (`tab->ref().disable_cache = true`) because cached keys would not be updated on PTRC hit
- Add pseudo_context_table columns

**For subqueries:**
- Create a temporary table from the subquery's visible SELECT list fields
- `skip_create_table = true` (structure only, no storage)
- `precomputed_group_by = true` (aggregates already computed)
- The tmp table's Field objects serve as the cache storage format
- Items are stored via `save_in_field()` and restored via `Item_field` wrapping

### 5.4 Cost Estimation (NLJ)

```c++
SetCostOnPtrcPath(thd, sub, join, path):
  cpu_tuple_cost = row_evaluate_cost(1)
  cpu_operator_cost = tmptable_readwrite_cost(MEMORY_TMPTABLE, 1, 0)

  hash_mem_bytes = partial_result_cache_max_mem_size
  est_entry_bytes = ComputeRowSizeUpperBound(key_tables) + ComputeRowSizeUpperBound(res_tables)
  est_cache_entries = floor(hash_mem_bytes / est_entry_bytes)

  estimate_num_groups() → ndistinct, est_rows
  evict_ratio = 1 - min(est_cache_entries, ndistinct) / ndistinct
  hit_ratio = 1/ndistinct * min(est_cache_entries, ndistinct) - (ndistinct/est_rows)
  hit_ratio = max(hit_ratio, 0.0)

  total_cost = child->cost * (1 - hit_ratio) + cpu_tuple_cost
  total_cost += cpu_tuple_cost * evict_ratio                           // eviction overhead
  total_cost += cpu_tuple_cost / 10 * evict_ratio * calls              // per-tuple eviction
  total_cost += cpu_tuple_cost + cpu_operator_cost * (1-hit_ratio) * calls  // cache write
```

For subqueries: a simpler formula that adds `best_rowcount * cpu_operator_cost` to child cost.

### 5.5 Distinct Value Estimation

```c++
estimate_num_groups(src_join, tables, &distinct_rows, &est_rows, is_sub):
  for each table in tables:
    skip if const/eq_ref/system
    if position() is nullptr (outer query not yet optimized):
      distinct_rows = 1.0, est_rows = 1.0  // conservative: avoid PTRC
    rowcount = pos->rows_fetched * pos->filter_effect
    rec_per_key = get_record_per_key(table)  // index statistics
    distinct_rows *= (rec_per_key > 0) ? rowcount / rec_per_key : rowcount
    rows *= rowcount

  if is_sub: est_rows = max(rows, src_join->best_rowcount)
  else: est_rows = rows
```

`get_record_per_key()` finds the best matching index that covers all key columns and uses its `records_per_key` statistic.

### 5.6 PtrcIterator::Init()

```
1. If BYPASS_MODE: just init source iterator
2. If first Init():
   a. PrepareForRequestRowId on res_tables
   b. Create PTRC_context (item_addr + exec_flags Items)
   c. Set pseudo_context_table from res_tables
   d. InitRowBuffer(): create HashJoinRowBuffer with max_mem_size
      - If OOM: disable PTRC, set BYPASS_MODE
   e. For subqueries: create saved_res_items and cached_res_fields
      - Map each tmp table field → Item_field for result restoration
3. Change status to LOOKUP
4. Init source iterator
```

### 5.7 PtrcIterator::Read() — State Machine

```
LOOKUP / END_OF_SCAN:
  1. Read key columns into key_buff (StoreFromTableBuffers)
  2. Hash lookup: m_row_buffer->find(key)
  3. If cache hit:
     a. Load cached row into res_tables (LoadIntoTableBuffers)
     b. Update LRU list (move node to head)
     c. For subqueries: ChangeToUseTmpField()
     d. Read pseudo_context flags:
        - If NO_MATCHED_ROW: END_OF_SCAN → return -1
        - If WAS_NULL: set *m_was_null
     e. Change to FETCH_NEXT_TUPLE, set m_current_row = next
     f. Return 0 (row found)
  4. If cache miss: change to FILLING_CACHE

FILLING_CACHE:
  1. Swap MEM_ROOT (source iterator uses THD's, not PTRC's)
  2. Read from source iterator
  3. Swap MEM_ROOT back
  4. If EOF:
     - If this scan previously had rows: return -1
     - Otherwise: cache the "no matched row" fact (NO_MATCHED_ROW flag)
  5. If source error (> 0): return error
  6. If row read successfully:
     - Handle row ID if needed
  7. Check hit ratio:
     - Every N misses (N = check_hit_ratio_frequency):
       if hits/(hits+misses) < min_hit_ratio → DisablePtrc(), go to BYPASS_MODE
  8. Insert new LRU_node at head of LRU list
  9. Set exec_flags (NO_MATCHED_ROW for EOF, WAS_NULL for null match)
  10. For subqueries: FillTmpTable() to store select list into tmp table fields
  11. Read result into res_buff
  12. StoreRow into hash buffer:
      - If BUFFER_FULL: attempt LRU_free_mem()
        - If still no space: undo LRU insertion, cache_overflows++, go to BYPASS_MODE
        - If space found: StoreRow at freed location
  13. Update current_LRU_node data from stored key/row pointers

FETCH_NEXT_TUPLE:
  1. If m_current_row == nullptr:
     - For subqueries: RestoreCacheItems()
     - Change to END_OF_SCAN → return -1
  2. Load m_current_row into res_tables
  3. Update LRU list
  4. Advance m_current_row = m_current_row.DecodeFixed().next
  5. Return 0

BYPASS_MODE:
  - Simply pass through to m_source_iterator->Read()
  - Handle row IDs if needed
```

### 5.8 LRU Eviction Algorithm

```
LRU_free_mem(thd, key, res_size):
  1. First check free_list for a slot with sufficient size
  2. Walk from LRU tail (oldest):
     - Skip entries with the same key as the one being inserted
     - When finding a removable entry:
       a. Find the batch start (all entries with same key)
       b. Remove entire batch (all entries with same key must go,
          otherwise incomplete results would be returned)
       c. If the slot size matches exactly: use it immediately
       d. Otherwise: add to free_list for later reuse
       e. Erase the key from the hash buffer
     - If no suitable entry found: return nullptr (cache overflow)
```

**Critical invariant**: All rows for a given key must be either fully cached or not cached at all. Partial caching would cause incorrect results because the lookup code assumes it can iterate through all matching rows from the cache.

### 5.9 Subquery Result Field Switching

For correlated subqueries, PTRC needs to store the subquery's SELECT list values and later restore them. This is done through a field switching mechanism:

```
During filling:
  FillTmpTable():
    for each visible field i:
      (*m_res_items)[i + num_hidden_fields]->save_in_field(res_tmp_table->field[i])
    → stores select list values into tmp table fields

  ChangeToUseTmpField():
    Copy cached_res_fields (Item_field objects wrapping tmp table fields)
    into m_res_items → replaces original Items with cached field readers

During cache hit:
  LoadIntoTableBuffers() loads the cached row into res_tables (including tmp table fields)
  ChangeToUseTmpField() switches select list to read from cached fields

During cleanup / bypass:
  RestoreCacheItems():
    Copy saved_res_items (original Items) back into m_res_items
```

### 5.10 Dynamic Hit Ratio Monitoring

PTRC monitors cache effectiveness at runtime and disables itself if hit ratio is too low:

```c++
if ((++stats.cache_misses) % check_hit_ratio_frequency == 0 &&
    (double)stats.cache_hits / (stats.cache_hits + stats.cache_misses) < min_hit_ratio) {
  DisablePtrc();  // Switch to BYPASS_MODE
}
```

This prevents PTRC from adding overhead when the cache is not beneficial for the actual data distribution.

---

## 6. Integration Points

### 6.1 Nested Loop Join Integration

```
sql/sql_executor.cc: CreateNestedLoopAccessPath()
  → inner = ptrc::CreateAccessPath(thd, nullptr, inner, outer_tables, inner_tables, join)
```

PTRC is automatically considered for every nested loop join where `join != nullptr` (i.e., the NLJ is not a semi/anti join). The decision is made during access path creation in the optimizer.

### 6.2 Correlated Subquery Integration

```
sql/sql_union.cc: Query_expression::create_access_paths()
  → For correlated subqueries (UNCACHEABLE_DEPENDENT, not RAND/SIDEEFFECT):
    m_root_access_path = ptrc::CreateAccessPath(thd, item, m_root_access_path, 0, 0, nullptr)
```

### 6.3 Iterator Creation

```
sql/join_optimizer/access_path.cc: CreateIteratorFromAccessPath()
  case AccessPath::PARTIAL_RESULT_CACHE:
    iterator = NewIterator<ptrc::PtrcIterator>(
        thd, mem_root, param->key_tables, param->res_tables, move(child),
        param->res_items, param->res_tmp_table, param->was_null,
        !param->is_exists_subquery);
```

### 6.4 Row ID Propagation

```
sql/join_optimizer/access_path.cc: FindTablesToGetRowidFor()
  case AccessPath::PARTIAL_RESULT_CACHE:
    path->ptrc().param->res_tables->set_store_rowids(true);
    path->ptrc().param->res_tables->set_tables_to_get_rowid_for(
        GetUsedTableMap(path) & ~handled_by_others);
```

PTRC must propagate row IDs from cached rows to parent iterators. When a cache hit occurs, `PtrcIterator::UnlockRow()` is a no-op (keeps the row locked since it may be returned again).

### 6.5 Cleanup

```
sql/sql_class.cc: THD::cleanup_after_query()
  if (!in_sub_stmt) ptrc::cleanup(this);

ptrc::cleanup(thd):
  delete thd->ptrc_objects;  // destroys PathParameters, frees MEM_ROOT
  thd->ptrc_objects = nullptr;
```

---

## 7. System Variables

| Variable | Type | Scope | Default | Description |
|----------|------|-------|---------|-------------|
| `optimizer_switch=partial_result_cache` | flag | session | ON | Enable/disable PTRC via optimizer_switch |
| `rds_partial_result_cache_max_mem_size` | ulonglong | session | 16MB | Maximum memory for PTRC hash buffer per statement |
| `rds_partial_result_cache_cost_threshold` | double | session | 0.5 | Minimum distinct value ratio to consider PTRC |
| `rds_partial_result_cache_min_hit_ratio` | double | session | 0.2 | Minimum runtime hit ratio; below this, PTRC disables itself |
| `rds_partial_result_cache_hit_ratio_frequency` | uint | session | 200 | Number of cache misses between hit ratio checks |

### Optimizer Switch

```c++
constexpr const uint64_t OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE(1ULL << 30);
```

Added to the `optimizer_switch` bitmap. When OFF, PTRC is never considered.

### Cost Threshold Semantics

`partial_result_cache_cost_threshold` is compared against `estimate_distinct_val_ratio`, which is:

```
ratio = (estimate_rows - distinct_rows) / estimate_rows
```

Higher ratio means more duplicate parameter values → more benefit from caching. If `ratio < threshold`, PTRC is not used. Default threshold of 0.5 means at least 50% of parameter values must be duplicates for PTRC to be considered.

---

## 8. Optimizer Hints

PTRC supports query block level hints to force or disable caching:

### Hint Syntax

```sql
-- Force PTRC for subquery in this query block
SELECT /*+ PTRC(@qb1) */ * FROM t1 WHERE a IN (SELECT /*+ QB_NAME(qb1) */ b FROM t2 WHERE c = t1.c);

-- Force PTRC for specific tables in nested loop join
SELECT /*+ PTRC(t2) */ * FROM t1 JOIN t2 ON t1.a = t2.b;

-- Disable PTRC
SELECT /*+ NO_PTRC(@qb1) */ * FROM t1 WHERE a IN (SELECT b FROM t2 WHERE c = t1.c);
```

### Hint Processing

```c++
process_ptrc_hints(sub, join, inner_tables, &force_to_use, &force_not_use):
  → Read from query_block->opt_hints_qb->apply_ptrc_hints()
  → For subqueries: force_subquery_ptrc / ignore_subquery_ptrc
  → For NLJ: force_join_ptrc / ignore_join_ptrc
  → Per-table: force_tab_map / ignore_tab_map
    - All inner tables must be forced for force_to_use
    - Any inner table ignored → force_not_use
```

When `force_to_use` is true:
- Skip cost threshold check
- Skip distinct value ratio check
- Still check suitability (deterministic, no BLOB, etc.)

---

## 9. EXPLAIN Output

### EXPLAIN FORMAT=TREE

```
-> Result cache : cache keys(t1.c) (Cache Hits: 5, Cache Misses: 2, ...)
    -> <inner plan>
```

Implemented by `ptrc::print()` which formats:
1. Cache key tables (TableCollection::print)
2. For EXPLAIN ANALYZE only: Instrumentation statistics (hits, misses, evictions, overflows, memory usage)

### Optimizer Trace

```
"partial_result_cache": {
  "caching_result_of": "subquery" | "nested_loop_join",
  "plan_prefix": ["t1"],
  "right_tables": ["t2"],
  "distinct_rows": 100,
  "estimate_rows": 1000,
  "estimate_distinct_val_ratio": 0.9,
  "chosen": true | false,
  "cause": "..." // if not chosen
}
```

---

## 10. Memory Management

### MEM_ROOT Swapping

PTRC uses a separate MEM_ROOT (`ptrc::Memory_objects::root`) with `set_max_capacity()` set to `partial_result_cache_max_mem_size`. The `PtrcMemRoot` RAII class temporarily swaps `THD::mem_root` with PTRC's MEM_ROOT during PTRC operations:

```c++
class PtrcMemRoot {
  explicit PtrcMemRoot(THD *thd) { std::swap(m_thd->mem_root, m_thd->ptrc_objects->saved_root); }
  ~PtrcMemRoot() { std::swap(m_thd->mem_root, m_thd->ptrc_objects->saved_root); }
};
```

This ensures:
- PTRC allocations count against the PTRC memory limit
- Source iterator allocations use the regular THD MEM_ROOT (not PTRC's limited one)
- When swapping back, source iterator memory is not charged to PTRC

### Buffer Full Handling

When `HashJoinRowBuffer::StoreRow()` returns `BUFFER_FULL`:
1. Attempt `LRU_free_mem()` to evict old entries
2. If successful: retry `StoreRow()` at the freed location
3. If no space available: increment `cache_overflows`, undo LRU insertion, switch to `BYPASS_MODE`

In BYPASS_MODE, the iterator simply passes through to the source iterator without caching, allowing the query to complete (albeit without PTRC benefit).

### Row Buffer Initialization

```c++
PtrcIterator::InitRowBuffer():
  m_row_buffer = new HashJoinRowBuffer(max_mem_size, mem_root);
  m_thd->mem_root->set_error_for_capacity_exceeded(true);  // allow fallback allocation
  m_row_buffer->Init(true);
  reserve_space(&key_buff, m_key_tables);   // pre-allocate key buffer
  reserve_space(&res_buff, m_res_tables);   // pre-allocate result buffer
```

If any step fails (OOM), PTRC is disabled and set to BYPASS_MODE. The error is caught via `Dummy_error_handler` and reported in optimizer trace.

---

## 11. Interaction with Other Features

### 11.1 Parallel Query (PQ)

PTRC and PQ can coexist. Known interaction:
- In `reset_record()`: PQ sets `TABLE::const_table = false` during `pq_dup_select`, so PTRC checks `table->pq_saved_const_table` instead
- PQ reverse index scan: PTRC must correctly handle reverse scan results (bug fix in commit #6)
- PTRC cache lookup uses `StoreFromTableBuffers` which includes NULL bits; these must be initialized even for columns outside read_set (handled by `reset_record()`)

### 11.2 Left Join Elimination

If left join elimination removes a table that PTRC references, the table's `QEP_shared_owner` may not be fully initialized. Commit #5 fixed this by checking for `nullptr` position in `estimate_num_groups()`.

### 11.3 Hash Join

PTRC reuses `HashJoinRowBuffer` infrastructure. The `TableCollection`, `StoreFromTableBuffers`, `LoadIntoTableBuffers`, `ComputeRowSizeUpperBound`, `ImmutableStringWithLength`, and `LinkedImmutableString` types all come from `sql/iterators/hash_join_buffer.h`.

### 11.4 Subquery Strategies

PTRC is only applied for correlated subqueries when:
- The subquery is `UNCACHEABLE_DEPENDENT` (correlated)
- But NOT `UNCACHEABLE_RAND` (non-deterministic) or `UNCACHEABLE_SIDEEFFECT`
- This excludes materialized subqueries (which already have their own caching)

For IN/EXISTS subqueries, PTRC handles:
- `was_null` tracking for IN subqueries (stored in `Exec_flags`)
- EXISTS subqueries: `m_need_fill_res_tables = false` (only need to know if any row exists, not the actual values)

---

## 12. Test Coverage

3 MTR test files in `mysql-test/suite/ptrc/t/`:

| Test File | Lines | Purpose |
|-----------|-------|---------|
| `partial_result_cache.test` | 863 | Core functionality: NLJ caching, subquery caching, BLOB exclusion, hints, cost threshold, hit ratio, eviction, overflow, EXPLAIN |
| `ptrc_bugs.test` | 958 | Bug regression tests: crashes, wrong results, interactions with PQ/left join elimination |
| `memory_used.test` | 15 | Memory tracking verification |

Additional test files:
| `dstore_main/t/partial_result_cache-dstore.test` | DStore-specific PTRC tests |

Test categories covered:
- Basic NLJ caching with duplicate outer keys
- Correlated subquery (IN, EXISTS, scalar) caching
- Cache miss and cache hit scenarios
- LRU eviction when memory is exceeded
- Cache overflow leading to BYPASS_MODE
- Dynamic hit ratio monitoring and auto-disable
- Optimizer hints (PTRC/NO_PTRC)
- Cost threshold filtering
- EXPLAIN FORMAT=TREE and EXPLAIN ANALYZE output
- Interaction with parallel query
- Left join elimination interaction
- Temporary table queries
- Reverse index scan with PQ
- BLOB columns (PTRC not used)
- Non-deterministic expressions (RAND)
- Subquery in HAVING clause (not supported)
- Outer query aggregation reference (not supported)
- Const plan detection

---

## 13. Error Handling

### OOM During Init

If `InitRowBuffer()` fails due to OOM:
- Error caught by `Dummy_error_handler`
- PTRC disabled, set to BYPASS_MODE
- Query continues without caching
- Logged in optimizer trace: `"disabled": true, "cause": "InitRowBuffer failed because of ER_OUTOFMEMORY"`

### Cache Overflow

When hash buffer is full and LRU cannot free enough space:
- `cache_overflows++`
- Switch to BYPASS_MODE for current scan
- On next rescan, status resets to LOOKUP, allowing a fresh attempt

### Dynamic Hit Ratio Failure

If runtime hit ratio falls below `partial_result_cache_min_hit_ratio`:
- PTRC disables itself for the remainder of the statement
- Logged in optimizer trace: `"disabled": true, "cause": "hit count is too low"`

### BYPASS_MODE Behavior

In BYPASS_MODE, `PtrcIterator::Read()` simply calls `m_source_iterator->Read()` and passes through results without any caching. Row IDs are still handled correctly.

---

## 14. Limitations

- **No BLOB support**: Tables with BLOB columns are excluded from PTRC
- **No non-deterministic expressions**: RAND() in conditions or subquery left_expr disables PTRC
- **No subquery in HAVING**: Cannot cache subqueries referenced in HAVING conditions
- **No outer-query aggregation**: Subqueries that aggregate in the outer query are excluded
- **Over-approximate key columns for NLJ**: All outer table read_set columns are used as cache keys, even if only some are needed for the join condition
- **Memory-only cache**: No disk spill; when memory is exhausted, cache entries are evicted or PTRC disables itself
- **No partial key caching**: All rows for a given key must be fully cached or not at all
- **Const plan exclusion**: PTRC is not used for queries with const plans
- **Subquery materialization exclusions**: When subquery materialization is used, PTRC is not applied (materialization is already a form of caching)

---

## 15. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **Add `OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE`** — Define the flag in `sql/sql_const.h`, add to `optimizer_switch` in `sys_vars.cc`
2. **Add session variables** — Define `partial_result_cache_max_mem_size`, `_cost_threshold`, `_min_hit_ratio`, `_check_hit_ratio_frequency` in `sys_vars.cc`
3. **Add `PARTIAL_RESULT_CACHE` to AccessPath** — Extend the union in `access_path.h`, add accessor methods
4. **Implement `PathParameters`** — Key/result table collections, tmp table management, pseudo context table
5. **Implement `Memory_objects` and `PtrcMemRoot`** — MEM_ROOT management with RAII swapping
6. **Implement key/result table construction** — `make_key_cols_tables()`, `make_res_cols_tables()`, `fill_table_collection()`
7. **Implement cost estimation** — `estimate_num_groups()`, `SetCostOnPtrcPath()`, `estimate_distinct_val_ratio()`
8. **Implement eligibility check** — `is_ptrc_suitable()`, `CreateAccessPath()`
9. **Implement `PtrcIterator`** — State machine, `Init()`, `Read()`, row buffer initialization
10. **Implement LRU management** — `LRU_node`, `LRU_list_type`, `insert_into_LRU_list()`, `update_LRU_list()`, `LRU_free_mem()`
11. **Implement subquery result switching** — `FillTmpTable()`, `ChangeToUseTmpField()`, `RestoreCacheItems()`
12. **Implement dynamic hit ratio monitoring** — Runtime check in `Read()`, `DisablePtrc()`
13. **Add `THD::ptrc_objects` member** — In `sql_class.h`, add cleanup in `sql_class.cc`
14. **Integrate with NLJ** — Call `ptrc::CreateAccessPath()` in `CreateNestedLoopAccessPath()`
15. **Integrate with subqueries** — Call `ptrc::CreateAccessPath()` in `Query_expression::create_access_paths()`
16. **Add iterator creation** — Handle `PARTIAL_RESULT_CACHE` in `CreateIteratorFromAccessPath()`
17. **Add row ID propagation** — Handle `PARTIAL_RESULT_CACHE` in `FindTablesToGetRowidFor()`
18. **Implement optimizer hints** — `Opt_hints_qb::ptrc_hints`, `apply_ptrc_hints()`, `register_ptrc_hint()`
19. **Add EXPLAIN output** — `ptrc::print()`, handle in `explain_access_path.cc`
20. **Port MTR test cases** — From `mysql-test/suite/ptrc/`
