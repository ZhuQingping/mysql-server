# WL#016: PQ Secondary Index Scan & ICP Support

## Summary

Enable parallel query on secondary index scans, including support for
Index Condition Pushdown (ICP). Handle the visibility checking and
compare function differences between primary and secondary indexes in
the parallel scanning context.

## Motivation

Secondary index scans are common in query execution plans. Without
secondary index PQ support, queries that use secondary indexes fall back
to single-threaded execution. ICP is an important optimization for
secondary index scans that must also work correctly in parallel mode.

## Specification

### Secondary Index Scan in Parallel

Unlike primary key scans, secondary index records do not contain
`DATA_TRX_ID` and `DATA_ROLL_PTR`, so visibility cannot be directly
determined from the secondary index record alone.

**Visibility checking for secondary index:**

1. If all modifications on the page are visible (checked via page LSN),
   no table lookup is needed
2. Otherwise, look up the primary key record and check visibility via
   the primary index's MVCC mechanism

**Secondary index record read process:**

```c++
// 1. Read secondary index record
// 2. Check if page LSN indicates all changes are visible
//    - If visible: return record directly
//    - If not visible: look up primary key record
// 3. For primary key lookup: check visibility via MVCC
//    - If visible: return record
//    - If not visible: get older version via row_vers_build_for_consistent_read
```

### Secondary Index Compare Function

When scanning secondary index data blocks in parallel, the boundary
compare function differs from primary key because secondary index records
have a different format. The compare function must correctly handle:

- Secondary key columns
- Primary key columns appended to secondary index entries
- NULL handling in secondary index columns

### Index Condition Pushdown (ICP)

ICP allows pushing WHERE conditions down to the storage engine level,
filtering records during index scan before returning to the SQL layer.

**ICP in parallel scan:**
- Each worker evaluates ICP conditions independently on its assigned
  data block
- ICP filtering happens before the record is returned to the SQL layer,
  reducing the number of records transferred through the message queue
- The ICP state must be properly initialized for each worker's scan context

### Secondary Index Range Scan

For parallel range scan on a secondary index:

1. The leader determines the scan boundaries from `QUICK_RANGE_SELECT`
   or `QUICK_SELECT_DESC`
2. Boundaries are constructed as dtuple_t using secondary index format
3. The first-level partition uses the secondary index B+tree root page
4. Workers scan their assigned blocks using the secondary index

### Context Boundary Judgment

For secondary index Ctx boundary judgment:
- Use `end_tuple->compare(rec, index, offsets)` with the secondary index
- Both the record and end_tuple must belong to the same secondary index
- Properly handle the case where secondary index entries include primary
  key columns

## References

- `Design/HLD/二级索引查询.md` - secondary index query design
- `Design/HLD/二级索引ICP.md` - ICP support design
- `Design/HLD/二级索引查询回表后的compare函数.md` - compare function after lookup
- `Scan_ctx::find_visible_record` - visibility checking
