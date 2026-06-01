# WL#014: PQ Correlated Subquery & NST Clone

## Summary

Enable parallel execution for correlated subqueries by cloning the entire
Nested Select_lex Tree (NST). This allows each worker to independently
execute the full query structure including subqueries, rather than requiring
the leader to pre-materialize subquery results.

## Motivation

Correlated subqueries reference columns from outer query blocks, making them
inherently dependent on the outer query's execution state. Previously, PQ
could only handle materialized (uncorrelated) subqueries. By cloning the
entire NST, each worker can execute its portion of the outer query and
resolve correlated subqueries independently.

## Specification

### Nested Select_lex Tree (NST)

MySQL organizes subqueries as nested `SELECT_LEX_UNIT -> SELECT_LEX`
structures. For example:

```sql
SELECT .. FROM t1 WHERE a IN (SELECT .. FROM t2 UNION SELECT .. FROM t3)
```

```
SELECT_LEX_UNIT
  -> SELECT_LEX (qb1: t1)
    -> SELECT_LEX_UNIT
      -> SELECT_LEX(qb2: t2) -> SELECT_LEX(qb3: t3)
```

### Clone NST

The entire NST is cloned recursively using `pq_clone`:

```c++
SELECT_LEX_UNIT::pq_clone(thd, lex, outer):
  unit = new SELECT_LEX_UNIT(CTX_NONE)
  for each select_lex in unit:
    clone_sl = sl->pq_clone(thd, lex, unit)
    link clone_sl into unit
  if outer: unit->include_down(lex, outer)

SELECT_LEX::pq_clone(thd, lex, outer):
  sl = lex->new_query(nullptr)
  for each inner unit:
    new_unit = unit->pq_clone(thd, lex, sl)
  if outer: sl->include_down(lex, outer)
```

After cloning the structure, resolved members are copied via nested
`pq_copy_from` calls.

### Prepare Each Query Block

```c++
pq_prepare(thd, orig, clone):
  open_tables_for_query(thd, lex->query_tables, 0)
  pq_prepare_unit(thd, orig, clone)

pq_prepare_unit(thd, orig, clone):
  for each select_lex:
    pq_prepare_select(thd, orig_select, clone_select)
    // Item::fix_fields() recursively resolves inner subqueries
```

### Optimize Each Query Block

```c++
pq_optimize_unit(thd, orig, clone, shared_table):
  for each select_lex:
    if clone->set_limit(thd, clone_select): return true
    pq_optimize_select(thd, orig_select, clone_select)
  // Setup materialized table for derived tables
  clone->create_access_paths(thd)
  clone->create_iterator_from_accesspath(thd)
```

### Generate PQ Leader Plan

```c++
make_pq_plan(thd, orig):
  for each select_lex:
    if sel->join->suite_for_parallel_query(): suite_for_pq = true
  if !suite_for_pq: return false

  new_thd = pq_new_thd(thd)
  clone = clone_execution_plan(new_thd, orig)
  generate_gather_node(new_thd, orig, clone)

  for each select_lex:
    if sel->parallel_exec: make_pq_leader_plan(thd, sel->join)

  orig->create_access_paths(thd)
  orig->create_iterator_from_accesspath(thd)
```

### Generate PQ Worker Plan

For UNION queries, workers cannot simply execute all query blocks
simultaneously because `S1 UNION S2 != (S11 U S12) UNION (S21 U S22)`.

**Solution:** Add a fake `select_lex_unit` for each worker's cloned
select_lex during execution:

```c++
pq_worker_execute_query(exec_thd, mngr):
  select = gather->m_template_join->select_lex
  saved_master = select->master_unit()
  saved_next = select->next_select()
  orig = select->add_fake_unit(exec_thd)
  clone = clone_execution_plan(exec_thd, orig)
  mngr->signal_status(exec_thd, PQ_worker_state::READY)
  clone->ExecuteIteratorQuery(exec_thd)
  // Restore nesting structure
  select->set_master_unit(saved_master)
  select->set_next_select(saved_next)
```

### Performance

100GB TPC-H, `parallel_default_dop = 32`:

| Query | Non-PQ | PQ(32) | Speedup |
|-------|--------|--------|---------|
| Q2    | 14.79s | 1.19s  | 12.4x   |
| Q17   | 35.92s | 7.22s  | 4.98x   |
| Q20   | 103s   | 72s    | 1.43x   |

### Limitations

- Each worker clones the entire NST and executes subqueries independently,
  which is inefficient for uncorrelated subqueries (results could be shared)
- Future work: share uncorrelated subquery results across workers

## References

- `HLD/PQ-support-for-correlated-subquery.md`
- `pq_dup_unit`, `pq_clone` - NST cloning functions
- `pq_prepare`, `pq_optimize` - query block preparation and optimization
- `pq_worker_execute_query` - worker execution with fake unit
