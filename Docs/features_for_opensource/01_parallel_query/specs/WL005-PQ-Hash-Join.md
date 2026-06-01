# WL#005: PQ Hash Join

## Summary

Implement parallel execution for hash join queries. Support both in-memory
and on-disk (partition) hash join scenarios with different parallelism
strategies depending on whether the build table or probe table is partitioned.

## Motivation

Hash join is the primary join method for large tables in MySQL 8.0. Parallel
hash join must handle both cases where data fits in memory and cases requiring
external chunk files, while minimizing file handle usage, disk space, and I/O
contention.

## Specification

### Non-Partition Scenario (In-Memory Hash Join)

**Case 1: Divide the build table**

Each worker reads a portion of the build table (t1) and performs hash join
with the full probe table (t2). Each worker creates its own hash map.

Advantages:
- No hash map contention between workers (higher concurrency)
- No data duplication across hash maps (modest memory increase vs single-thread)

```
Worker1: t1.part1 -> build hash map -> probe with full t2
Worker2: t1.part2 -> build hash map -> probe with full t2
```

**Case 2: Divide the probe table**

Each worker reads the full build table (t1) and probes with its portion of
the probe table (t2). All workers share a single hash map.

- One worker creates the hash map preemptively; others wait
- After creation, all workers read the hash map concurrently

```
Worker1: full t1 (shared hash map) -> probe with t2.part1
Worker2: full t1 (shared hash map) -> probe with t2.part2
```

### Partition Scenario (On-Disk Hash Join)

When the build table doesn't fit in memory, both tables must write chunk
files. Build table chunks and probe table chunks have a one-to-one
correspondence.

**Case 3: Divide build table (recommended for large build table)**

- Build table: workers process different parts, create separate chunk files
  and hash maps
- Probe table: workers process different parts in parallel but share a single
  chunk file group per chunk pair

Impact:
- Chunk file disk space: same as single-threaded
- Memory: total hash map memory increases (needs global limit)
- File handles: increased (use file pool to control)

**Case 4: Divide probe table (build table needs partition)**

Rare case: probe table is divided but build table also needs partitioning.
- Workers share the same chunk file group of the build table (created by one
  worker preemptively)
- Workers use different hash map structures (different execution speeds mean
  different chunk processing)

### Multi-Table Hash Join

For multi-table joins like `(t1 x t2 x t3)`, the execution plan is a left-deep
tree. The position of the divided table determines the parallel strategy:

1. **Divided table in left sub-hash-join (t1 x t2):** Similar to Case 1/3
2. **Divided table is right probe table (t3):** Similar to Case 2/4
3. **No divided table in current hash join tree:** Normal hash join execution

```
       HashJoin(t1 x t2 x t3)
       /                    \
  HashJoin(t1 x t2)         t3
  /           \
t1             t2

If t1 is divided: Case 1/3 applies to HashJoin(t1 x t2)
If t3 is divided: Case 2/4 applies to the top HashJoin
If t2 is divided: similar to t1 being divided
```

### Hash Join Chunk Management

For on-disk hash join, chunk files require careful management:
- File handle pool to limit open file descriptors
- Shared chunk file groups to avoid disk space multiplication
- Coordinated chunk processing between workers

## References

- `Parallel-hash-join-Design-(HLD).md` - hash join design document
- `Parallel-hash-join-LLD.md` - low-level design
- `HashJoinChunk` - chunk file management
