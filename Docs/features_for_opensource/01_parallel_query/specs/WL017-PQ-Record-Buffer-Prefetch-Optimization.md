# WL#017: PQ Record Buffer, Prefetch & Virtual FD

## Summary

Optimize parallel query I/O performance through record buffer optimization,
prefetch/read-ahead support, and virtual file descriptor management. These
optimizations reduce per-row overhead and improve disk I/O efficiency in
parallel scanning.

## Motivation

In parallel query, each row read incurs overhead from InnoDB cursor
management and record format conversion. For large table scans, this
overhead becomes significant. Additionally, parallel scanning of multiple
data blocks creates contention on file descriptors and I/O bandwidth.

## Specification

### Record Buffer Optimization

MySQL's `record_buffer` (also called `read_ahead` buffer) allows reading
multiple rows in a single I/O operation. In parallel query:

- Each worker uses its own `record_buffer` to batch-read records from
  its assigned data block
- The buffer size is configurable via `parallel_read_buffer_size`
- When the buffer is exhausted, the worker refills it from the next
  page in the data block

**Implementation:** The `PQblockScanIterator::Read()` function uses
`ha_pq_next()` which internally reads records through the record buffer,
reducing the number of B+tree cursor restore operations.

### Prefetch / Read-Ahead

InnoDB's read-ahead mechanism detects sequential page access patterns
and prefetches pages into the buffer pool before they are needed.

**Prefetch in parallel scan:**
- Each worker's sequential page access triggers InnoDB's linear read-ahead
- The leader's initial partition provides ordered data blocks, naturally
  producing sequential access patterns
- For index range scan, pages within each Ctx are accessed sequentially,
  triggering read-ahead
- Read-ahead is disabled for random access patterns (e.g., dependent-ref
  join with scattered key lookups)

**Configuration:**
- `innodb_read_ahead_threshold` controls when linear read-ahead is triggered
- The default threshold works well for parallel sequential scans

### Virtual File Descriptor (VFD)

Parallel hash join's on-disk variant (partition scenario) opens many chunk
files simultaneously. Each worker may need to open files for multiple chunk
pairs, potentially exhausting the process file descriptor limit.

**VFD pool design:**
- A global file descriptor pool manages open file handles
- Workers acquire and release file descriptors from the pool
- When the pool is exhausted, least-recently-used (LRU) file descriptors
  are closed and recycled
- The pool size is bounded by a configurable limit

**Implementation:**
```c++
class PQ_file_pool {
  acquire_fd(path)     // get an open FD from pool or open new
  release_fd(path)     // return FD to pool (stays open for reuse)
  evict_lru()          // close least recently used FD when pool is full
};
```

### Quick-Select Deep Copy Optimization

When copying the `QUICK_RANGE_SELECT` object for workers, a deep copy
of the MRR (Multi-Range Read) information is required. This includes:

- Copying `QUICK_RANGE` objects with their key boundaries
- Copying the handler's MRR iterator state
- Ensuring each worker has its own independent MRR cursor

The deep copy is performed in `QUICK_RANGE_SELECT::reset()` for each
worker, initializing fresh MRR state.

## References

- `Design/HLD/并行查询使用Record_buffer.md` - record buffer design
- `Design/HLD/预读功能设计文档.md` - prefetch design
- `Design/HLD/虚拟文件句柄.md` - virtual file descriptor design
- `Design/HLD/Quick-Select-深拷贝设计.md` - quick select deep copy
- `Design/HLD/worker记录扫描函数.md` - worker scan function details
