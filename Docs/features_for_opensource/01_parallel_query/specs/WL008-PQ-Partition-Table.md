# WL#008: PQ Partition Table Support

## Summary

Extend parallel query to support MySQL partition tables. Each partition is
treated as a separate data source, and data blocks from all used partitions
are distributed to workers. Both unordered and ordered (index-aligned) parallel
reading are supported.

## Motivation

Partition tables are widely used for managing large datasets. Without parallel
partition support, queries on partitioned tables cannot benefit from the PQ
framework, creating a significant gap in analytical query coverage.

## Specification

### MySQL Partition Table Management

**SQL Layer:**
- `TABLE::part_info` (partition_info object)
- `MY_BITMAP read_partitions` - partitions used for reading
- `MY_BITMAP lock_partitions` - partitions requiring locks

**InnoDB Layer:**
- `ha_innopart` class (subclass of `ha_innodb` / `Partition_helper`)
- Per-partition cursors: `m_pcur_parts`, `m_clust_pcur_parts`
- Partition switching: `ha_innopart::set_partition(part_id)`

### Data Partition Phase

The leader performs first-round data division on all used partitions:

```c++
ha_innopart::pq_leader_scan_init
  -> for each used partition:
     -> split_used_partition()
```

For dependent-ref join, range building is extended per partition:

```c++
PQRefIterator::Read
  -> ha_innobase::pq_depref_build_ranges
     -> for each partition:
        -> ha_innobase::pq_ref_build_ranges(part_id, ...)
```

### Data Block Management

**Two-level map structure** for managing data blocks across partitions:

```
Level 1: join ref key -> partition map
Level 2: partition id -> data block collection
```

For non-partitioned tables, all data blocks are placed in the bucket with
`part_id=0`.

```c++
Slices_part_map = PQ_part_map<Slices_mngr *, Slice_cell>
```

**Worker local management:** Workers also use a map to manage local data
blocks by partition:

```c++
PQ_ctx_map = PQ_part_map<PQ_dual_queue<PQ_Ctx_Base> *, Ctx_cell>;
```

### Data Reading Phase

#### Index Unordered Parallel Reading

Workers sequentially read data blocks from each partition bucket. When one
partition is exhausted, the worker switches to the next:

```c++
ha_innobase::pq_worker_scan_init:
  part_id = m_part_info->get_first_used_partition()
  set_partition(part_id)
  m_last_part = part_id

ha_innopart::pq_worker_scan_next:
  part_id = m_last_part
  dispatch_ctx(part_id, ...)
  m_part_info->get_next_used_partition(part_id)
```

#### Index Ordered Parallel Reading

When the table access requires ordering (determined by `QEP_TAB::use_order()`),
workers use a priority queue to merge-ordered records from multiple partitions.

**Sorting infrastructure** (reuses `Partition_helper` members):

```c++
Prio_queue *m_queue;           // priority queue for sorted read
uint m_top_entry;              // partition delivering next result
uchar *m_ordered_rec_buffer;   // row and key buffer for ordered scan
```

**Sorting process:**

```c++
ha_innopart::pq_worker_scan_next:
  -> pq_handle_ordered_next:
     1. If ordered queue is empty:
        - Read records from each partition, fill into Prio queue
        - Record partition id of top record to m_top_entry
     2. If ordered queue has records:
        - Read record from queue top
        - Read a record from m_top_entry partition, fill into queue
        - Update Prio queue with m_queue.update_top()
```

### Multi-Partition Cursor Management

Since ordered reading requires multiple partitions to be open simultaneously,
the `m_prebuilt` structure is modified:

```c++
// Before: single partition
m_prebuilt->is_attach_ctx    // single boolean
m_prebuilt->pq_ctx           // single context

// After: multiple partitions
m_prebuilt->is_attach_ctx[]  // array indexed by partition
m_prebuilt->pq_ctx[]         // array indexed by partition
```

### Performance

Benchmarks show significant speedup for partition table queries with parallel
execution (see design document for detailed TPC-H results).

## References

- `ha_innopart::pq_leader_scan_init` - partition data partitioning
- `ha_innopart::pq_worker_scan_init/next` - partition parallel scanning
- `PQ_part_map` - partition-aware data block map
- `Partition_helper::init_record_priority_queue` - ordered scan setup
