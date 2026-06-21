# Session Plan Cache MR

## Title

sql: add session-local plan cache for prepared SELECT

## Summary

This MR adds a conservative session-local plan cache for prepared SELECT
statements. Eligible single-table plans are copied after normal optimization
and reused on later executions when session and table validation still match.
Unsupported shapes keep executing through the existing optimizer path.

## Problem

Repeated execution of the same prepared SELECT can pay the optimizer cost on
every execution even when the statement shape, table metadata, optimizer
settings, and parameter-sensitive plan structure are unchanged. The server
already keeps prepared statement state, but it does not keep a reusable
execution plan snapshot for this narrow session-local case.

## Design

High-level design:

- The first execution optimizes normally.
- After `make_join_readinfo()`, eligible plans are copied into a cached `JOIN`
  owned by the query block.
- The cached plan uses a dedicated `MEM_ROOT` and query arena.
- Later executions try `exec_cached_plan()` at the start of
  `Query_block::optimize()`.
- A hit requires matching optimizer switches, client character set, table
  metadata version, and allowed table row-count drift.
- Recoverable mismatches invalidate the plan and fall back to normal
  optimization.
- Fatal cached-apply errors are propagated.

Low-level implementation:

- Adds `sql/sql_plan_cache.cc` and `sql/sql_plan_cache.h`.
- Adds `Query_block::cached_plan` and `plan_cache_state`.
- Adds `JOIN::plan_cache_exec_context` and `JOIN::shallow_clone()`.
- Adds `plan_cache::Exec_context` for cached arena ownership, validation
  snapshots, transient item clones, and item-tree replay.
- Copies the supported single-table `QEP_TAB` shape and ref-family key data.
- Rebuilds ref keys from current prepared statement parameter values on each
  cached hit.
- Restores covering const/ref-family `key_read` behavior before handler reads.
- Adds cleanup and detach paths for query-block cleanup, destruction,
  repreparation, recoverable misses, and fatal errors.
- Adds session variables and status variables for enabling, invalidation
  policy, hits, live plans, and invalidations.
- Adds Performance Schema memory instrumentation for cached plan memory.

See `Docs/session-plan-cache-design.md` for the full design and scope
boundary.

## Supported cached-hit scenarios

- Session-local prepared `SELECT`.
- Single-table query blocks.
- Traditional optimizer path.
- `JT_ALL`, `JT_CONST`, `JT_REF`, `JT_EQ_REF`, and `JT_REF_OR_NULL` access.
- Covering const/ref-family reads.
- Restricted non-correlated scalar subqueries that remain cacheable and are not
  max/min transformed.
- Invalidation on optimizer-switch changes, client charset changes, table
  metadata changes, and configured row-count drift.

## Fallback scenarios

Fallback means the SQL statement is still supported and runs through normal
optimization; it only means the plan-cache hit path is not used.

Current fallback boundaries include:

- Multi-table joins and set operations.
- Range, MRR, index-merge, dynamic range, reversed access, and duplicate
  removal.
- Handler pushed ICP for non-covering ref-family plans.
- Temporary-table, aggregate, `GROUP BY`, `ORDER BY`, `DISTINCT`, `HAVING`,
  window, rollup, and CTE shapes.
- Correlated, non-scalar, uncacheable, and max/min-transformed scalar
  subqueries.
- Hypergraph optimizer plans, full-text access, secondary-engine tables,
  schema/system tables, temporary tables, and `mysql` or Performance Schema
  tables.

Future optimization work can expand these boundaries after adding explicit
state replay and cleanup for each shape.

## Test plan

Current MR-preparation verification was run from the existing `build/`
directory on 2026-06-21. `build/` was configured as Debug with `WITH_ASAN=0`
and `WITH_UNIT_TESTS=0`.

Build, passed:

```bash
cmake --build build --target mysqld -j 16
```

Focused MTR, passed with all 32 tests successful:

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

Whitespace check, passed:

```bash
git diff --check -- . \
  ':(exclude)mysql-test/r/mysqld--help-notwin.result' \
  ':(exclude)mysql-test/r/mysqld--help-win.result'
```

The full `git diff --check` command reports generated trailing spaces in
`mysqld--help-*.result` for `--rds-plan-cache-allow-change-ratio=# `. Those
spaces match generated `mysqld --help` output and are intentionally preserved
for result-file stability.

## Not included in this MR evidence

- Full release MTR.
- Full debug MTR beyond the focused debug MTR cases.
- Full ASAN/UBSAN MTR.
- Formal coverage report.
- Official sysbench performance result.

These should be run before final upstream acceptance. Existing local
performance smoke is engineering evidence only and should not be used as an
official performance claim.

## Follow-up opportunities

- Full replay for handler pushed ICP with cleanup of stale pushed conditions.
- `HAVING` cached-hit support with explicit `having_cond` clone/replay.
- Range and MRR replay after path regeneration and validation are designed.
- Temporary-table, aggregate, grouping, ordering, window, and CTE cached-hit
  support.
- Formal release-build sysbench benchmarking.
