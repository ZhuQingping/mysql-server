# Parallel Query - Worklog Specification Index

This directory contains worklog-style specification documents for the
Parallel Query (PQ) feature. Each document describes a sub-feature's design,
extracted from the PQ design document repository.

## Specification Documents

| # | Worklog ID | Title | Description |
|---|-----------|-------|-------------|
| 1 | WL#001 | [PQ Core Architecture](WL001-PQ-Core-Architecture.md) | Execution plan rewrite, leader/worker model, iterator framework, PQ eligibility rules |
| 2 | WL#002 | [PQ Data Partition & InnoDB Scan](WL002-PQ-Data-Partition-InnoDB-Scan.md) | B+tree partitioning, two-level split, reverse scan, read view sharing |
| 3 | WL#003 | [PQ Aggregation Parallel](WL003-PQ-Aggregation-Parallel.md) | COUNT/SUM/AVG/GROUP BY/HAVING, secondary aggregation, group reshuffle |
| 4 | WL#004 | [PQ Order By & Gather Merge](WL004-PQ-Order-By-Gather-Merge.md) | Worker sort, leader merge sort via binary heap |
| 5 | WL#005 | [PQ Hash Join](WL005-PQ-Hash-Join.md) | In-memory and on-disk parallel hash join strategies |
| 6 | WL#006 | [PQ Message Queue](WL006-PQ-Message-Queue.md) | Lock-free ring buffer, blocking send, non-blocking receive, batch update |
| 7 | WL#007 | [PQ Subquery & UNION Parallel](WL007-PQ-Subquery-UNION-Parallel.md) | UNION/UNION ALL, materialized derived tables, CTEs, temporary table sharing |
| 8 | WL#008 | [PQ Partition Table Support](WL008-PQ-Partition-Table.md) | Multi-partition parallel scan, ordered/unordered reading |
| 9 | WL#009 | [PQ EXPLAIN Support](WL009-PQ-EXPLAIN-Support.md) | EXPLAIN output for parallel execution plans |
| 10 | WL#010 | [PQ Hint & RBO Rules](WL010-PQ-Hint-RBO-Rules.md) | PQ/NO_PQ hints, eligibility rules, resource limits |
| 11 | WL#011 | [PQ Scalar Subquery Parallel](WL011-PQ-Scalar-Subquery-Parallel.md) | Independent scalar subquery in field_list/WHERE/HAVING/ORDER BY, result caching |
| 12 | WL#012 | [PQ Dependent-Ref Join](WL012-PQ-Dependent-Ref-Join-Dynamic-Partition.md) | Dynamic partition for ref key dependent tables, PQRefIterator |
| 13 | WL#013 | [PQ Semi/Anti/Outer Hash Join](WL013-PQ-Semi-Anti-Outer-Hash-Join.md) | Semi-join, anti-join, outer join parallel hash join strategies |
| 14 | WL#014 | [PQ Correlated Subquery & NST Clone](WL014-PQ-Correlated-Subquery-NST-Clone.md) | Nested Select_lex Tree cloning, correlated subquery parallel execution |
| 15 | WL#015 | [PQ Derived Table as Divided Table](WL015-PQ-Derived-Table-as-Divided-Table.md) | Parallel scan on materialized derived tables, views, CTEs |
| 16 | WL#016 | [PQ Secondary Index & ICP](WL016-PQ-Secondary-Index-ICP.md) | Secondary index parallel scan, Index Condition Pushdown, visibility check |
| 17 | WL#017 | [PQ Record Buffer & Prefetch](WL017-PQ-Record-Buffer-Prefetch-Optimization.md) | Record buffer optimization, read-ahead, virtual file descriptor pool |
| 18 | WL#018 | [PQ Semi-Join Mat, Thread Pool & Aggr Distinct](WL018-PQ-SemiJoin-Materialization-Thread-Pool-AggrDistinct.md) | Semi-join materialization, thread pool integration, aggr(DISTINCT) |

## Feature Coverage Map

The 18 spec documents cover all major PQ sub-features from the design
document repository. Here is the mapping from design documents to specs:

| Design Document | Spec(s) |
|----------------|---------|
| Parallel_query设计文档.md | WL#001 |
| Paralel-query-execute-plan-Design.md | WL#001 |
| Parallel-query-data-partition.md | WL#002 |
| Parallel-query-InnoDB-module.md | WL#002 |
| new-Parallel-query-InnoDB-module.md | WL#002 |
| InnoDB并行查询设计.md | WL#002 |
| InnoDB代码重构设计.md | WL#018 |
| 并行range-scan优化设计方案.md | WL#002 |
| 并行查询Range-Scan设计文档.md | WL#002 |
| 并行查询range-scan重构方案.md | WL#002 |
| 逆序扫描设计文档.md | WL#002 |
| 索引反序扫描调研.md | WL#002 |
| 并行查询执行计划.md | WL#001 |
| Parallel-query-of-Aggregation.md | WL#003 |
| aggr(group-by)并行执行.md | WL#003 |
| Aggr(distinct)并行设计.md | WL#018 |
| pq_group_reshuffle.md | WL#003 |
| group-aware-PQ设计.md | WL#003 |
| Order-by-parallel-execuction.md | WL#004 |
| order-by-并行执行.md | WL#004 |
| gather_merge归并排序.md | WL#004 |
| Parallel-hash-join-Design-(HLD).md | WL#005 |
| Parallel-hash-join-LLD.md | WL#005 |
| 多表Join并行.md | WL#005 |
| Implementation-of-message-queue-in-parallel-execution.md | WL#006 |
| 消息队列设计文档.md | WL#006 |
| Paritial-results-passing-in-parallel-execution.md | WL#006 |
| record[0]的优化方案.md | WL#006 |
| Parallel-query-of-UNION-Clause.md | WL#007 |
| UNION的并行实现.md | WL#007 |
| Parallel-execution-of-materialized-derived-table.md | WL#007 |
| Partition-table-PQ.md | WL#008 |
| 分区表多分区并行查询设计.md | WL#008 |
| 分区表的并行查询.md | WL#008 |
| 多表join并行查询innodb模块重构设计.md | WL#008 |
| Parallel-query-explain.md | WL#009 |
| explain并行查询.md | WL#009 |
| 并行查询explain-format=tree支持.md | WL#009 |
| Hint-support-for-Parallel-query.md | WL#010 |
| 并行hint实现.md | WL#010 |
| 优化规则RBO.md | WL#010 |
| 并发度策略.md | WL#010 |
| Parallel-scalar-subquery.md | WL#011 |
| PQ-Depend-ref-join-V-3.0.md | WL#012 |
| SemiJoin-Materialized-Exec-Doc.md | WL#018 |
| SemiJoin-materialized-PQ.md | WL#018 |
| Parallel-semi-anti-outer-hash-join.md | WL#013 |
| PQ-support-for-correlated-subquery.md | WL#014 |
| Design-for-dividing-derived-table-in-PQ.md | WL#015 |
| 二级索引查询.md | WL#016 |
| 二级索引ICP.md | WL#016 |
| 二级索引查询回表后的compare函数.md | WL#016 |
| 并行查询使用Record_buffer.md | WL#017 |
| 预读功能设计文档.md | WL#017 |
| 虚拟文件句柄.md | WL#017 |
| Quick-Select-深拷贝设计.md | WL#017 |
| worker记录扫描函数.md | WL#017 |
| 线程池.md | WL#018 |
| 并行查询拆分表动态切分数据方案.md | WL#012 |
| PQ-subquery-plan.md | WL#014 |
| Design/LLD/执行计划改写.md | WL#001 |
| 8.0.20-PQ移植总结.md | WL#001 |
| 并行执行使用说明.md | WL#010 |

## Source Documents

These specifications were extracted from the PQ design document repository
at `/workdir/zhuqingping/Code/parallel_query/parallel-query-document/`.
