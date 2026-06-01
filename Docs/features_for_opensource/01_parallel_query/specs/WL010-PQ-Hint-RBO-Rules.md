# WL#010: PQ Hint & RBO Rules

## Summary

Provide SQL hint syntax for users to control parallel query execution on a
per-statement basis. Implement rule-based optimization (RBO) rules to
automatically determine whether a query should execute in parallel and which
table to parallel-scan.

## Motivation

Fine-grained control over parallel execution is essential:
- Users may want to enable PQ for specific queries when it's disabled globally
- Users may want to disable PQ for specific queries when it's enabled globally
- Automatic table selection for parallel scan must follow cost and structural
  rules to avoid performance regressions

## Specification

### PQ Hint Syntax

**Enable parallel query:**

```sql
SELECT /*+ PQ [( [table_name] [degree] )]*/ ... FROM ...
```

| Hint | Description |
|------|-------------|
| `/*+ PQ() */` | Enable PQ with default DOP |
| `/*+ PQ(8) */` | Enable PQ with DOP=8 |
| `/*+ PQ(t2) */` | Enable PQ, specify t2 as parallel table, default DOP |
| `/*+ PQ(t2, 8) */` | Enable PQ, specify t2 and DOP=8 |

**Disable parallel query:**

```sql
SELECT /*+ NO_PQ */ ... FROM ...
```

### Hint Implementation

Two hint types are added:
- `PT_table_level_hint` (PQ_HINT) - specifies table and degree
- `PT_qb_level_hint` (NO_PQ_HINT) - disables PQ for the query block

**Parsing:** Hints are parsed and contextualized, setting `pq_dop` (for PQ_HINT)
and `no_pq` (for NO_PQ_HINT) on the THD.

**Table selection with hint:**

```c++
choose_parallel_tables():
  for each non-const primary table:
    pq_table_on = hint_table_state(thd, table_ref, PQ_HINT_ENUM, 0)
    if pq_table_on:
      if pq_check_not_support_tab(tab):
        push_warning("Table does not support parallel scan")
        continue
      tab = &qep_tab[i]  // select this table
      break
```

### RBO Rules for PQ Eligibility

The following rules are checked in `JOIN::suite_for_parallel_query()` and
`choose_parallel_tables()`:

**Hard exclusion rules (query level):**
- Not a SELECT statement
- Inside stored procedure or trigger
- Prepared statement execution
- Attachable transaction
- SERIALIZABLE isolation level

**Hard exclusion rules (table level):**
- Temporary table
- Non-InnoDB storage engine
- Fulltext search

**RBO cost/structure rules:**

| Rule | Description |
|------|-------------|
| `parallel_cost_threshold` | Query cost must exceed threshold |
| `primary_tables == const_tables` | No non-const tables to scan |
| `zero_result_cause` | Query produces no results |
| `SELECT DISTINCT` | Not supported (eliminated separately) |
| `COUNT(*)` | Handled by InnoDB parallel COUNT, not PQ |
| `MAX_FIELDS` | Too many fields in select list |
| `ROLLUP` | Not supported |
| Semi-join strategy | Materialization and DuplicateWeedout not supported |

**Table selection rules in `choose_parallel_tables()`:**

1. If PQ hint specifies a table, use that table (if eligible)
2. Otherwise, iterate through non-const primary tables
3. Check `pq_check_not_support_tab()` for each candidate
4. Select the first eligible table (currently; could use cost-based selection)

**Resource limit rules:**

| Rule | Description |
|------|-------------|
| `parallel_memory_limit` | Total PQ memory must not exceed limit |
| `parallel_max_threads` | Running PQ threads must not exceed limit |
| `parallel_queue_timeout` | Timeout waiting for PQ thread slot |

### Optimizer Switch

Parallel query can be controlled via `optimizer_switch`:

```
SET optimizer_switch='parallel_query=off';
```

This provides a session-level toggle without requiring hint syntax.

### Thread Pool Integration

When the thread pool is active, PQ workers are obtained from the thread pool
rather than creating new threads. This avoids oversubscription and ensures
PQ workers respect thread pool resource limits.

Key considerations:
- Workers are dispatched through the thread pool's task queue
- Worker priority may be adjusted based on the leader's priority
- Thread pool monitors PQ worker thread count

## References

- `PT_table_level_hint`, `PT_qb_level_hint` - hint parser
- `choose_parallel_tables` - table selection logic
- `hint_table_state` - hint state checking
- `pq_check_not_support_tab` - table eligibility check
- `parallel_cost_threshold`, `parallel_memory_limit` - system variables
