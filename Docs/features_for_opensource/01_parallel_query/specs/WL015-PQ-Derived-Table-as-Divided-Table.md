# WL#015: PQ Derived Table as Divided Table

## Summary

Allow materialized derived tables (including views and CTEs) to be selected
as the divided table in parallel query. This includes supporting both
Temptable and InnoDB intrinsic temporary table storage engines for
parallel data scanning.

## Motivation

Some TPC-H queries use materialized derived tables as the only or largest
table in a query block. Previously, derived tables could not be chosen as
the divided table, making parallel execution impossible for such queries.

## Specification

### Problem

When a query has only one table and it is a derived table, PQ cannot
parallelize it because derived tables were not eligible as divided tables.
This is a significant limitation for queries like:

```sql
SELECT * FROM (SELECT SUM(a) FROM t2 GROUP BY a) AS sub
```

### Temptable Engine Support

For derived tables materialized in the Temptable engine:

- **Table scan:** The linked list of all data pages is scanned and divided
  into intervals, which are distributed to workers
- **Index scan:** NOT supported for division (Temptable indexes use
  `std::unordered_map` / `std::multimap`, which do not support efficient
  division)

### InnoDB Intrinsic Table Support

For derived tables materialized in InnoDB intrinsic tables:

- A specialized `read_record()` function takes advantage of intrinsic table
  properties (read-only, no transaction concerns)
- The `rec_cache_t` feature, previously disabled in PQ, is re-enabled for
  performance with thread-safety modifications

### Refactoring

Common code between Temptable and InnoDB implementations is extracted into
base classes in `sql/pq_handler.*`:

- `pq_handler.h/cc` - base class with shared data structures and functions
- InnoDB extends these with engine-specific members (dtuple_t, trx_t, etc.)

### SQL Layer Changes

- Existing PQ code needed fixes to handle derived tables at points where
  only physical tables were expected
- `choose_parallel_tables()` now considers derived tables as candidates
  for the divided table

### Limitations

- Temptable with index scan cannot be divided
- InnoDB intrinsic table scanning requires re-enabling rec_cache_t with
  thread-safety considerations

## References

- `Other/Design-for-dividing-derived-table-in-PQ.md`
- `sql/pq_handler.*` - shared base classes
- `rec_cache_t` - record cache for InnoDB intrinsic tables
