# WL#001: Parallel Query Core Architecture

## Summary

Implement a parallel query execution framework for MySQL that enables a single
SELECT query to utilize multiple CPU cores. The framework splits the first
non-const primary table into data blocks, distributes them to worker threads,
and merges results through a leader thread using message queues.

## Motivation

MySQL executes each query using a single thread, leaving multi-core resources
underutilized. Large analytical queries (e.g., TPC-H) benefit significantly
from parallel execution. This work provides the foundational architecture for
all parallel query sub-features.

## Specification

### High-Level Design

The execution plan of MySQL is a left-deep tree. Parallel query splits the
first non-const primary table into N data blocks. Each worker thread gets an
identical copy of the execution plan but scans only its assigned block. The
leader thread merges worker results and performs final aggregation, sorting,
or sends results directly to the client.

```
Before parallel:
  Aggregation -> Sort -> T1 JOIN T2 JOIN T3

After parallel:
  Gather (merge)
    |            |            |
  Worker1      Worker2     WorkerN
  T1.part1     T1.part2    T1.partN
  JOIN T2,T3   JOIN T2,T3  JOIN T2,T3
```

### Execution Plan Rewrite

The core transformation is `make_pq_leader_plan(JOIN *join, THD *thd)`, which
rewrites the single-threaded serial execution plan into a parallel execution
plan after `optimize()` and before `execute()`.

#### Leader Plan

1. Copy the entire serial execution plan as a **template JOIN** (including THD,
   SELECT_LEX, SELECT_LEX_UNIT, JOIN, QEP_TAB structures).
2. Rewrite the leader's plan: replace the parallel-scanned table with a
   temporary table that receives worker results via `ParallelScanIterator`.
3. The leader's `qep_tab1` array consists of:
   - A message-queue temporary table (receives worker results)
   - Additional temporary tables for group-by/aggregation

Key JOIN member variables for plan switching:

```c++
class JOIN {
  QEP_TAB *qep_tab0;   // single-thread plan
  QEP_TAB *qep_tab1;   // parallel plan
  Ref_item_array *ref_items0, *ref_items1;
  List<Item> *tmp_all_fields0, *tmp_all_fields1;
  List<Item> *tmp_fields_list0, *tmp_fields_list1;
};
```

#### Worker Plan

Each worker creates its own JOIN from the template JOIN:

```
1. pq_make_join(new_thd, template_join)     // copy JOIN
2. join->setup_tmp_table_info(template_join) // setup tmp table params
3. join->make_tmp_tables_info()              // create tmp tables
4. Create Query_result_mq for sending results
```

### New Iterators

**ParallelScanIterator** (leader side):
- `Init()`: create message queue, split data, launch workers
- `Read()`: fetch worker results from message queue, fill into `record[0]`

**PQblockScanIterator** (worker side):
- `Init()`: copy leader's read view, start parallel scan on assigned block
- `Read()`: read records from the assigned data block

### Temporary Tables for Data Exchange

Leader and worker each have a temporary table at the end/beginning of their
QEP_TAB arrays. These tables have identical structure. Worker fills its
tmp_table `record[0]` and sends it via message queue; leader receives and
copies into its own tmp_table `record[0]`.

The temporary table solves two problems:
1. Multi-table join results are scattered across multiple `record[0]` buffers;
   the tmp table consolidates them into one.
2. Computed fields (e.g., `a + b`) are evaluated when writing to the tmp table,
   so only final values are transferred.

### PQ Eligibility Rules

A query is eligible for parallel execution only if it passes all checks in
`THD::suite_for_parallel_query()`, `TABLE::suite_for_parallel_query()`, and
`JOIN::suite_for_parallel_query()`:

- Must be a SELECT statement
- Must not be in a stored procedure, trigger, or attachable transaction
- Must not use SERIALIZABLE isolation level
- Must use InnoDB storage engine
- Must not be a temporary table or use fulltext search
- Must not exceed `parallel_memory_limit` or max PQ thread limits
- Must pass RBO rules in `choose_parallel_tables()`

RBO checks include:
- Query cost must exceed `parallel_cost_threshold`
- Must have at least one non-const primary table
- Must not have `zero_result_cause`
- Must not use `SELECT DISTINCT` or `COUNT(*)` (handled separately)
- Must not exceed `MAX_FIELDS`
- Must not use ROLLUP
- Must not use semi-join with Materialization or DuplicateWeedout strategy

### Interface Changes

#### New System Variables

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `parallel_default_dop` | int | 4 | Default degree of parallelism |
| `parallel_cost_threshold` | int | 10000 | Minimum query cost to trigger PQ |
| `parallel_memory_limit` | int | 256MB | Max memory for all PQ executions |
| `parallel_queue_timeout` | int | 0 | Timeout for PQ thread queue |

#### New Status Variables

| Variable | Description |
|----------|-------------|
| `parallel_threads_refused` | Count of PQ refusals due to thread limit |
| `parallel_memory_refused` | Count of PQ refusals due to memory limit |

#### New EXPLAIN Output

A `<gatherN>` row is added to traditional EXPLAIN output showing the number
of workers and the parallel-scanned table:

```
| 1 | SIMPLE | <gather1> | NULL | ALL | ... | Parallel execute (4 workers, db.t1) |
| 1 | SIMPLE | t1        | NULL | range | ... | Using where; Using index |
```

### Limitations

- Parallel query operates within a single query block; UNION queries are
  parallelized per query block
- Subqueries using DuplicateWeedout or Materialization semi-join strategies
  are not parallelized
- Partition table parallel support is a separate sub-feature
- The old executor (pre-8.0.20) was the initial target; migrated to iterator
  model with minimal changes

## References

- `sql/parallel_query/` - main PQ source directory
- `ParallelScanIterator` - leader-side iterator
- `PQblockScanIterator` - worker-side iterator
- `Gather_operator` - manages workers and data partitioning
- `Query_result_mq` - worker result sender via message queue
