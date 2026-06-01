# WL#013: PQ Semi/Anti/Outer Hash Join

## Summary

Extend parallel hash join to support semi-join, anti-join, and outer (left)
join. These join types have different correctness requirements than inner
join, particularly regarding which table can be divided.

## Motivation

The first version of parallel hash join only supported inner join. Semi-join
(`IN` subquery), anti-join (`NOT IN` subquery), and outer join (`LEFT JOIN`)
are common in analytical queries and must also be parallelized.

## Specification

### Semi Hash Join

```sql
SELECT ... FROM t1 WHERE ... IN (SELECT ... FROM t2)
```

- t2 (IN-part) is the build input; t1 is the probe input
- For each probe row, find one match in the hash table and output the
  probe row
- **Divided table must come from probe tables only.** If the build table
  is divided (reading partial data), some matching rows will be lost.

### Anti Hash Join

```sql
SELECT ... FROM t1 WHERE ... NOT IN (SELECT ... FROM t2)
```

- t2 (NOT IN-part) is the build input; t1 is the probe input
- For each probe row, if no match is found in the hash table, output the
  probe row
- **Divided table must come from probe tables only.** If the build table
  is divided, duplicate rows will be produced.

### Outer Hash Join

```sql
SELECT ... FROM t1 LEFT JOIN t2 ON ...
```

- t2 (right input) is the build input; t1 is the probe input
- For matching rows, output the joined row; for non-matching, output a
  NULL-complemented row
- **Divided table must come from probe tables only.** If the build table
  is divided, duplicate NULL-complemented rows will be produced.

### Parallel Strategy

For all three join types, the divided table can only come from probe tables.
Two scenarios:

**Build input is small (fits in memory):**
Each worker creates its own hash table from the full build input. Workers
scan different parts of the probe table independently.

**Build input is large:**
Parallel-aware hash join: workers cooperatively build a **shared hash
table**. Each worker reads a subset of the build table and inserts into
the shared hash table. Since all workers can access all build rows,
semi/anti/outer correctness is preserved.

### Cut Table Selection for Semi/Anti/Outer Hash Join

The qep_tab order may differ from the hash join tree order. For example:

```
EXPLAIN: t1 -> t2 (Left hash join) -> t3 (Left hash join)
Hash join tree:
  LeftHashJoin(t3=t2)
    LeftHashJoin(t2=t1)
      t1 (probe)
      Hash: t2 (build)
    Hash: t3 (build)
```

The divided table must be t1 (probe of the first hash join). But the cut
table (the table whose data blocks are distributed to workers) could be t2
or t3 if they are before the divided table in the hash join tree.

**Rule:** After choosing the divided table, `MarkCutTable()` finds the
largest table (by rows or prefix rows) that is before the divided table
in qep_tab order. For semi/anti/outer hash join, the algorithm also
traverses the build table of the hash join to find eligible cut tables.

The cut table cannot be the inner table of a non-inner join.

## References

- `Parallel-semi/anti/Parallel-semi-anti-outer-hash-join.md`
- `MarkCutTable` - cut table selection algorithm
- `Parallel-hash-join-LLD.md` - low-level design
