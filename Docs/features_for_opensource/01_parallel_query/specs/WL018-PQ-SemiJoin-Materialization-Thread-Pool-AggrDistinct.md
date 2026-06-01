# WL#018: PQ Semi-Join Materialization, Thread Pool & Aggr(Distinct)

## Summary

Extend parallel query to support semi-join materialization strategy, thread
pool integration, and aggregation with DISTINCT. These features fill gaps
in PQ coverage for common query patterns and deployment scenarios.

## Specification

### Part 1: Semi-Join Materialization Parallel

Semi-join has four execution strategies: FirstMatch, LooseScan,
DuplicateWeedout, and Materialization. The first three were already
supported in PQ. This adds support for Materialization.

**Two-phase parallel execution:**

1. **Phase 1:** Materialize the SJ inner table into a physical temporary
   table with deduplication by SJ key (executed by the leader thread only,
   since the SJ inner query is flattened into the outer query and cannot
   be independently cloned)
2. **Phase 2:** Execute the SJ outer query in parallel, with workers
   reading from the shared materialized table

**Clone Semijoin_mat_exec:**

```c++
pq_dup_tabs
  -> if orig_tab->sj_mat_exec():
     -> pq_clone_sj_mat_exec()
        -> new NESTED_JOIN, TABLE_LIST, Semijoin_mat_exec
        -> create_tmp_table()  // new temp table for worker's SJ Mat
```

**Phase 1 execution (leader):**

```c++
ParallelScanIterator::Init
  -> para_exec_init
    -> MaterializeIterator::init_shared_materialize_table
      -> MaterializeIterator::Init  // materialize inner table
      -> *m_qep_tab->m_pq_shared_materialized_table =
           table()->file->get_shared_info()  // set shared info
```

**Phase 2 execution (workers):**

- Workers share the materialized temporary table (same mechanism as
  derived tables)
- Workers set `m_shared_info` from the leader's table
- Workers skip creating `MaterializeIterator::query_block` objects
  (subquery already materialized)

```c++
QEP_TAB {
  void **m_pq_shared_materialized_table;  // shared table info pointer
};

// Worker side:
MaterializeIterator::Init
  -> table()->file->set_shared_info(_shared_info)

// Worker skips subquery iterator creation:
CreateIteratorFromAccessPath
  -> case MATERIALIZE:
     if sharing_materialized_table(..):
       skip create MaterializeIterator::QueryBlock
```

**Performance:** TPC-H Q16 variant: 44.52s -> 6.55s (7-8x speedup)

### Part 2: Thread Pool Integration

When the MySQL thread pool plugin is active, PQ workers are dispatched
through the thread pool rather than creating dedicated threads.

**Key design points:**
- Workers are submitted as tasks to the thread pool's high-priority queue
- The leader thread waits for workers to complete (not blocked in pool)
- Thread pool monitors PQ worker thread count to prevent oversubscription
- When the thread pool is full, PQ falls back to creating threads or
  refusing the parallel execution

**Thread pool configuration interaction:**
- `thread_pool_size` affects available workers
- PQ respects `thread_pool_max_threads` limit
- Worker priority can be adjusted based on leader priority

### Part 3: Aggregation with DISTINCT

`SELECT COUNT(DISTINCT col) FROM t1` requires reading all rows of a group
to compute the distinct count. In the basic PQ design, workers only have
partial data, making DISTINCT aggregation incorrect.

**Solution: Group reshuffle**

When the query contains `aggr(DISTINCT)`, the group reshuffle mechanism
(WL#003) is used to ensure all rows of a group are on the same backend
worker. The backend worker can then correctly compute `COUNT(DISTINCT)`
without leader intervention.

**Without group reshuffle:** `aggr(DISTINCT)` queries cannot be parallelized
because workers would compute incorrect distinct counts on partial data.

### Part 4: InnoDB Code Refactoring

The InnoDB parallel scan module was refactored to separate leader and worker
roles more clearly:

- **Leader role:** Data partition, scan initialization, thread management
- **Worker role:** Data scanning, visibility checking, record conversion

This refactoring improved code maintainability and made it easier to add
new scan types (e.g., partition table scan, dependent-ref scan).

## References

- `Design/HLD/SemiJoin-Materialized-Exec-Doc.md` - semi-join materialization
- `Design/HLD/SemiJoin-materialized-PQ.md` - Chinese version
- `Design/HLD/线程池.md` - thread pool integration
- `Design/HLD/Aggr(distinct)并行设计.md` - distinct aggregation
- `Design/HLD/InnoDB代码重构设计.md` - InnoDB refactoring
- `new-Parallel-query-InnoDB-module.md` - new module design
