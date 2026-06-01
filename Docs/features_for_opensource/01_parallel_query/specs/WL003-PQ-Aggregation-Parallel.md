# WL#003: PQ Aggregation Parallel Execution

## Summary

Implement parallel execution for aggregate queries (COUNT, SUM, AVG, GROUP BY,
HAVING). Each worker computes partial aggregation on its data partition, and
the leader performs secondary aggregation to produce final results.

## Motivation

Aggregate queries are common in analytical workloads. Without parallel
aggregation, these queries are limited to single-thread execution. Parallel
aggregation enables near-linear speedup for SUM/COUNT and efficient handling
of AVG, complex expressions, and HAVING clauses.

## Specification

### Aggregation Parallel Principle

```
Worker1: Primary_tables -> TMP table1 -> TMP table2 -> MQ
Worker2: Primary_tables -> TMP table1 -> TMP table2 -> MQ
  ...
Leader:  MQ -> TMP table1 -> TMP table2 -> client
```

The leader's first temporary table replaces all primary tables. Subsequent
temporary tables are generated from this table. Workers copy the complete
execution plan and recreate their temporary tables. Workers send data to the
leader's first temporary table through the message queue.

### Secondary Aggregation

Since each worker processes only a portion of the data, worker aggregation
results are partial. The leader must perform secondary aggregation on the
combined partial results.

#### COUNT() and SUM()

Workers compute partial SUM/COUNT on their data partitions. The leader
re-aggregates by summing all workers' partial results.

```
Worker1: Sum(part1)  Worker2: Sum(part2)  ...  WorkerN: Sum(partN)
                        \        |        /
                    Leader: Sum(Worker_results)
```

**Leader rebuild sum_funcs:** When the leader creates its temporary table,
`Item_func` is replaced with `result_field`. The `rebuild_sum_functions`
restores the aggregation function pointers from `result_field` so that
secondary aggregation works correctly.

#### AVG()

AVG = SUM / COUNT, requiring both values to be passed from workers to the
leader for correct secondary aggregation.

**Extended field format:**

```
Extended_avg:
  [sum.pack_length bytes][8 bytes for count]
```

The result_field of AVG on the worker/leader tmp table uses extended precision:
- Significant digits are expanded to match SUM's precision
- 8 additional bytes appended to store COUNT

**Restore Item_sum_avg:** If a temporary table follows AVG (e.g., `SELECT
AVG() FROM t`), `Item_field` replaces `Item_sum_avg` during tmp table
creation. Before creating the leader's temporary table, `Item_sum_avg` must
be restored to generate the correct `result_field`.

### Operations with Aggregation

For expressions like `SELECT SUM(a) - SUM(b) FROM t`, the worker must compute
both `SUM(a)` and `SUM(b)` separately rather than the subtraction, so the
leader can perform secondary aggregation correctly.

**aggregation_ref mechanism:**

```
all_fields:
  [0] Item_sum_sum(a)   [1] Item_sum_sum(b)   [2] Item_sum_minus
                                                    /          \
                                           aggr_ref(a)    aggr_ref(b)
```

- `Item_aggr_ref` uses a pointer to the all_fields position rather than
  directly to the Item, so that when the temporary table switches slices,
  positions [0] and [1] are replaced with result_fields, and the minus
  operation still references the correct Items.

### Temporary Table Data Exchange

- Leader and worker temporary tables are generated from the same `all_fields`
- Table structures are identical (including hidden fields)
- Workers copy aggregation results (hidden_fields) to leader's tmp table

### Skip Operations Between Aggregations on Workers

Since aggregation is computed first (typically on the GROUP BY temporary
table), inter-aggregation operations (e.g., `SUM(a) - SUM(b)`) are computed
after all data is scanned. On workers with incomplete data, these operations
are meaningless.

**Solution:** When building the worker's `fields_list`, check if Items contain
aggregation or `group_func`. If so, exclude them from the worker's
`fields_list`, preventing workers from computing inter-aggregation expressions.

### HAVING with Aggregation

Workers cannot evaluate HAVING clauses containing aggregation (their data is
incomplete and would incorrectly filter rows).

**Three-part solution:**
1. **Worker:** Set HAVING to NULL if it contains aggregation or group_func
2. **Leader:** Rebuild HAVING judgment using the saved HAVING condition from
   the optimization phase
3. **all_fields:** Add HAVING aggregate operations to `all_fields` via
   `split_sum_func`, similar to the standard optimization process

### Group Reshuffle (Advanced)

For GROUP BY queries with many groups, the previous design (worker partial
aggregation + leader secondary aggregation) creates a leader bottleneck.

**Group reshuffle design:**
- Workers are split into **frontend** (read data, reshuffle by group key hash)
  and **backend** (receive reshuffled data, perform complete aggregation)
- DOP=N requires 2N total worker threads (N frontend + N backend)
- Frontend and backend communicate via FIFO queues
- Backend workers perform complete aggregation, eliminating leader bottleneck
- Enables support for `aggr(DISTINCT)`, non-only_full_group_by, and HAVING
  pushdown to workers

**Memory control:** Uses dual memroot strategy - when one memroot reaches
limit, switch to the other; clear the first when switching back.

**Limitations:**
- Overhead may exceed benefit when number of groups is small
- Leader still needs secondary sort for ORDER BY

## References

- `JOIN::make_leader_tmp_table` - leader tmp table creation
- `rebuild_sum_functions` - secondary aggregation setup
- `Item_aggr_ref` - reference mechanism for inter-aggregation expressions
- `PQ_group_aware` - group reshuffle base class
- `TemptableAggregateIterator`, `AggregateIterator` - reshuffle operators
