# WL#009: PQ EXPLAIN Support

## Summary

Extend MySQL's EXPLAIN output to display parallel query execution plans.
A new `<gatherN>` pseudo-table row is added to traditional EXPLAIN, showing
the number of workers and the parallel-scanned table.

## Motivation

Users need visibility into whether a query is executed in parallel and which
table is being parallel-scanned. Without EXPLAIN support, debugging and
tuning parallel queries is difficult.

## Specification

### Target EXPLAIN Output

**With parallel query:**
```
| id | select_type | table     | type | key     | Extra                                    |
|----|-------------|-----------|------|---------|------------------------------------------|
|  1 | SIMPLE      | <gather1> | ALL  | NULL    | Parallel execute (4 workers, test.t1)    |
|  1 | SIMPLE      | t1        | range| idx_abc | Using where; Using index                 |
```

**Without parallel query:**
```
| id | select_type | table | type | key     | Extra                    |
|----|-------------|-------|------|---------|--------------------------|
|  1 | SIMPLE      | t1    | range| idx_abc | Using where; Using index |
```

### Parallel Physical Execution Plan Structure

```
thd->lex->unit
  -> select_lex
     -> join->qep_tab(tmp table)    // leader plan
        -> gather_operator
           -> m_template_join       // worker plan template
              -> qep_tabs           // original tables
```

### EXPLAIN Plan Generation

The parallel EXPLAIN adds gather node parsing to the standard EXPLAIN flow:

```c++
Explain_join::shallow_explain
  -> Explain_join::explain_qep_tab
     -> Explain::prepare_columns()
     -> if (tab->gather):     // NEW: parse gather node
        -> explain_pq_gather(tab->gather)
```

**Gather node explanation:**

```c++
Explain_join::explain_pq_gather(QEP_TAB *tab):
  JOIN *join = tab->gather->m_template_join;
  Explain_join *ej = new Explain_join(..., join->thd, ...);
  ej->shallow_explain();
```

### Extra Field Addition

A new `Extra_tag` is added for parallel execution:

```c++
enum Extra_tag {
  ...
  ET_PQ_PARALLEL,  // NEW: "Parallel execute (N workers, table)"
  ...
};

static const char *traditional_extra_tags[] = {
  ...
  "Parallel execute",  // ET_PQ_PARALLEL
  ...
};
```

The Extra column shows the number of workers and the parallel-scanned table
name.

### EXPLAIN Format Tree

For `EXPLAIN FORMAT=TREE`, the parallel execution plan is displayed
hierarchically, showing the ParallelScanIterator and PQblockScanIterator
nodes in the iterator tree.

## References

- `Explain_join::explain_pq_gather` - gather node explanation
- `ET_PQ_PARALLEL` - Extra tag for parallel execution
- `explain_query_specification` - standard EXPLAIN entry point
