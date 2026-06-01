# WL#011: PQ Scalar Subquery Parallel Execution

## Summary

Implement parallel execution for independent scalar subqueries located in
field_list, WHERE/HAVING conditions, and ORDER BY clauses. The scalar
subquery is executed first and its result is cached; the outer query then
shares the cached result across all workers.

## Motivation

Scalar subqueries like `SELECT id, (SELECT AVG(b) FROM t2) FROM t1` are
common in analytical queries (e.g., TPC-H Q11, Q15, Q22). Without parallel
support, these queries remain single-threaded. Benchmarks show 1.6x-17x
speedup on TPC-H with this feature.

## Specification

### Supported Scalar Subquery Positions

- Field list: `SELECT id, (SELECT AVG(b) FROM t2) AS t FROM t1`
- WHERE condition: `SELECT * FROM t1 WHERE a > (SELECT AVG(b) FROM t2)`
- Expression argument: `SELECT id, SUM((SELECT AVG(b) FROM t2)) FROM t1 GROUP BY id`
- ORDER BY clause: `SELECT id FROM t1 ORDER BY (SELECT AVG(b) FROM t2)`
- HAVING condition: `SELECT SUM(a), (SELECT SUM(b) FROM t2) AS scalar FROM t1 HAVING SUM(a) > scalar`

### Execution Model

Two-step execution:
1. **Execute the scalar subquery** and cache the result in `Item_cache`
   (shared between leader and workers)
2. **Execute the outer query** in parallel, with workers reading the
   cached scalar result

Both the scalar subquery and the outer query can independently be executed
in parallel mode.

### Generating the Execution Plan

During optimization, the new method `SELECT_LEX_UNIT::scalar_independent_subquery()`
determines if the scalar subquery is eligible for PQ. If so, the PQ execution
plan is generated via `make_pq_unit_plan()`.

The cached result is stored in `Item_singlerow_subselect`'s row, where
leader and worker share the same element.

### Executing the Scalar Subquery

The scalar subquery is executed during `ParallelScanIterator::Init()`,
before workers are launched:

```c++
ParallelScanIterator::Init() {
  para_exec_init(m_root_access_path, m_join)    // materialized derived tables
  exec_scalar_independent_subquery()              // scalar subqueries
  pq_init_record_gather()                         // init message queue
  pq_launch_worker()                              // launch workers
}
```

The traversal covers all potential scalar subquery locations:

```c++
exec_scalar_independent_subquery() {
  // field_list items (including ORDER BY formed items)
  for (auto item : m_join->select_lex->fields) {
    if (item->execute_inner_subquery()) return true;
  }
  // WHERE condition
  if (where_cond) return where_cond->execute_inner_subquery();
  // HAVING condition
  if (having_cond) return having_cond->execute_inner_subquery();
}
```

Each scalar subquery is wrapped in `Item_singlerow_subselect`, which uses
`exec()` to obtain and cache the result:

```c++
bool Item_singlerow_subselect::execute_inner_subquery() {
  if (!assigned()) {
    bool res = exec(current_thd);  // result cached into row
    assigned(true);
    return res;
  }
  return false;
}
```

### Performance

100GB TPC-H, `parallel_default_dop = 32`:

| Query | Non-PQ | PQ(32) | Speedup |
|-------|--------|--------|---------|
| Q11   | 21.7s  | 13.14s | 1.6x    |
| Q15   | 521.1s | 138s   | 3.8x    |
| Q22   | 24.2s  | 1.4s   | 17.3x   |

### Future Work

Currently, only one query block is cloned per phase. Future enhancement:
workers could execute the outer query while dynamically triggering scalar
subquery execution on the leader, allowing multiple divided tables to exist
simultaneously.

## References

- `Design/HLD/Parallel-scalar-subquery.md`
- `exec_scalar_independent_subquery` - scalar subquery execution entry
- `Item_singlerow_subselect::execute_inner_subquery` - per-item execution
