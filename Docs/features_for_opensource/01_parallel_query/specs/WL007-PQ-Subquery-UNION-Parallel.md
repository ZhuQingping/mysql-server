# WL#007: PQ Subquery & UNION Parallel Execution

## Summary

Extend parallel query to support UNION/UNION ALL queries and materialized
subqueries (derived tables, CTEs). UNION queries are parallelized per query
block. Materialized subqueries use a two-phase parallel execution model.

## Motivation

UNION queries and derived tables are common in complex analytical workloads.
Without parallel support, these queries remain single-threaded even when
individual query blocks could benefit from parallelism.

## Specification

### UNION Parallel Execution

#### SELECT_LEX & SELECT_LEX_UNIT

- `SELECT_LEX`: query block (SELECT keyword, table list, WHERE, GROUP BY)
- `SELECT_LEX_UNIT`: query expression (single block or UNION of blocks)

```
Simple:   unit -> select_lex(t1)
UNION:    unit -> select_lex(t1) -> select_lex(t2)
Subquery: unit -> select_lex(t1) -> unit -> select_lex(t11) -> select_lex(t12)
```

#### UNION/UNION ALL Iterator Model

- `UNION DISTINCT`: creates `MaterializeIterator` (dedup required)
- `UNION ALL`: creates `AppendIterator` (streaming)

#### Parallel Plan Generation

The `make_pq_unit_plan()` function handles parallel plan generation for
`SELECT_LEX_UNIT`:

```c++
make_pq_unit_plan(unit, thd):
  if unit->is_simple():
    join = unit->first_select()->join
    if join && join->suite_for_parallel_query():
      make_pq_leader_plan(join, thd)
  else if unit->fake_select_lex == nullptr:  // UNION ALL
    for each select_lex in unit:
      if join->suite_for_parallel_query():
        make_pq_leader_plan(join, thd)
  unit->create_iterators(thd)
```

For `UNION DISTINCT`, parallel execution plans are created in
`setup_materialization()` before iterator creation:

```c++
SELECT_LEX_UNIT::setup_materialization():
  for each select_lex:
    if join->suite_for_parallel_query():
      make_pq_leader_plan(join, thd)
    query_block.subquery_iterator = join->release_root_iterator()
```

### Materialized Subquery Parallel Execution

Materialized subqueries (derived tables, CTEs) require two-phase parallel
execution:

- **Phase I:** Inner subquery executes in parallel, writes results to
  materialized table
- **Phase II:** Outer query executes in parallel, reading from the
  materialized table

#### Rebuilding Execution Tree

The original on-the-fly materialization is changed: inner subqueries are
materialized upfront during `ParallelScanIterator::Init()`.

```c++
para_exec_init(iterator):
  if !iterator: return false
  childs = iterator->children()
  for curr_iter in childs:
    if para_exec_init(curr_iter): return true
  return iterator->pq_init()  // calls Init() to materialize
```

The execution tree is rebuilt so that:
- Inner subquery: `MaterializeIterator -> TMP table -> ParallelScanIterator`
- Outer query: `ParallelScanIterator` (which triggers inner materialization
  during Init())

#### Sharing the Materialized Temporary Table

The key challenge: multiple worker threads need to read from the same
materialized temporary table, but MySQL does not allow concurrent access
to the same temporary table.

**Solution:**
- Workers share only the **data storage** of the materialized table
- Each worker has its own **query structure** (search cursors, etc.)

##### Temptable Engine

- Data: `m_opened_table.m_rows`, `m_opened_table.m_index_entries`
- Cursors: `m_rnd_iterator` (table scan), `m_index_cursor` (index scan)
- Per worker: create own temp table, replace `m_opened_table` with leader's,
  restore after query

##### InnoDB Temporary Table

- More complex due to `m_prebuilt` structure
- Data: stored in primary index, secondary indexes for fast lookup
- Search: `row_search_no_mvcc` only changes cursor, not index data
- Per worker: create own temp table, replace `m_index` with leader's,
  close shared `rec_cache`, store private `pcursor`, restore after query

##### PQ_shared_info Structure

```c++
struct PQ_shared_info {
  PQ_tmp_info m_type{TEMP_MEM};  // table type
  void *m_share{nullptr};        // shared storage data
  void *m_table{nullptr};        // table pointer
};

init_shared_info()   // write shared info
set_shared_info()    // replace worker storage with shared
reset_shared_info()  // restore worker's original storage
```

### Semi-Join Materialization

For queries with semi-join using FirstMatch or LooseScan strategy,
parallel execution is supported by cloning the materialization state
to each worker thread.

### Limitations

- Dependent (correlated) subqueries are not parallelized in the current
  implementation
- Early materialization of inner subqueries may be wasteful if the outer
  query produces no results
- UNION DISTINCT parallel requires materialization overhead

## References

- `make_pq_unit_plan` - UNION parallel plan generation
- `para_exec_init` - inner subquery materialization trigger
- `PQ_shared_info` - temporary table sharing structure
- `MaterializeIterator` - MySQL's materialization iterator
- `pq_clone_sj_mat_exec` - semi-join materialization clone
