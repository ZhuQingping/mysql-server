# WL#002: PQ Data Partition & InnoDB Parallel Scan

## Summary

Implement data partitioning for parallel query in the InnoDB storage engine.
The leader thread splits table data into blocks based on B+tree structure,
and workers scan assigned blocks. Support includes full table scan, range
scan, and ref scan with two-level partitioning and dynamic second-level split.

## Motivation

Data partitioning is the foundation of parallel query. Without efficient
partitioning, workers would scan overlapping or unbalanced data ranges,
leading to incorrect results or poor parallelism. InnoDB's B+tree index
structure provides natural partition boundaries at the root page level.

## Specification

### Context Queue Architecture

```
Parallel_leader -> PQ_ctxs_manager -> PQ_ctxs_queue (shared queue)
                                         ^
                                         | dequeue
Parallel_worker -> PQ_ctxs_dual_queue (private: current + standby)
```

- `PQ_ctxs_manager`: manages the global context queue
- `PQ_ctxs_queue`: thread-safe queue of scan contexts
- `PQ_ctxs_dual_queue`: per-worker queue ensuring same data across scan rounds

### Index Table Partition (First-Level)

The leader determines partition boundaries based on the scan type:

#### Full Table Scan
- Forward: `range_start` and `range_end` are empty dtuple_t
- Reverse: `range_end` positioned at `index_last`

#### Range Scan
Supports `QUICK_RANGE_SELECT` and `QUICK_SELECT_DESC`. Each `KEY_MULTI_RANGE`
corresponds to one range, and the leader partitions each range independently.

```c++
while (!(range_res = mrr_funcs.next(mrr_iter, &mrr_cur_range))) {
  // construct range_start and range_end dtuple_t
  pq_reader->add_scan();
}
```

#### Ref Scan
- **Independent ref**: Treated as a single-range scan; boundaries from
  `HA_READ_KEY_OR_NEXT` and `HA_READ_AFTER_KEY`
- **Dependent ref**: Partition deferred to workers since ref key depends on
  outer table values

#### First-Level Partition Algorithm

`Scan_ctx::create_ranges` traverses the B+tree root node records. Each root
record corresponds to a leaf page range, forming one Ctx (scan task). The
number of Ctxs equals the number of root page records within the query range.

### Second-Level Split

The number of first-level Ctxs is determined by root page records, which may
not evenly divide among workers. The second-level split dynamically subdivides
remaining Ctxs to improve load balancing.

**Split point determination:**
1. If `#Ctxs > #threads`: split only the tail Ctxs
2. If B+tree height < 3: no second split (smallest unit is a page)
3. Otherwise: all Ctxs are marked for second split

**Dynamic split by workers:**
- Workers perform the actual split when encountering a marked Ctx
- Split uses `Scan_ctx::partition` with depth=1
- Child Ctxs are enqueued in order to preserve scan sequence
- Serialization ensured by tracking `m_split_cur`

```
Ctx::split() {
  auto ranges = m_scan_ctx->partition(scan_range, 1);
  // Wait for this ctx's turn to enqueue (preserve order)
  while (m_id != m_split_cur) { os_thread_sleep(20); }
  m_scan_ctx->m_reader->ctx.enqueue(ctxs, false);
  m_split_cur++;  // or -- for reverse scan
}
```

### Worker Data Collection

Each worker must scan the same data blocks across multiple rounds (e.g., in
nested-loop joins where the inner table is scanned repeatedly).

```
Worker_get_next_ctx:
  if public_queue not empty:
    get ctx from public queue -> put in standby queue
  else if current_ctx queue not empty:
    get ctx from current queue -> put in standby queue
  else:
    rotate: standby becomes current, get next ctx
```

This guarantees:
1. All contexts are consumed even if a worker fails to start
2. A worker scans the same data in each round (private queue)
3. Work distribution is balanced (first-round assignment persists)

### InnoDB Parallel Scan Interface

```c++
ha_innobase::pq_leader_scan_init  // Leader: determine boundaries + partition
ha_innobase::pq_worker_scan_init  // Worker: copy read view, start scan
ha_innobase::pq_worker_scan_next  // Worker: read next record from block
```

**Read view:** All workers share the leader's read view for consistent
visibility checking.

**Visibility check:**
- Read-only mode: view is empty
- Non-read-only: check via MVCC (primary key) or page LSN (secondary key),
  falling back to primary key lookup if needed

### Reverse Index Scan

For covering index queries with `ORDER BY DESC` or descending indexes:

- Ctxs enqueue in reverse order
- Workers scan each Ctx in reverse
- Boundary tuples are repositioned for reverse direction
- Second-split child Ctxs maintain reverse order via `m_split_cur--`

### Data Partition for Multi-Table Join

In a join, only one table (the first non-const primary table) is partitioned.
Other tables are fully scanned by each worker. The `choose_parallel_tables()`
function selects the best candidate based on cost and size heuristics.

## References

- `row0pread.cc` - InnoDB parallel reader (community reference)
- `PQ_ctxs_manager`, `PQ_ctxs_queue`, `PQ_ctxs_dual_queue` - context management
- `Scan_ctx::create_ranges` - first-level partition
- `Ctx::split` - second-level dynamic split
