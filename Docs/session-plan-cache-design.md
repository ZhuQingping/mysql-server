# Session Plan Cache Design

## Summary

This change adds a session-local execution plan cache for prepared SELECT
statements. The first eligible execution is optimized normally. After the
optimizer has produced a safe single-table plan, the server copies the reusable
plan state into a dedicated cache lifetime. Later executions validate the
session and table environment and, when still safe, reuse the cached plan
instead of optimizing the same prepared statement again.

The feature is intentionally conservative. If a query shape is outside the
current cached-hit surface, the SQL statement still executes through the normal
MySQL optimizer and executor path. In this document, "fallback" means "do not
reuse a cached plan"; it does not mean that the SQL syntax or execution path is
unsupported by MySQL.

## Goals

- Cache plans only inside the owning session and prepared statement lifetime.
- Keep the first execution on the existing optimizer path.
- Reuse cached plans only after validating optimizer settings, character set,
  table metadata version, and table row-count drift.
- Keep unsupported or high-risk shapes on the normal optimizer path.
- Preserve result correctness before expanding the cached-hit surface.
- Expose minimal session controls and status counters for testing and
  observability.

## Non-goals

- No cross-session or global plan sharing.
- No ad hoc text query plan cache.
- No dependency on DStore, Parallel Query, or secondary-engine-specific logic.
- No hypergraph optimizer cached-hit support.
- No range, multi-range read, index-merge, temporary-table, grouping, ordering,
  window, or common-table-expression plan replay in this patch.

## User-visible interface

New session variables:

- `rds_plan_cache`: enables or disables plan-cache creation and hit use in the
  current session. The current patch defaults this variable to `ON`.
- `rds_plan_cache_allow_change_ratio`: invalidates a cached plan when the table
  row-count change ratio is greater than this value. A value of `0` disables
  row-count drift invalidation.

New status variables:

- `Cached_plan_hits`: session and global count of successful cached-plan hits.
- `Cached_plan_count`: global count of currently live cached plans.
- `Cached_plan_invalidations`: global count of invalidated cached plans.

Performance Schema adds the memory instrument
`memory/sql/plan_cache_mem_root` for cached plan memory.

## High-level design

1. During `Query_block::optimize()`, the server first checks whether a cached
   plan is already `READY` for the query block.
2. If a cached plan exists, `exec_cached_plan()` validates the environment:
   relevant optimizer switches, `character_set_client`, table metadata
   version, row-count drift, and absence of pending optimizer item-tree changes.
3. If validation succeeds, the cached `JOIN` and `QEP_TAB` state is rebound to
   the current execution's table objects, parameter-dependent ref keys are
   rebuilt, access paths are recreated, and the cached `JOIN` becomes the
   query block's active plan.
4. If validation fails without a fatal error, the cached plan is invalidated
   and the query falls back to normal optimization.
5. After normal optimization reaches `make_join_readinfo()`, `cache_plan()`
   checks whether the produced plan is eligible. If it is, the reusable state is
   copied into a dedicated `MEM_ROOT` and the query block state becomes
   `READY`.
6. Prepared-statement repreparation, query-block destruction, full cleanup, and
   fatal cached-apply errors destroy or detach cached state so table objects,
   handlers, item trees, and memory roots are not reused after their lifetime.

## Supported cached-hit surface

The current implementation supports cached hits for prepared statements with
these properties:

- `SQLCOM_SELECT` only.
- Session plan cache enabled.
- Single base table in the query block.
- Traditional optimizer path; hypergraph optimizer disabled for cached hits.
- Non-temporary, non-schema, non-system-view table.
- Not using the `mysql`, `information_schema`, or `performance_schema`
  schemas.
- No stored routine dependency and no in-query variable assignment.
- No locked-table/prelocked execution mode.
- Access type is one of:
  - `JT_ALL`
  - `JT_CONST`
  - `JT_REF`
  - `JT_EQ_REF`
  - `JT_REF_OR_NULL`
- Ref-family access has a copied `Index_lookup`, copied `Key_use` array, and
  per-execution ref-key reconstruction.
- Covering const/ref-family reads restore `TABLE::key_read` before handler
  reads on cached hits.
- Restricted scalar non-correlated subqueries are allowed when they are
  `SINGLEROW_SUBS`, not max/min transformed, and cacheable. Correlated,
  non-scalar, and max/min-transformed scalar shapes fall back.

## Fallback surface and future optimization points

The following shapes execute normally but do not currently use cached-plan
hits. They are kept as fallback to avoid replaying optimizer or handler state
before the replay rules are fully implemented and tested:

- Multi-table joins.
- Set operations.
- Temporary tables, schema tables, system views, and `mysql` or Performance
  Schema tables.
- Hypergraph optimizer plans.
- Full-text access and secondary-engine tables.
- Range, multi-range read, index-merge, dynamic range, reversed access, and
  duplicate-removal plans.
- Handler pushed index condition pushdown for non-covering ref-family plans.
  Future support must save and replay pushed conditions through handler APIs
  and include cleanup equivalent to the commercial patch-0010 class of fixes.
- `ORDER BY`, `GROUP BY`, `DISTINCT`, implicit grouping, aggregate temp-table
  plans, window functions, rollup, and common table expressions.
- Simple `HAVING` cached hits. Future support needs explicit `having_cond`
  clone/replay design.
- Non-scalar, correlated, uncacheable, or max/min-transformed scalar subquery
  items, including `ANY`/`ALL` wrappers that transform into unsupported items.
- Parameter transformations that remove, replace, or type-transform prepared
  statement parameters in a way the cached item replay cannot reproduce.

These are product limitations of the current plan-cache reuse surface, not SQL
compatibility limitations.

## Low-level design

### State ownership

`Query_block` owns:

- `cached_plan`: the cached `JOIN` for the query block.
- `plan_cache_state`: `NONE`, `START`, `READY`, or `UNCACHEABLE`.

`JOIN` owns:

- `plan_cache_exec_context`: an `Exec_context` allocated in the cached plan
  memory root.
- A shallow copy of reusable optimizer state.
- A copied `QEP_TAB` array for the supported single-table cached-hit surface.

`Exec_context` owns:

- The cached `Query_arena` and its dedicated `MEM_ROOT`.
- A flag showing whether the plan is included in `Cached_plan_count`.
- Environment snapshots for relevant optimizer switches,
  `character_set_client`, table metadata version, and table row count.
- A map from transient optimizer-created `Item` objects to their cached
  clones.
- A replay list for `THD::change_item_tree()` changes that must be applied
  before a cached hit can execute.

### Cache creation

`cache_plan()` runs after `make_join_readinfo()`. It first applies
`check_query_plan_cacheable()` and `is_supported_single_table_candidate()`.
The creation path:

- Allocates a dedicated cached-plan `MEM_ROOT`.
- Constructs a cached `JOIN`.
- Shallow-copies stable `JOIN` fields.
- Captures transient optimizer-created item state from the current execution
  root.
- Copies the supported single-table `QEP_TAB` shape.
- Clones transient WHERE and QEP conditions where required.
- Copies ref-family key metadata and parameter-dependent key values.
- Marks the cached plan `READY` only if a cached-hit-capable QEP snapshot was
  created successfully.

Unsupported non-error shapes are marked `UNCACHEABLE` and are not counted as
live cached plans.

### Cached apply

`exec_cached_plan()` runs at the start of `Query_block::optimize()`.
`apply_cached_plan_if_suitable()` checks:

- The cached plan and context exist.
- The table is open and still matches the cached table metadata version.
- Relevant optimizer switches and `character_set_client` match.
- Table row-count drift is within
  `rds_plan_cache_allow_change_ratio`.
- No pending item-tree change list needs optimizer-only state that cannot be
  replayed safely.

`apply_cached_plan()` then:

- Rebinds cached fields with `bind_fields()`.
- Replays cached item-tree changes.
- Binds the current `TABLE` and `Table_ref` into the cached `QEP_TAB`.
- Rebuilds ref keys from current prepared statement parameter values.
- Reads `JT_CONST` tables through `read_const_maybe_key_read()`.
- Restores covering ref-family `TABLE::key_read`.
- Recreates access paths and pushes the plan to engines.
- Marks the cached `JOIN` optimized and increments `Cached_plan_hits`.

Recoverable misses invalidate the cached plan and continue through normal
optimization. Fatal cached-apply errors are propagated rather than retried as
normal misses.

### Invalidation and cleanup

Cached plans are invalidated or destroyed when:

- The prepared statement is reprepared.
- Query-block cleanup or destruction reaches the cached plan owner.
- Optimizer switches, client character set, table metadata version, or
  row-count drift make the plan unsuitable.
- Cached apply hits a recoverable miss or a fatal error.

Cleanup detaches cached `QEP_TAB` entries from current `TABLE` objects, clears
handler key-read state, rolls back item-tree changes on apply failure, releases
cached arena items, and updates `Cached_plan_count`.

## Testing

The patch adds focused MTR coverage for:

- Basic lifecycle and deallocation.
- Cache hits for table scan, primary-key/const, secondary ref, composite ref,
  nullable ref, `ref_or_null`, and missing-row cases.
- Cache invalidation for optimizer switch, client charset, table metadata, and
  row-count drift.
- OFF/ON session variable behavior.
- Status counters and invalidation counter deltas.
- DML visibility on repeated prepared executions.
- Restricted scalar subquery hits and unsupported scalar fallback shapes.
- Generated-column item clone handling and parameter-transformation fallback.
- `remove_eq_conds()` item-address safety.
- Covering const/ref-family `key_read` replay on cached hits.
- Handler pushed ICP fallback for non-covering ref-family plans.
- Range, MRR, ORDER, GROUP, HAVING, aggregate, window, CTE, and temp-table
  fallback boundaries.
- Performance Schema memory instrumentation for cached plan allocation and
  release.
- Debug-only failure injection around clone, bind, access-path, engine-push,
  row-count, and cleanup paths.

The current focused regression command used for local acceptance is:

```bash
cd build/mysql-test
./mtr session_plan_cache_lifecycle session_plan_cache_hit \
  session_plan_cache_ref_hit session_plan_cache_ref_edge \
  session_plan_cache_invalidation session_plan_cache_eligibility \
  session_plan_cache_scalar_subquery session_plan_cache_scalar_regression \
  session_plan_cache_range_order session_plan_cache_timestamp_icp \
  session_plan_cache_mrr_boundary session_plan_cache_tmp_table_boundary \
  session_plan_cache_gcol_item_ref session_plan_cache_remove_eq_conds \
  session_plan_cache_unsupported session_plan_cache_cleanup_debug \
  session_plan_cache_hit_debug session_plan_cache_item_tracking \
  session_plan_cache_item_tracking_debug session_plan_cache_sysvar_off \
  session_plan_cache_invalidation_counter session_plan_cache_dml_visibility \
  session_plan_cache_scalar_fallback session_plan_cache_window_cte_boundary \
  session_plan_cache_pfs_memory session_plan_cache_const_keyread_debug \
  session_plan_cache_ref_keyread_debug session_plan_cache_ref_icp_boundary \
  perfschema.memory_key_descriptions main.ps main.1st
```

## Verification status and known gaps

Current local verification is focused on the feature-owned tests and build-tree
MTR runs. During MR preparation on 2026-06-21, the following commands were run
from the existing `build/` directory:

- `cmake --build build --target mysqld -j 16`: passed.
- The focused MTR command above: all 32 tests were successful.
- `git diff --check -- . ':(exclude)mysql-test/r/mysqld--help-notwin.result'
  ':(exclude)mysql-test/r/mysqld--help-win.result'`: passed.

The build used for this verification is a Debug build with `WITH_ASAN=0` and
`WITH_UNIT_TESTS=0`.

Full release MTR, full debug MTR, full ASAN/UBSAN MTR, formal code coverage,
and official sysbench performance evidence are not included in this patch
preparation and should be run before final upstream acceptance.

Earlier local performance smoke used isolated prepared-statement workloads to
confirm expected hit deltas and fallback controls. Those runs are engineering
evidence only and should not be presented as official performance claims.
