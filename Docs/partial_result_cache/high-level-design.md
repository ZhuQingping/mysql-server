# Partial Result Cache High Level Design

## Summary

Partial Result Cache (PRC/PTRC) accelerates SQL plans that repeatedly execute
the same parameterized inner work. It is most useful when a nested-loop join or
a correlated subquery sees duplicate outer parameter values and the inner path
is expensive enough to make cache lookup cheaper than repeated execution.

The feature is implemented as a new `AccessPath::PARTIAL_RESULT_CACHE` wrapper.
The wrapper owns the cache key definition, result-row definition, runtime
iterator state, and diagnostics. The wrapped child path remains the source of
truth for misses and fallback execution.

## Goals

- Cache partial inner results for eligible nested-loop join inner paths.
- Cache deterministic correlated subquery results.
- Preserve serial query semantics, including no-match, NULL, `was_null`, and
  multi-row result behavior.
- Bound memory per statement.
- Dynamically disable the cache when runtime hit ratio is too low.
- Expose plans and runtime counters through `EXPLAIN FORMAT=TREE`,
  `EXPLAIN ANALYZE FORMAT=TREE`, and optimizer trace.
- Keep unsupported shapes on the existing execution path.

## Non-Goals

- This is not a global query cache.
- Cached data is not shared across statements, sessions, transactions, or
  stored procedure statements.
- The feature does not change table data visibility or transaction semantics.
- It does not cache non-deterministic expressions.
- It does not optimize every TPC-H query. It benefits workloads with repeated
  parameter values and sufficient inner-path cost.
- It does not add aliases named `PTRC` or `NO_PTRC`; the implemented hint names
  are `PRC_*`.

## User Interface

Optimizer switch:

```sql
SET optimizer_switch='partial_result_cache=on';
SET optimizer_switch='partial_result_cache=off';
```

The switch is enabled by default in this port.

Session variables:

| Variable | Default | Meaning |
|---|---:|---|
| `rds_partial_result_cache_max_mem_size` | `16777216` | Maximum per-statement cache memory. |
| `rds_partial_result_cache_cost_threshold` | `0.5` | Estimated duplicate-value ratio threshold for choosing PRC unless forced. |
| `rds_partial_result_cache_min_hit_ratio` | `0.2` | Runtime hit-ratio floor before dynamic disablement. |
| `rds_partial_result_cache_hit_ratio_frequency` | `200` | Miss count interval for runtime hit-ratio checks. |

Hints:

| Hint | Scope |
|---|---|
| `PRC_SUBQUERY()` | Force correlated subquery PRC when the shape is otherwise supported. |
| `NO_PRC_SUBQUERY()` | Disable correlated subquery PRC. |
| `PRC_JOIN()` | Force nested-loop join PRC globally in the query block. |
| `PRC_JOIN(table_list)` | Force nested-loop join PRC when the wrapped inner side is covered by the table list. |
| `NO_PRC_JOIN()` | Disable nested-loop join PRC globally in the query block. |
| `NO_PRC_JOIN(table_list)` | Disable nested-loop join PRC for matching inner tables. |

Diagnostics:

```text
Result cache : cache keys(...)
```

With `EXPLAIN ANALYZE FORMAT=TREE`, the same line includes:

```text
Cache Hits, Cache Misses, Cache Evictions, Cache Overflows, Memory Usage
```

Optimizer trace records `partial_result_cache` chosen, rejected, and runtime
disablement details where the implementation reaches PRC eligibility or
runtime decision code.

## Supported Plan Shapes

### Nested-Loop Join

For eligible non-anti and non-semi nested-loop joins, the optimizer can wrap the
inner access path:

```text
Nested loop join
  outer path
  Result cache : cache keys(outer columns)
    inner path
```

The cache key is built from the selected outer tables. The cached result is
built from the selected inner tables. On a miss, the child iterator runs and the
result batch is stored. On a hit, cached rows are replayed.

### Correlated Subquery

For eligible simple dependent query expressions, PRC can wrap the subquery root
access path. The cache key is built from dependent outer parameters. The cached
result is stored through a temporary result table abstraction so the outer
expression can consume cached values without rerunning the subquery.

The implementation covers scalar, `EXISTS`, `IN`, `ANY`, and `ALL` semantics
for the supported deterministic shapes.

## Eligibility And Fallback

PRC is rejected or bypassed for unsupported shapes. Important examples:

- feature disabled by `optimizer_switch` or a `NO_PRC_*` hint;
- non-`SELECT` statements;
- non-deterministic expressions, including `RAND()`;
- zero-row paths;
- const-only plans;
- dependent set-operation or non-simple subqueries;
- unsupported HAVING placement;
- BLOB/TEXT cache-key or result-table shapes that cannot be safely packed;
- estimated duplicate-value ratio below the configured threshold when not
  forced by hint;
- runtime hit ratio below the configured threshold.

Unsupported cases keep the original child access path or enter bypass mode.
Runtime bypass is sticky for the lifetime of the iterator instance.

## Memory And Lifetime

Each statement owns its PRC memory through `THD::ptrc_objects`. The memory root
is capped by `rds_partial_result_cache_max_mem_size`. PRC objects are cleaned
from `THD::cleanup_after_query()` so statement-local caches do not leak across
statements.

The iterator stores key and result rows in a hash-row buffer and uses an LRU
list for eviction. Eviction removes complete batches for a key; returning an
incomplete cached result set is not allowed.

## Safety Model

Correctness is prioritized over caching:

- Misses and unsupported paths execute the existing child iterator.
- OOM or cache initialization failure disables PRC and falls back to the child
  iterator.
- Low-hit-ratio runtime disablement keeps query results unchanged.
- Correlated subquery `was_null` and no-match state are cached with result data.
- Row IDs needed by upper iterators are requested for cached rows.

## Performance Model

PRC helps when:

- outer parameter values repeat;
- the inner path cost is non-trivial;
- cache key/result row size fits the memory cap;
- runtime hit ratio stays above the configured floor.

PRC can be neutral or slower when keys are mostly unique or inner work is
already cheap. In that case the cost threshold, runtime hit-ratio disablement,
and hints control exposure.

## Public Limitations

- The feature is statement-local and does not persist cached rows.
- The cache is limited by statement memory and may enter bypass mode when
  useful reuse is low or cache storage cannot be initialized.
- It targets repeated dependent work. Queries whose outer keys are mostly
  unique, whose inner work is already cheap, or whose shape is unsupported can
  see no benefit and stay on the original execution path.
- Performance and validation evidence is documented separately in the test and
  performance reports.
