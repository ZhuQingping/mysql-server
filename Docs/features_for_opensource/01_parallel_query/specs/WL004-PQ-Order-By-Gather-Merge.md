# WL#004: PQ Order By & Gather Merge

## Summary

Implement parallel execution for ORDER BY queries. Each worker sorts its
local data partition, and the leader performs a merge sort across all workers'
partially ordered results using a binary heap (gather merge).

## Motivation

ORDER BY is one of the most common query clauses. Without parallel sort, the
entire sorting workload falls on a single thread. Pushing sort down to workers
and merging results enables near-linear speedup for large sorted result sets.

## Specification

### ORDER BY on Worker Threads

The worker sorting process mirrors the original MySQL sorting:

1. **Copy ORDER structure:** Copy `select_lex->order_list`, resolve it,
   generate optimized `join->order` using saved `optimized_order_flags`
2. **Sort execution:** `SortingIterator::Init()` reads all data into the sort
   buffer, constructs sort keys, and sorts
3. **Send results:** Sorted results are sent to the message queue

#### optimized_order_flags

A vector of bool where `optimized_order_flags[i] = 1` means `order_list[i]`
was NOT optimized away and still exists in `join->order`. This avoids
re-invoking the optimizer on the worker thread.

### Worker-Side Optimization

**Remove redundant sorting on temporary tables:** For queries like
`GROUP BY t1.a ORDER BY t1.b`, sorting is performed after grouping. Since
the leader also performs this operation, the worker's sort is redundant and
is removed.

### Merge Sort on Leader (Gather Merge)

The leader uses a **binary heap** to merge partially sorted results from
all workers.

```
Binary heap:
        w[1]
       /    \
     w[2]   w[3]
    /   \
  w[4]  w[5]

w[i] = i-th worker's sorted result queue
```

**Algorithm:**

```
i = bh.top()             // get top element
if w[i] has unread tuple:
  bh.top() = i
  m_slots[i] = read tuple from w[i]
  sift_down(bh, 0)      // re-adjust heap
else:
  bh.pop()               // remove exhausted worker
  if bh not empty:
    i = bh.top()
    return m_slots[i]
```

### Constructing Sort Fields on Leader

In some scenarios, the leader must reconstruct sort fields:

1. **GROUP BY with index scan:** Results must be aggregated by group field,
   requiring sort on group field
2. **ORDER BY with index scan:** Sorting is via index scan on workers; leader
   must merge by sort field
3. **Implicit index scan (covering index):** Results are ordered by index key;
   sort fields must be constructed based on covering index keys to ensure
   consistency with non-parallel execution

Implementation: `pq_make_filesort()`

### Gather Merge with Aggregation

When both GROUP BY and ORDER BY are present:
1. Workers perform partial GROUP BY (no sort needed after group)
2. Leader performs secondary aggregation
3. Leader sorts the final aggregated results

If group-by uses sorting (not hash), workers sort by group key and the leader
can use the gather merge to produce globally sorted group results without
an additional sort phase.

## References

- `pq_make_filesort` - construct sort fields for leader
- `Gather_merge` - binary heap merge sort implementation
- `optimized_order_flags` - preserved optimization state for workers
- `SortingIterator` - MySQL's sort iterator (used on workers)
