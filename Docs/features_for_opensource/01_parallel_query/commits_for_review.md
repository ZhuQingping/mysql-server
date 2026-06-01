# Parallel Query (PQ) Feature - Commit List for Review

> Generated: 2026-05-23
> Main commit: dd979fe3406d783ae5b65b633c24a807ddd9f17c
> Branch: taurusondstore_develop

## Overview

The following commits are identified as PQ-related, specifically modifying
`sql/parallel_query/` code, fixing PQ-specific bugs, or updating PQ test
cases. DStore-specific parallel features (ha_cde_parallel, aggregation
pushdown, parallel COUNT for DStore) are excluded as separate features.

---

## Commit List (ordered by merge time / committer date)

### #1 - Main PQ feature commit
| Field | Value |
|-------|-------|
| **Hash** | dd979fe3406d783ae5b65b633c24a807ddd9f17c |
| **Merge date** | 2025-12-19 |
| **Author** | zhuqingping |
| **Subject** | FEATURE Support Parallel Query for InnoDB table |
| **Files changed** | 1036 files, +1,520,268 / -3,379 |
| **Scope** | Full PQ feature: optimizer, executor, iterators, clone, exchange, hash join, test suite |
| **Patch** | `parallel_query-0001-support-parallel-query-for-InnoDB-table.patch` (85M) |

### #2
| Field | Value |
|-------|-------|
| **Hash** | ebc03e4964dcf99213595bb81f55551a11cd0556 |
| **Merge date** | 2025-12-23 |
| **Author** | zhuqingping |
| **Subject** | BUGFIX Fix mtr test cases for Dstore engine |
| **Scope** | Fix mtr test failures when Dstore is default engine (includes PQ tests) |
| **Patch** | `parallel_query-0002-fix-mtr-test-cases-for-Dstore-engine.patch` (439K) |

### #3
| Field | Value |
|-------|-------|
| **Hash** | 2b7bdc4ebe3bfcf4c214328707c8766b0985f03a |
| **Merge date** | 2025-12-26 |
| **Author** | wangjin |
| **Subject** | BUGFIX fix PQ review comments |
| **Scope** | PQ code review fixes |
| **Patch** | `parallel_query-0003-fix-PQ-review-comments.patch` (11K) |

### #4
| Field | Value |
|-------|-------|
| **Hash** | 99f7c96b7c510f11b18dbc432601e7fa82eab876 |
| **Merge date** | 2025-12-29 |
| **Author** | zhuqingping |
| **Subject** | BUGFIX Assert failure in function CDE::SingleStmtTrxCommit for parallel query on Dstore table scenario |
| **Scope** | Fix assert failure in DStore parallel query commit flow |
| **Patch** | `parallel_query-0004-fix-assert-failure-in-SingleStmtTrxCommit.patch` (19K) |

### #5
| Field | Value |
|-------|-------|
| **Hash** | e0c08b8be24e0f40352015e035761f9bf3aecbc7 |
| **Merge date** | 2025-12-31 |
| **Author** | wangyunhua |
| **Subject** | FEATURE Add test cases for BIT data type in parallel query |
| **Scope** | Add BIT type test coverage for PQ |
| **Patch** | `parallel_query-0005-add-BIT-data-type-test-cases.patch` (28K) |

### #6
| Field | Value |
|-------|-------|
| **Hash** | ed4274c30ff090f711131a75d41a6ad69a6a5786 |
| **Merge date** | 2026-01-13 |
| **Author** | wangjin |
| **Subject** | BUGFIX fix PQ review comments |
| **Scope** | PQ code review fixes (continued) |
| **Patch** | `parallel_query-0006-fix-PQ-review-comments-continued.patch` (30K) |

### #7
| Field | Value |
|-------|-------|
| **Hash** | a0be857b2f2aee286a683768742d857bfc768d72 |
| **Merge date** | 2026-01-23 |
| **Author** | zhuqingping |
| **Subject** | BUGFIX Optimize parallel query code |
| **Scope** | PQ code optimization |
| **Patch** | `parallel_query-0007-optimize-parallel-query-code.patch` (49K) |

### #8
| Field | Value |
|-------|-------|
| **Hash** | 0b9de0fc8c87412f399c2761ff39a189216288d1 |
| **Merge date** | 2026-02-13 |
| **Author** | zhuqingping |
| **Subject** | BUGFIX: PQ Crash in pq_clone_sj_mat_exec for PQ with Semi Join |
| **Scope** | Fix crash in PQ semi-join materialization clone |
| **Patch** | `parallel_query-0008-fix-crash-in-pq_clone_sj_mat_exec-for-semi-join.patch` (16K) |

### #9
| Field | Value |
|-------|-------|
| **Hash** | d27edeefadd3d66f3b2bba9f3aba4044b2f1bb3e |
| **Merge date** | 2026-03-19 |
| **Author** | Guilhem Bichot |
| **Subject** | BUGFIX BUGFIX BUGFIX Updating the PQ-specific result file following the fixes for these bugs |
| **Scope** | Update PQ test result files after related bug fixes |
| **Patch** | `parallel_query-0009-update-PQ-result-files.patch` (12K) |

---

## Commits EXCLUDED (not PQ feature)

### DStore parallel features (separate from PQ)

| Hash | Subject | Reason |
|------|---------|--------|
| 5421145f543 | Add parallel COUNT(*) support for DSTORE storage engine | DStore ha_cde_parallel feature, not sql/parallel_query |
| 3ff20621b45 | Disable parallel execution for COUNT(*) on temporary tables | DStore ha_cde_parallel fix, not sql/parallel_query |
| 01def68bf3f | Fix crash on parallel COUNT worker creation failure and KILL signal | DStore ha_cde_parallel fix, not sql/parallel_query |
| 2f5cfa99808 | Support aggregation pushdown | DStore storage engine optimization, not PQ |

### Other parallel-related but not PQ

| Hash | Subject | Reason |
|------|---------|--------|
| 880a06960b2 | flush logs and parallel executed non automic ddl cause gtid gap | Binlog/GTID fix; "parallel" = parallel DDL, not PQ |
| 5153877e77e | Supports parallel build heap relation | DStore DDL (BuildParallel), not query parallelism |
| b20ae90d1e5 | Fix BuildParallel not interrupted on row log failure | DStore DDL fix, not PQ |
| 82e8f716331 | Support heap/index tuples comparison for online ddl | DStore DDL feature, not PQ |
| 633aa5a0b88 | Mysqld crashes during instant column addition at CDE::DDCopyColumnPrivate | General DStore fix, not PQ-specific |
| 627c8eb3afc | Mysqld crashes during instant column addition at CDE::DDCopyColumnPrivate | Same as above |
| 2ddb2983573 | MEM_ROOT::ClearForReuse crash during pquery pressure test | General sql_admin.cc fix, not PQ-specific |
| 2addb3f678d | Resolve errors during parallel creation of indexes | DStore DDL parallel index creation, not PQ |
| fc62a87bd28 | Support inplace add index has prefix column or virtual column | DStore DDL feature, not PQ |
| 47cf3d92d3c | Add recovery/restore parallelized page reading guc | XLog/recovery feature, not PQ |
| 17360b67cd2 | HashJoinChunk crash during LEX::cleanup | Hash join cleanup crash, not PQ-specific |
| 2d32e008f27 | Statement Outline | Separate feature, not PQ |
| 6f4143ea0aa | Add perfcounter for inplace rebuild table | DStore DDL, not PQ |
| 53d293bb8de | Fix data loss due to instance shutdown during rebuild table | DStore rebuild fix, not PQ |
| aa2dbbc5a0a | Fix can not found referenced index after rename column | DStore DDL fix, not PQ |

### Not on current branch (taurusondstore_develop)

| Hash | Branch | Subject | Notes |
|------|--------|---------|-------|
| 0f9a0c9487e | develop only | Fix PQ partition-blob + subquery_to_derived | PQ fix but only on develop, not merged into taurusondstore_develop |

---

## Summary

| Category | Count |
|----------|-------|
| **Total PQ-related (on current branch)** | **9** (1 main + 8 follow-up) |

### Apply patches in order:

```bash
git am patches/parallel_query-0001-support-parallel-query-for-InnoDB-table.patch
git am patches/parallel_query-0002-fix-mtr-test-cases-for-Dstore-engine.patch
git am patches/parallel_query-0003-fix-PQ-review-comments.patch
git am patches/parallel_query-0004-fix-assert-failure-in-SingleStmtTrxCommit.patch
git am patches/parallel_query-0005-add-BIT-data-type-test-cases.patch
git am patches/parallel_query-0006-fix-PQ-review-comments-continued.patch
git am patches/parallel_query-0007-optimize-parallel-query-code.patch
git am patches/parallel_query-0008-fix-crash-in-pq_clone_sj_mat_exec-for-semi-join.patch
git am patches/parallel_query-0009-update-PQ-result-files.patch

# Or apply all at once:
# git am patches/parallel_query-*.patch
```

### Sensitive information to sanitize:

| Type | Example | Where |
|------|---------|-------|
| Internal issue IDs | FEATURE, BUGFIX | Commit messages, source code comments |
| Internal Git URLs | codehub-g.huawei.com, clouddevops.huawei.com | PQ main commit message |
| Employee IDs | (removed from documents and patches) | Git author/committer fields |
| Internal email | @huawei.com | Git author/committer fields |
| Internal product names | TaurusDB, GaussDB | Commit messages, code comments |
