# Partial Result Cache Low Level Design

## Source Layout

Main implementation:

- `sql/partial_result_cache.h`
- `sql/partial_result_cache.cc`

Build integration:

- `sql/CMakeLists.txt`

Optimizer and executor integration:

- `sql/sql_const.h`
- `sql/sys_vars.cc`
- `sql/system_variables.h`
- `sql/sql_hints.yy`
- `sql/lex.h`
- `sql/opt_hints.h`
- `sql/opt_hints.cc`
- `sql/parse_tree_hints.cc`
- `sql/sql_lex_hints.cc`
- `sql/join_optimizer/access_path.h`
- `sql/join_optimizer/access_path.cc`
- `sql/join_optimizer/explain_access_path.cc`
- `sql/join_optimizer/walk_access_paths.h`
- `sql/join_optimizer/join_optimizer.cc`
- `sql/sql_executor.cc`
- `sql/sql_union.cc`
- `sql/item_subselect.h`
- `sql/item_subselect.cc`
- `sql/sql_class.h`
- `sql/sql_class.cc`

Shared row-packing infrastructure:

- `sql/pack_rows.h`
- `sql/pack_rows.cc`
- `sql/immutable_string.h`
- `sql/iterators/hash_join_buffer.h`
- `sql/iterators/hash_join_buffer.cc`
- `sql/table.h`
- `sql/error_handler.h`
- `sql/psi_memory_key.h`
- `sql/psi_memory_key.cc`

## AccessPath Representation

`AccessPath` has a new type:

```c++
AccessPath::PARTIAL_RESULT_CACHE
```

The `partial_result_cache` union member stores:

```c++
AccessPath *child;
ptrc::PathParameters *param;
```

`ptrc::PathParameters` owns:

- key table collection;
- result table collection;
- correlated subquery result item metadata;
- temporary tables used to store subquery result values and context flags;
- `was_null` pointer for `IN`/`ANY`/`ALL` handling;
- an `is_exists_subquery` flag used to avoid unnecessary result-table filling.

## SQL Interface Wiring

`optimizer_switch=partial_result_cache` is registered as a normal optimizer
switch flag and is enabled in `OPTIMIZER_SWITCH_DEFAULT`. In the MySQL 8.0.46
port, `OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE` uses bit `1ULL << 26`.

The four session variables are:

| Variable | Default | Implementation note |
|---|---:|---|
| `rds_partial_result_cache_max_mem_size` | `16777216` | Per-statement PRC memory cap. |
| `rds_partial_result_cache_cost_threshold` | `0.5` | Estimated duplicate/distinct ratio threshold, not an absolute cost. |
| `rds_partial_result_cache_min_hit_ratio` | `0.2` | Runtime hit-ratio floor before bypass. |
| `rds_partial_result_cache_hit_ratio_frequency` | `200` | Miss-count interval for runtime hit-ratio checks. |

The registered hint names are `PRC_SUBQUERY`, `NO_PRC_SUBQUERY`, `PRC_JOIN`,
and `NO_PRC_JOIN`. `PTRC` and `NO_PTRC` are not registered aliases.

## Optimizer Hooks

### Nested-Loop Join

`CreateNestedLoopAccessPath()` in `sql/sql_executor.cc` receives the current
outer-prefix and inner-table maps. For non-anti and non-semi joins, it calls:

```c++
ptrc::CreateAccessPath(thd, nullptr, inner, outer_tables, inner_tables, join)
```

If PRC is not suitable, `CreateAccessPath()` returns the original `inner` path.

### Correlated Subquery

`Query_expression::create_access_paths()` in `sql/sql_union.cc` uses a scope
guard after building the root access path. It attempts PRC only when:

- the query expression is simple;
- the expression is dependent;
- it is not marked with RAND or side effects;
- the root access path exists.

The call shape is:

```c++
ptrc::CreateAccessPath(thd, item, m_root_access_path, 0, 0, nullptr)
```

## Eligibility Processing

`ptrc::CreateAccessPath()` performs these steps:

1. Reject invalid call combinations or non-simple subqueries.
2. Read `PRC_*` and `NO_PRC_*` hints through `Opt_hints_qb::apply_ptrc_hints()`.
3. Check `optimizer_switch=partial_result_cache` unless a force hint applies.
4. Reject non-deterministic or unsupported shapes through `is_ptrc_suitable()`.
5. Allocate `THD::ptrc_objects` and the PRC `MEM_ROOT` if needed.
6. Build key table collection from dependent parameters or join outer tables.
7. Reject empty key collections and BLOB/TEXT key shapes.
8. Estimate duplicate-value ratio unless forced.
9. Allocate the wrapper `AccessPath` and `PathParameters`.
10. Build result table collection from join inner tables or subquery result
    items.
11. Compute cost and trace `chosen=true`.

Rejected paths trace a concrete cause where practical and keep the child path.

## Cache Key And Result Packing

PRC reuses table-buffer packing helpers from `pack_rows` and hash join row
buffer infrastructure.

Key data:

- Nested-loop join keys are packed from the selected outer tables.
- Subquery keys are packed from dependent outer parameter fields.

Result data:

- Nested-loop join results are packed from selected inner tables.
- Correlated subquery results are packed from a temporary result table
  generated from visible subquery result items.

`TableCollection` rejects BLOB/TEXT shapes for this feature. The packing layer
also supports row ID collection for plans where upper iterators need row IDs.

## Runtime State Machine

`ptrc::PtrcIterator` is a `RowIterator` wrapper around the child iterator. It
uses this state machine:

| State | Meaning |
|---|---|
| `NONE` | Initial state before iterator initialization. |
| `LOOKUP` | Build key and search the cache. |
| `FILLING_CACHE` | Cache miss; run child and store rows for the key. |
| `FETCH_NEXT_TUPLE` | Cache hit; replay next cached row for the key. |
| `BYPASS_MODE` | PRC disabled; delegate to child iterator. |
| `END_OF_SCAN` | Current key has no more cached rows. |

`BYPASS_MODE` is sticky for the current iterator lifetime. `Init()` on a
bypassed iterator initializes the child path directly instead of retrying PRC
setup for later rescans.

Read path:

1. In `LOOKUP`, pack the key and search the row buffer.
2. On hit, load cached row data into table buffers, restore subquery result
   items where needed, update LRU, and return rows.
3. On miss, switch to `FILLING_CACHE` and call the child iterator.
4. Store every result row with the current key.
5. Store no-match and `was_null` flags in a pseudo context table.
6. If memory is full, evict a complete LRU key batch or enter bypass mode.

## Memory Root Handling

`ptrc::Memory_objects` is attached to `THD::ptrc_objects` and owns:

- PRC `MEM_ROOT`;
- saved original `THD::mem_root`;
- allocated `PathParameters`.

`PtrcMemRoot` swaps `THD::mem_root` while PRC allocates cache metadata and row
buffer memory. Child iterator reads run with the original `THD::mem_root` by
temporarily swapping back.

`ptrc::cleanup(thd)` deletes `THD::ptrc_objects` after statement execution.

## LRU Eviction And Overflow

Each stored row has an LRU node containing encoded key and row metadata. A key
can map to multiple result rows. Eviction removes whole batches for the same
key so a later hit never returns a partial result set.

If a row cannot fit even after eviction, PRC increments cache overflow
instrumentation and falls back to bypass behavior.

## Correlated Subquery Result Handling

Subquery PRC must preserve expression semantics, not only table buffers. The
iterator therefore:

- builds a temporary table matching visible result items;
- creates cached `Item_field` objects over the temporary table fields;
- swaps subquery result items to cached fields on cache hits;
- restores original result items when the cached scan ends or bypasses;
- stores no-match and `was_null` flags in the pseudo context table.

For `EXISTS`, result-table filling can be skipped because the existence state
is represented by context flags.

## Costing And Runtime Disablement

For joins, the wrapper estimates:

- cache entry size from key and result row upper bounds;
- number of cacheable entries from memory cap;
- distinct key count and total rows from optimizer statistics;
- eviction ratio and hit ratio.

If the estimated duplicate-value ratio is below
`rds_partial_result_cache_cost_threshold`, PRC is not chosen unless forced by
hint.

At runtime, every `rds_partial_result_cache_hit_ratio_frequency` misses, the
iterator compares actual hit ratio with
`rds_partial_result_cache_min_hit_ratio`. If the ratio is too low, it records
optimizer trace cause `hit count is too low` and enters bypass mode.

## Diagnostics

`EXPLAIN FORMAT=TREE` prints:

```text
Result cache : cache keys(...)
```

`EXPLAIN ANALYZE FORMAT=TREE` appends:

```text
Cache Hits: N, Cache Misses: N, Cache Evictions: N,
Cache Overflows: N, Memory Usage: N
```

Optimizer trace records:

- `caching_result_of`: `nested_loop_join` or `subquery`;
- plan prefix and right tables for joins;
- duplicate-value estimate;
- `chosen` state;
- rejection or force-hint cause where the implementation records a traced
  decision;
- runtime disablement cause.

## Error And Fallback Behavior

PRC favors fallback over hard failure:

- allocation failures during row-buffer initialization disable PRC and trace
  `InitRowBuffer failed because of ER_OUTOFMEMORY`;
- row storage failures and overflows switch to bypass where possible;
- unsupported shapes return the original child path;
- result item state is restored before leaving cached subquery mode.

Child iterator errors are returned unchanged.

## Test Hooks

The implementation includes debug hooks used by MTR:

- `ptrc_initrowbuffer_reserve_space_fail`
- `ptrc_init_row_buffer_fail`

These hooks validate OOM and fallback paths.
